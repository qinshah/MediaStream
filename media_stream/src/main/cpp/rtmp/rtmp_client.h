#ifndef MEDIA_STREAM_RTMP_CLIENT_H
#define MEDIA_STREAM_RTMP_CLIENT_H

// 自研最小 RTMP 推流客户端（零三方依赖，simple handshake + AMF0 + FLV tag）
// 协议路径（参考 Harmony-OBS-Studio/plugins/obs-outputs/librtmp 行为）：
//   TCP 连接(带超时) → simple handshake(C0C1/S0S1S2/C2) → connect(app) → _result
//   → createStream → _result(streamId) → publish(stream,'live') → onStatus(Publish.Start)
//   → @setDataFrame onMetaData → video/audio sequence header → FLV tag 消息流
// 断线重连：指数退避 1s/2s/4s，最多 3 次；重连成功后重发 metadata + sequence header
// 背压：视频队列满丢新帧（计 droppedVideoFrames），音频队列独立（不丢音频）

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/bounded_queue.h"

namespace media_stream {

template <typename T>
using MediaQueue = BoundedQueue<T>;

class RtmpClient {
public:
    enum class State {
        kIdle,
        kConnecting,
        kStreaming,
        kReconnecting,
        kStopped,
        kError,
    };

    struct Callbacks {
        // 状态变化（reconnectAttempt 仅 kReconnecting 时有意义，从 1 起）
        std::function<void(State state, int reconnectAttempt)> onState;
        // 致命错误（重连耗尽 / URL 非法 / 连接失败）：code 对应 ErrorCode
        std::function<void(int errorCode, const std::string &message)> onError;
    };

    RtmpClient() = default;
    ~RtmpClient();

    // 启动推流线程；URL 非法返回 false（不同步阻塞连接）
    bool Start(const std::string &url, Callbacks callbacks);
    // 停止（幂等）；重连中亦安全
    void Stop();

    // 编码参数（连接建立后发送；重连自动重发）
    void SetVideoConfig(const std::vector<uint8_t> &avcC);
    void SetAudioConfig(const std::vector<uint8_t> &asc);
    void SetMetaData(int width, int height, int fps, int videoBitrateKbps);

    // 媒体数据投递（ptsUs 微秒；内部维护会话起始 epoch 转 FLV ms）
    void SendVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe);
    void SendAudio(const uint8_t *data, int32_t size, int64_t ptsUs);

    bool IsStreaming() const { return state_.load() == State::kStreaming; }
    int64_t SentBytes() const { return sentBytes_.load(); }
    int64_t DroppedVideoFrames() const { return droppedVideoFrames_.load(); }
    int64_t PtsUsToMs(int64_t ptsUs); // 会话内 pts 转 FLV 毫秒

private:
    struct RtmpUrl {
        std::string host;
        int port = 1935;
        std::string app;
        std::string stream;
        bool valid = false;
    };

    struct OutMessage {
        std::vector<uint8_t> payload;
        uint32_t timestampMs = 0;
        uint8_t typeId = 0;  // 8=audio 9=video 18=script data 20=command
        uint32_t msgStreamId = 0;
        uint8_t chunkStreamId = 0;
    };

    static bool ParseUrl(const std::string &url, RtmpUrl &out);
    void WriteCommandMessage(OutMessage &msg, const std::vector<uint8_t> &amfPayload);

    void ThreadMain();
    // 单轮连接生命周期：连接→握手→命令序列→推流循环；返回 false 表示连接断开（可重连）
    bool RunSession();
    bool ConnectSocket();
    bool Handshake();
    bool SendConnectCommand();
    bool SendCreateStreamCommand();
    bool SendPublishCommand();

    // 消息发送：chunk 化（chunkSize 默认 4096 协商）
    bool SendMessage(const OutMessage &msg);
    bool SendRaw(const uint8_t *data, size_t size);

    // 消息接收：chunk 重组（处理 fmt 0/1/2/3、extended timestamp、Set Chunk Size）
    // 返回 true 表示收到一条完整消息
    bool RecvMessage();
    bool RecvRaw(uint8_t *data, size_t size);
    bool HandleCommandMessage(const uint8_t *data, size_t size);
    bool HandleProtocolControl(uint8_t typeId, const uint8_t *data, size_t size);

    void SetState(State state, int reconnectAttempt = 0);
    void FailSession(int errorCode, const std::string &message);
    void EnqueueConfigMessages(); // （重）连成功且 publish 后发送 metadata + sequence header

    int socketFd_ = -1;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<State> state_{State::kIdle};
    Callbacks callbacks_;

    RtmpUrl url_;
    std::string tcUrl_;

    // 发送队列：音视频分离（音频不丢）
    std::unique_ptr<BoundedQueue<OutMessage>> videoQueue_;
    std::unique_ptr<BoundedQueue<OutMessage>> audioQueue_;
    // 控制消息（metadata/sequence header/command）队列：连接建立后优先发送
    std::unique_ptr<BoundedQueue<OutMessage>> controlQueue_;

    // 编码配置缓存（重连重发）
    std::mutex configMutex_;
    std::vector<uint8_t> avcC_;
    std::vector<uint8_t> asc_;
    std::vector<uint8_t> metaData_;
    bool hasMeta_ = false;

    std::atomic<int64_t> sentBytes_{0};
    std::atomic<int64_t> droppedVideoFrames_{0};
    std::atomic<int64_t> basePtsUs_{-1}; // 会话 epoch（首帧 pts）

    // 接收侧 chunk 重组状态
    static constexpr int kMaxChunkStreams = 8;
    struct ChunkIn {
        uint32_t timestamp = 0;
        uint32_t msgLen = 0;
        uint8_t typeId = 0;
        uint32_t msgStreamId = 0;
        std::vector<uint8_t> payload;
        bool headerValid = false;
    };
    ChunkIn chunkIn_[kMaxChunkStreams];
    uint32_t inChunkSize_ = 128;
    uint32_t outChunkSize_ = 4096;
    uint32_t streamId_ = 0;
    int txnCounter_ = 1;

    // 命令同步：等待 _result / onStatus 的条件变量
    std::mutex cmdMutex_;
    std::condition_variable cmdCv_;
    int pendingTxnResult_ = 0;   // 最近完成的 txn id（0=无）
    bool publishStarted_ = false;
    bool sessionDead_ = false;
};

} // namespace media_stream

#endif // MEDIA_STREAM_RTMP_CLIENT_H
