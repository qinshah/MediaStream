#include "rtmp_client.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/bounded_queue.h"
#include "../common/logger.h"
#include "amf.h"
#include "flv_packager.h"

namespace media_stream {

// ErrorCode 与 Index.d.ts 对齐
static constexpr int kErrRtmpUrlInvalid = 7;
static constexpr int kErrRtmpConnectFail = 8;
static constexpr int kErrRtmpTimeout = 9;

static constexpr int kMaxReconnect = 3;
static constexpr int kConnectTimeoutMs = 5000;
static constexpr size_t kVideoQueueCap = 300;
static constexpr size_t kAudioQueueCap = 300;
static constexpr size_t kControlQueueCap = 64;

// RTMP message type
static constexpr uint8_t kMsgSetChunkSize = 1;
static constexpr uint8_t kMsgAudio = 8;
static constexpr uint8_t kMsgVideo = 9;
static constexpr uint8_t kMsgDataAmf0 = 18;   // @setDataFrame
static constexpr uint8_t kMsgCommandAmf0 = 20;

RtmpClient::~RtmpClient() {
    Stop();
}

bool RtmpClient::ParseUrl(const std::string &url, RtmpUrl &out) {
    // rtmp://host[:port]/app/stream
    static const std::string kPrefix = "rtmp://";
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    std::string rest = url.substr(kPrefix.size());
    size_t slash = rest.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    std::string hostPort = rest.substr(0, slash);
    std::string path = rest.substr(slash + 1);
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos || lastSlash + 1 >= path.size()) {
        return false;
    }
    out.app = path.substr(0, lastSlash);
    out.stream = path.substr(lastSlash + 1);
    size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        out.host = hostPort.substr(0, colon);
        out.port = atoi(hostPort.substr(colon + 1).c_str());
        if (out.port <= 0 || out.port > 65535) {
            return false;
        }
    } else {
        out.host = hostPort;
        out.port = 1935;
    }
    out.valid = !out.host.empty() && !out.app.empty() && !out.stream.empty();
    return out.valid;
}

bool RtmpClient::Start(const std::string &url, Callbacks callbacks) {
    if (state_.load() == State::kConnecting || state_.load() == State::kStreaming ||
        state_.load() == State::kReconnecting) {
        MS_LOG_WARN("RtmpClient already active, ignore Start");
        return true;
    }
    if (!ParseUrl(url, url_)) {
        MS_LOG_ERROR("invalid rtmp url: %{public}s", url.c_str());
        if (callbacks.onError) {
            callbacks.onError(kErrRtmpUrlInvalid, "RTMP 地址格式错误（应为 rtmp://host/app/stream）");
        }
        return false;
    }
    callbacks_ = std::move(callbacks);
    tcUrl_ = "rtmp://" + url_.host + (url_.port != 1935 ? ":" + std::to_string(url_.port) : "") + "/" +
             url_.app;
    stopRequested_ = false;
    sentBytes_ = 0;
    droppedVideoFrames_ = 0;
    basePtsUs_ = -1;

    videoQueue_ = std::make_unique<BoundedQueue<OutMessage>>(kVideoQueueCap, OverflowPolicy::kDropNewest);
    audioQueue_ = std::make_unique<BoundedQueue<OutMessage>>(kAudioQueueCap, OverflowPolicy::kBlockOnFull);
    controlQueue_ = std::make_unique<BoundedQueue<OutMessage>>(kControlQueueCap, OverflowPolicy::kDropNewest);

    thread_ = std::thread(&RtmpClient::ThreadMain, this);
    return true;
}

void RtmpClient::Stop() {
    bool wasActive = state_.load() != State::kIdle && state_.load() != State::kStopped &&
                     state_.load() != State::kError;
    stopRequested_ = true;
    // 唤醒可能阻塞的队列与命令等待
    if (videoQueue_) videoQueue_->Close();
    if (audioQueue_) audioQueue_->Close();
    if (controlQueue_) controlQueue_->Close();
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        sessionDead_ = true;
        cmdCv_.notify_all();
    }
    if (socketFd_ >= 0) {
        shutdown(socketFd_, SHUT_RDWR);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (wasActive) {
        SetState(State::kStopped);
    }
    MS_LOG_INFO("RtmpClient stopped");
}

void RtmpClient::SetVideoConfig(const std::vector<uint8_t> &avcC) {
    std::lock_guard<std::mutex> lock(configMutex_);
    avcC_ = avcC;
}

void RtmpClient::SetAudioConfig(const std::vector<uint8_t> &asc) {
    std::lock_guard<std::mutex> lock(configMutex_);
    asc_ = asc;
}

void RtmpClient::SetMetaData(int width, int height, int fps, int videoBitrateKbps) {
    std::lock_guard<std::mutex> lock(configMutex_);
    metaData_ = FlvPackager::BuildMetaData(width, height, fps, videoBitrateKbps);
    hasMeta_ = true;
}

int64_t RtmpClient::PtsUsToMs(int64_t ptsUs) {
    int64_t base = basePtsUs_.load();
    if (base < 0) {
        basePtsUs_.compare_exchange_strong(base, ptsUs);
        base = ptsUs;
    }
    int64_t ms = (ptsUs - base) / 1000;
    return ms < 0 ? 0 : ms;
}

void RtmpClient::SendVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe) {
    if (state_.load() != State::kStreaming && state_.load() != State::kReconnecting) {
        return;
    }
    OutMessage msg;
    msg.payload = FlvPackager::BuildVideoTag(data, size, isKeyframe);
    msg.timestampMs = static_cast<uint32_t>(PtsUsToMs(ptsUs));
    msg.typeId = kMsgVideo;
    msg.msgStreamId = streamId_;
    msg.chunkStreamId = 6;
    if (!videoQueue_->Push(std::move(msg))) {
        droppedVideoFrames_++;
    }
}

void RtmpClient::SendAudio(const uint8_t *data, int32_t size, int64_t ptsUs) {
    if (state_.load() != State::kStreaming && state_.load() != State::kReconnecting) {
        return;
    }
    OutMessage msg;
    msg.payload = FlvPackager::BuildAudioTag(data, size);
    msg.timestampMs = static_cast<uint32_t>(PtsUsToMs(ptsUs));
    msg.typeId = kMsgAudio;
    msg.msgStreamId = streamId_;
    msg.chunkStreamId = 4;
    audioQueue_->Push(std::move(msg)); // 阻塞策略：音频不丢
}

void RtmpClient::SetState(State state, int reconnectAttempt) {
    state_.store(state);
    if (callbacks_.onState) {
        callbacks_.onState(state, reconnectAttempt);
    }
}

void RtmpClient::FailSession(int errorCode, const std::string &message) {
    SetState(State::kError);
    if (callbacks_.onError) {
        callbacks_.onError(errorCode, message);
    }
}

// —— 网络线程 ——

void RtmpClient::ThreadMain() {
    int attempt = 0;
    while (!stopRequested_.load()) {
        SetState(attempt == 0 ? State::kConnecting : State::kReconnecting, attempt);
        if (RunSession()) {
            // 正常返回（不应发生：RunSession 仅在连接断开或停止时返回）
            break;
        }
        if (stopRequested_.load()) {
            break;
        }
        attempt++;
        if (attempt > kMaxReconnect) {
            FailSession(kErrRtmpConnectFail, "网络异常，推流已停止（重连 3 次失败）");
            return;
        }
        // 指数退避 1s/2s/4s
        int backoffMs = 1000 << (attempt - 1);
        MS_LOG_WARN("rtmp reconnect attempt %{public}d after %{public}dms", attempt, backoffMs);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoffMs);
        while (!stopRequested_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

bool RtmpClient::RunSession() {
    sessionDead_ = false;
    publishStarted_ = false;
    streamId_ = 0;
    txnCounter_ = 1;
    inChunkSize_ = 128;
    for (auto &c : chunkIn_) {
        c = ChunkIn();
    }

    if (!ConnectSocket() || !Handshake() || !SendConnectCommand() || !SendCreateStreamCommand() ||
        !SendPublishCommand()) {
        if (socketFd_ >= 0) {
            close(socketFd_);
            socketFd_ = -1;
        }
        return false;
    }

    // publish 成功：发送 metadata + sequence header
    EnqueueConfigMessages();
    SetState(State::kStreaming);

    // 推流循环：接收（onStatus 监控）+ 发送（队列出队）
    while (!stopRequested_.load()) {
        // 非阻塞收包（10ms 轮询）
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(socketFd_, &readFds);
        struct timeval tv = {0, 10000};
        int sel = select(socketFd_ + 1, &readFds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            MS_LOG_WARN("socket select error %{public}d", errno);
            break;
        }
        if (sel > 0 && FD_ISSET(socketFd_, &readFds)) {
            if (!RecvMessage()) {
                MS_LOG_WARN("socket recv failed / closed");
                break;
            }
        }

        // 发送：control 优先，audio 次之，video 最后
        OutMessage msg;
        bool hasMsg = false;
        if (controlQueue_ && controlQueue_->Size() > 0) {
            hasMsg = controlQueue_->Pop(msg);
        } else if (audioQueue_ && audioQueue_->Size() > 0) {
            hasMsg = audioQueue_->Pop(msg);
        } else if (videoQueue_ && videoQueue_->Size() > 0) {
            hasMsg = videoQueue_->Pop(msg);
        }
        if (hasMsg && !SendMessage(msg)) {
            MS_LOG_WARN("socket send failed");
            break;
        }
    }

    if (socketFd_ >= 0) {
        close(socketFd_);
        socketFd_ = -1;
    }
    return false; // 到达此处均为连接断开
}

bool RtmpClient::ConnectSocket() {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    std::string portStr = std::to_string(url_.port);
    if (getaddrinfo(url_.host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
        MS_LOG_ERROR("getaddrinfo failed for %{public}s", url_.host.c_str());
        return false;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return false;
    }
    // 非阻塞连接 + select 超时
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    if (rc < 0) {
        fd_set writeFds;
        FD_ZERO(&writeFds);
        FD_SET(fd, &writeFds);
        struct timeval tv = {kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000};
        if (select(fd + 1, nullptr, &writeFds, nullptr, &tv) <= 0) {
            close(fd);
            return false;
        }
        int soError = 0;
        socklen_t len = sizeof(soError);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        if (soError != 0) {
            close(fd);
            return false;
        }
    }
    // 恢复阻塞模式（发送带 MSG_NOSIGNAL）；置 TCP_NODELAY 降低延迟
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    socketFd_ = fd;
    return true;
}

bool RtmpClient::RecvRaw(uint8_t *data, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = recv(socketFd_, data + got, size - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false; // 0=对端关闭，<0=错误
    }
    return true;
}

bool RtmpClient::SendRaw(const uint8_t *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(socketFd_, data + sent, size - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            sentBytes_ += n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool RtmpClient::Handshake() {
    // simple handshake：C0(0x03) + C1(1536 随机) → 收 S0+S1+S2(3073) → 发 C2
    uint8_t c0c1[1537];
    c0c1[0] = 0x03; // RTMP version
    // C1: time(4) + zero(4) + random(1528)
    uint32_t t = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    c0c1[1] = static_cast<uint8_t>((t >> 24) & 0xFF);
    c0c1[2] = static_cast<uint8_t>((t >> 16) & 0xFF);
    c0c1[3] = static_cast<uint8_t>((t >> 8) & 0xFF);
    c0c1[4] = static_cast<uint8_t>(t & 0xFF);
    memset(c0c1 + 5, 0, 4);
    for (int i = 9; i < 1537; i++) {
        c0c1[i] = static_cast<uint8_t>(rand() & 0xFF);
    }
    if (!SendRaw(c0c1, sizeof(c0c1))) {
        return false;
    }
    uint8_t s[3073];
    if (!RecvRaw(s, sizeof(s))) {
        return false;
    }
    if (s[0] != 0x03) {
        MS_LOG_ERROR("rtmp handshake: unexpected S0 version %{public}d", s[0]);
        return false;
    }
    // C2 = S1 回显（time + time2 + random echo）
    if (!SendRaw(s + 1, 1536)) {
        return false;
    }
    MS_LOG_INFO("rtmp handshake done");
    return true;
}

bool RtmpClient::SendMessage(const OutMessage &msg) {
    // fmt0 首块 + fmt3 续块
    uint32_t ts = msg.timestampMs;
    bool extendedTs = ts >= 0xFFFFFF;
    uint8_t header[16];
    size_t hlen = 0;
    header[hlen++] = static_cast<uint8_t>((0 << 6) | (msg.chunkStreamId & 0x3F));
    uint32_t tsField = extendedTs ? 0xFFFFFF : ts;
    header[hlen++] = static_cast<uint8_t>((tsField >> 16) & 0xFF);
    header[hlen++] = static_cast<uint8_t>((tsField >> 8) & 0xFF);
    header[hlen++] = static_cast<uint8_t>(tsField & 0xFF);
    uint32_t len = static_cast<uint32_t>(msg.payload.size());
    header[hlen++] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[hlen++] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[hlen++] = static_cast<uint8_t>(len & 0xFF);
    header[hlen++] = msg.typeId;
    // message stream id 小端
    header[hlen++] = static_cast<uint8_t>(msg.msgStreamId & 0xFF);
    header[hlen++] = static_cast<uint8_t>((msg.msgStreamId >> 8) & 0xFF);
    header[hlen++] = static_cast<uint8_t>((msg.msgStreamId >> 16) & 0xFF);
    header[hlen++] = static_cast<uint8_t>((msg.msgStreamId >> 24) & 0xFF);
    if (extendedTs) {
        header[hlen++] = static_cast<uint8_t>((ts >> 24) & 0xFF);
        header[hlen++] = static_cast<uint8_t>((ts >> 16) & 0xFF);
        header[hlen++] = static_cast<uint8_t>((ts >> 8) & 0xFF);
        header[hlen++] = static_cast<uint8_t>(ts & 0xFF);
    }
    if (!SendRaw(header, hlen)) {
        return false;
    }
    // 分块
    size_t offset = 0;
    while (offset < msg.payload.size()) {
        size_t chunk = msg.payload.size() - offset;
        if (chunk > outChunkSize_) {
            chunk = outChunkSize_;
        }
        if (offset > 0) {
            // fmt3 续块头（含 extended timestamp）
            uint8_t cont[5];
            size_t clen = 0;
            cont[clen++] = static_cast<uint8_t>((3 << 6) | (msg.chunkStreamId & 0x3F));
            if (extendedTs) {
                cont[clen++] = static_cast<uint8_t>((ts >> 24) & 0xFF);
                cont[clen++] = static_cast<uint8_t>((ts >> 16) & 0xFF);
                cont[clen++] = static_cast<uint8_t>((ts >> 8) & 0xFF);
                cont[clen++] = static_cast<uint8_t>(ts & 0xFF);
            }
            if (!SendRaw(cont, clen)) {
                return false;
            }
        }
        if (!SendRaw(msg.payload.data() + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

// —— 命令发送与响应等待 ——

void RtmpClient::WriteCommandMessage(OutMessage &msg, const std::vector<uint8_t> &amfPayload) {
    msg.payload = amfPayload;
    msg.timestampMs = 0;
    msg.typeId = kMsgCommandAmf0;
    msg.msgStreamId = 0;
    msg.chunkStreamId = 3;
}

bool RtmpClient::SendConnectCommand() {
    AmfWriter w;
    w.WriteString("connect");
    w.WriteNumber(txnCounter_++);
    w.WriteObjectBegin();
    AmfWriter props;
    props.WriteObjectProperty("app", url_.app);
    props.WriteObjectProperty("tcUrl", tcUrl_);
    props.WriteObjectProperty("type", "nonprivate");
    props.WriteObjectProperty("flashVer", "FMLE/3.0");
    props.WriteObjectPropertyNumber("fpad", 0);
    props.WriteObjectEnd();
    w.Append(props.Data());

    OutMessage msg;
    WriteCommandMessage(msg, w.Data());
    if (!SendMessage(msg)) {
        return false;
    }
    // 等 _result
    std::unique_lock<std::mutex> lock(cmdMutex_);
    int myTxn = txnCounter_ - 1;
    bool ok = cmdCv_.wait_for(lock, std::chrono::milliseconds(kConnectTimeoutMs),
                              [&] { return pendingTxnResult_ == myTxn || sessionDead_; });
    return ok && !sessionDead_;
}

bool RtmpClient::SendCreateStreamCommand() {
    AmfWriter w;
    w.WriteString("createStream");
    w.WriteNumber(txnCounter_++);
    w.WriteNull();
    OutMessage msg;
    WriteCommandMessage(msg, w.Data());
    if (!SendMessage(msg)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(cmdMutex_);
    int myTxn = txnCounter_ - 1;
    bool ok = cmdCv_.wait_for(lock, std::chrono::milliseconds(kConnectTimeoutMs),
                              [&] { return pendingTxnResult_ == myTxn || sessionDead_; });
    return ok && !sessionDead_ && streamId_ != 0;
}

bool RtmpClient::SendPublishCommand() {
    AmfWriter w;
    w.WriteString("publish");
    w.WriteNumber(0); // publish 无事务
    w.WriteNull();
    w.WriteString(url_.stream);
    w.WriteString("live");
    OutMessage msg;
    WriteCommandMessage(msg, w.Data());
    msg.msgStreamId = streamId_;
    msg.chunkStreamId = 8;
    if (!SendMessage(msg)) {
        return false;
    }
    // 等 onStatus(NetStream.Publish.Start)
    std::unique_lock<std::mutex> lock(cmdMutex_);
    bool ok = cmdCv_.wait_for(lock, std::chrono::milliseconds(kConnectTimeoutMs),
                              [&] { return publishStarted_ || sessionDead_; });
    return ok && !sessionDead_;
}

// —— 接收解析 ——

bool RtmpClient::RecvMessage() {
    // 读 basic header
    uint8_t b0 = 0;
    if (!RecvRaw(&b0, 1)) {
        return false;
    }
    int fmt = (b0 >> 6) & 0x03;
    int csid = b0 & 0x3F;
    if (csid == 0) {
        uint8_t b1 = 0;
        if (!RecvRaw(&b1, 1)) return false;
        csid = 64 + b1;
    } else if (csid == 1) {
        uint8_t b12[2] = {0};
        if (!RecvRaw(b12, 2)) return false;
        csid = 64 + b12[0] + b12[1] * 256;
    }
    if (csid >= kMaxChunkStreams) {
        return true; // 忽略不支持的 csid（本客户端只用低 csid）
    }
    ChunkIn &ch = chunkIn_[csid];

    uint32_t tsOrDelta = 0;
    if (fmt == 0) {
        uint8_t mh[11];
        if (!RecvRaw(mh, 11)) return false;
        tsOrDelta = (mh[0] << 16) | (mh[1] << 8) | mh[2];
        ch.msgLen = (mh[3] << 16) | (mh[4] << 8) | mh[5];
        ch.typeId = mh[6];
        ch.msgStreamId = mh[7] | (mh[8] << 8) | (mh[9] << 16) | (mh[10] << 24);
        ch.timestamp = tsOrDelta;
        ch.payload.clear();
        ch.headerValid = true;
    } else if (fmt == 1) {
        uint8_t mh[7];
        if (!RecvRaw(mh, 7)) return false;
        tsOrDelta = (mh[0] << 16) | (mh[1] << 8) | mh[2];
        ch.msgLen = (mh[3] << 16) | (mh[4] << 8) | mh[5];
        ch.typeId = mh[6];
        ch.timestamp += tsOrDelta;
        ch.payload.clear();
        ch.headerValid = true;
    } else if (fmt == 2) {
        uint8_t mh[3];
        if (!RecvRaw(mh, 3)) return false;
        tsOrDelta = (mh[0] << 16) | (mh[1] << 8) | mh[2];
        ch.timestamp += tsOrDelta;
        ch.payload.clear();
    }
    // fmt==3：沿用之前头部
    if (!ch.headerValid) {
        return true;
    }
    if (tsOrDelta == 0xFFFFFF && fmt <= 2) {
        uint8_t ext[4];
        if (!RecvRaw(ext, 4)) return false;
        uint32_t extTs = (ext[0] << 24) | (ext[1] << 16) | (ext[2] << 8) | ext[3];
        if (fmt == 0) {
            ch.timestamp = extTs;
        } else {
            ch.timestamp += extTs - 0xFFFFFF; // 近似处理
        }
    }
    // 读消息体（可能被 chunk 分割：一次读 min(剩余, inChunkSize)）
    size_t need = ch.msgLen - ch.payload.size();
    size_t thisChunk = need < inChunkSize_ ? need : inChunkSize_;
    size_t oldSize = ch.payload.size();
    ch.payload.resize(oldSize + thisChunk);
    if (!RecvRaw(ch.payload.data() + oldSize, thisChunk)) {
        return false;
    }
    if (ch.payload.size() < ch.msgLen) {
        return true; // 消息未完，等下一 chunk
    }

    // 完整消息
    if (ch.typeId == kMsgCommandAmf0) {
        HandleCommandMessage(ch.payload.data(), ch.payload.size());
    } else {
        HandleProtocolControl(ch.typeId, ch.payload.data(), ch.payload.size());
    }
    return true;
}

bool RtmpClient::HandleProtocolControl(uint8_t typeId, const uint8_t *data, size_t size) {
    if (typeId == kMsgSetChunkSize && size >= 4) {
        inChunkSize_ = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
        inChunkSize_ &= 0x7FFFFFFF;
    }
    // Window Acknowledgement / Set Peer Bandwidth 等忽略即可（simple 模式）
    return true;
}

bool RtmpClient::HandleCommandMessage(const uint8_t *data, size_t size) {
    AmfReader reader(data, size);
    std::string cmd;
    double num = 0;
    if (reader.ReadValue(cmd, num) != 0x02) {
        return true;
    }
    if (cmd == "_result") {
        std::string s;
        double txn = 0;
        reader.ReadValue(s, txn); // txn id
        reader.SkipValue();       // command object / null
        if (static_cast<int>(txn) == 2 || streamId_ == 0) {
            // createStream 的 _result 携带 stream id
            std::string s2;
            double sid = 0;
            if (!reader.Eof() && reader.ReadValue(s2, sid) == 0x00 && sid > 0) {
                streamId_ = static_cast<uint32_t>(sid);
            }
        }
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pendingTxnResult_ = static_cast<int>(txn);
        cmdCv_.notify_all();
    } else if (cmd == "onStatus") {
        std::string s;
        double txn = 0;
        reader.ReadValue(s, txn); // txn=0
        reader.SkipValue();       // null
        std::string info;
        double d = 0;
        if (reader.ReadValue(info, d) == 0x03) {
            // info 对象扁平解析后最后一个字符串值（level/code 之一）
            if (info.find("NetStream.Publish.Start") != std::string::npos) {
                std::lock_guard<std::mutex> lock(cmdMutex_);
                publishStarted_ = true;
                cmdCv_.notify_all();
            } else if (info.find("Failed") != std::string::npos ||
                       info.find("NetStream.Publish.BadName") != std::string::npos) {
                sessionDead_ = true;
                cmdCv_.notify_all();
            }
        }
    } else if (cmd == "_error") {
        sessionDead_ = true;
        cmdCv_.notify_all();
    }
    return true;
}

void RtmpClient::EnqueueConfigMessages() {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (hasMeta_ && !metaData_.empty()) {
        OutMessage msg;
        msg.payload = metaData_;
        msg.timestampMs = 0;
        msg.typeId = kMsgDataAmf0;
        msg.msgStreamId = streamId_;
        msg.chunkStreamId = 5;
        controlQueue_->Push(std::move(msg));
    }
    if (!avcC_.empty()) {
        OutMessage msg;
        msg.payload = FlvPackager::BuildVideoSequenceHeader(avcC_);
        msg.timestampMs = 0;
        msg.typeId = kMsgVideo;
        msg.msgStreamId = streamId_;
        msg.chunkStreamId = 6;
        controlQueue_->Push(std::move(msg));
    }
    if (!asc_.empty()) {
        OutMessage msg;
        msg.payload = FlvPackager::BuildAudioSequenceHeader(asc_);
        msg.timestampMs = 0;
        msg.typeId = kMsgAudio;
        msg.msgStreamId = streamId_;
        msg.chunkStreamId = 4;
        controlQueue_->Push(std::move(msg));
    }
}

} // namespace media_stream
