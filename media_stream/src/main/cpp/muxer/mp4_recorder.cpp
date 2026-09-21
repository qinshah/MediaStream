#include "mp4_recorder.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmuxer.h>

#include "../common/bounded_queue.h"
#include "../common/logger.h"

namespace media_stream {

// ErrorCode 与 Index.d.ts 对齐
static constexpr int kErrStorageFull = 11;
static constexpr int kErrInternal = 12;

static constexpr size_t kWriteQueueCap = 600; // 阻塞策略；磁盘写快，常规不满

// 归零点等待上限：等另一轨首样本最多这么久（单轨、或某轨长期无数据时兜底）
static constexpr int64_t kFirstPtsWaitMs = 220;
// 归零点待定期间允许缓存的最大样本数（防内存膨胀；正常 220ms 窗口内远达不到）
static constexpr size_t kMaxPendingFirst = 96;

// 等首个视频关键帧的上限：引擎挂录制时已强制请求 IDR，正常几十毫秒就到。超时则放行，
// 宁可开头画面有损，也不能整段没有录制。
static constexpr int64_t kIdrWaitMs = 1500;
// 停止时排空音频尾巴的上限。音频链路（采集→混音→AAC 编码器输入队列）积压实测 0.3~1.4s，
// 超过这个值就不再等：宁可截掉一点尾巴也不能让文件迟迟不落盘。
static constexpr int64_t kMaxAudioDrainMs = 2000;

static int64_t NowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Mp4Recorder::~Mp4Recorder() {
    Stop();
}

bool Mp4Recorder::Start(const std::string &dirPath, int width, int height, const std::vector<uint8_t> &avcC,
                        const std::vector<uint8_t> &asc, int64_t basePtsUs, Callbacks callbacks) {
    if (accepting_.exchange(true)) {
        MS_LOG_WARN("Mp4Recorder already recording, ignore Start");
        return true;
    }
    callbacks_ = std::move(callbacks);
    width_ = width;
    height_ = height;
    avcC_ = avcC;
    asc_ = asc;
    recording_.store(true);
    stopRequested_ = false;
    storageError_ = false;
    finished_ = false;
    writeThreadDone_ = false;
    // 时间轴零点：由调用方在「用户按下录制」那一刻确定（会话相对 μs），不再等两轨首样本取 min。
    // 原因（真机实测）：音频链路有 0.3~1.4s 内容积压，中途挂上来的录制最先收到的是「内容时间
    // 早于录制起点」的音频样本。取 min 会把零点一起拖回去 —— 实测被拖 1.357s，后果是：
    //   1) 文件开头 1.357s 只有「录制之前」的音频，画面冻在第一帧（界面计时已到 0:01，不是 0:00）；
    //   2) 整条音轨相对画面偏 1.357s（音画不同步）；
    //   3) 文件比真实录制长 1.357s，且尾部 1.4s 只剩画面没声音。
    // 现在零点固定=录制起点，早于它的样本按 MP4-EARLY 丢弃（那些内容本就不属于这次录制）。
    firstPtsUs_.store(basePtsUs);
    firstSealed_ = (basePtsUs >= 0);
    firstVideoIn_ = false;
    firstAudioIn_ = false;
    pendingFirst_.clear();
    pendingFirstStartMs_ = 0;
    droppedEarly_ = 0;
    videoStarted_.store(false);
    videoOpen_.store(true);
    videoGateStartMs_ = NowSteadyMs();
    droppedNoIdr_ = 0;
    draining_.store(false);
    drainUntilMs_.store(0);
    lastPtsUs_ = 0;
    stopSteadyMs_ = 0;
    writtenBytes_ = 0;
    writtenSamples_ = 0;
    // 录制起点（真实墙钟，用于时长统计）
    startSteadyMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    // 确保录制目录存在
    (void)mkdir(dirPath.c_str(), 0755);

    // 文件名含录制起始时间戳
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = {};
    localtime_r(&t, &tm);
    char name[64];
    snprintf(name, sizeof(name), "screen_%04d%02d%02d_%02d%02d%02d.mp4", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    fileName_ = name;
    filePath_ = dirPath + "/" + fileName_;

    fd_ = open(filePath_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd_ < 0) {
        MS_LOG_ERROR("open %{public}s failed errno=%{public}d", filePath_.c_str(), errno);
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "创建录制文件失败");
        }
        return false;
    }

    muxer_ = OH_AVMuxer_Create(fd_, AV_OUTPUT_FORMAT_MPEG_4);
    if (muxer_ == nullptr) {
        MS_LOG_ERROR("OH_AVMuxer_Create failed");
        close(fd_);
        fd_ = -1;
        unlink(filePath_.c_str());
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "创建 MP4 封装器失败");
        }
        return false;
    }

    // 视频轨：AVC + 宽高 + avcC
    if (!avcC_.empty()) {
        // 临时 dump：确认传入封装的 avcC 是否含 SPS/PPS（numSPS 应为 1）
        std::string hex;
        for (size_t i = 0; i < avcC_.size() && i < 16; i++) {
            char b[4];
            snprintf(b, sizeof(b), "%02x", avcC_[i]);
            hex += b;
        }
        uint8_t ns = avcC_.size() >= 6 ? (avcC_[5] & 0x1F) : 0xFF;
        MS_LOG_WARN("[HEX] avcC->muxer n=%{public}zu prefix=%{public}s numSPS=%{public}d", avcC_.size(), hex.c_str(), ns);
    }
    OH_AVFormat *vfmt = OH_AVFormat_Create();
    OH_AVFormat_SetStringValue(vfmt, OH_MD_KEY_CODEC_MIME, OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    OH_AVFormat_SetIntValue(vfmt, OH_MD_KEY_WIDTH, width_);
    OH_AVFormat_SetIntValue(vfmt, OH_MD_KEY_HEIGHT, height_);
    if (!avcC_.empty()) {
        OH_AVFormat_SetBuffer(vfmt, OH_MD_KEY_CODEC_CONFIG, avcC_.data(), avcC_.size());
    }
    int32_t rc = OH_AVMuxer_AddTrack(muxer_, &videoTrack_, vfmt);
    OH_AVFormat_Destroy(vfmt);
    if (rc != AV_ERR_OK || videoTrack_ < 0) {
        MS_LOG_ERROR("AddTrack video failed rc=%{public}d", rc);
        OH_AVMuxer_Destroy(muxer_);
        muxer_ = nullptr;
        close(fd_);
        fd_ = -1;
        unlink(filePath_.c_str());
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "添加视频轨失败");
        }
        return false;
    }

    // 音频轨（可选）：系统内录音频在无播放时无 ASC，此时仅视频轨，静音录制也能正常落盘
    audioTrack_ = -1;
    if (!asc_.empty()) {
        OH_AVFormat *afmt = OH_AVFormat_Create();
        OH_AVFormat_SetStringValue(afmt, OH_MD_KEY_CODEC_MIME, OH_AVCODEC_MIMETYPE_AUDIO_AAC);
        OH_AVFormat_SetIntValue(afmt, OH_MD_KEY_AUD_SAMPLE_RATE, 48000);
        OH_AVFormat_SetIntValue(afmt, OH_MD_KEY_AUD_CHANNEL_COUNT, 2);
        // 与官方录屏示例一致：显式声明 AAC-LC，便于封装器生成 esds 的 objectTypeIndication
        OH_AVFormat_SetIntValue(afmt, OH_MD_KEY_PROFILE, AAC_PROFILE_LC);
        OH_AVFormat_SetBuffer(afmt, OH_MD_KEY_CODEC_CONFIG, asc_.data(), asc_.size());
        rc = OH_AVMuxer_AddTrack(muxer_, &audioTrack_, afmt);
        OH_AVFormat_Destroy(afmt);
        if (rc != AV_ERR_OK || audioTrack_ < 0) {
            // 音频轨添加失败 → 降级为仅视频轨，不中断录制
            audioTrack_ = -1;
            MS_LOG_WARN("AddTrack audio failed rc=%{public}d, record video-only", rc);
        }
    } else {
        MS_LOG_INFO("no ASC (no system audio), record video-only");
    }

    hasVideoTrack_ = (videoTrack_ >= 0);
    hasAudioTrack_ = (audioTrack_ >= 0);
    rc = OH_AVMuxer_Start(muxer_);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_AVMuxer_Start failed rc=%{public}d", rc);
        OH_AVMuxer_Destroy(muxer_);
        muxer_ = nullptr;
        close(fd_);
        fd_ = -1;
        unlink(filePath_.c_str());
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "启动 MP4 封装失败");
        }
        return false;
    }

    MS_LOG_WARN("[MP4-BASE] fixed basePtsUs=%{public}lld (录制起点，零点由引擎给定)",
                static_cast<long long>(basePtsUs));

    queue_ = std::make_unique<BoundedQueue<Sample>>(kWriteQueueCap, OverflowPolicy::kBlockOnFull);
    writeThread_ = std::thread(&Mp4Recorder::WriteThreadMain, this);
    MS_LOG_INFO("Mp4Recorder started: %{public}s", filePath_.c_str());
    return true;
}

// 写队列满时的等待上限：超时即丢弃该元素（返回 false），避免持锁入队时因写线程
// 短暂卡顿（如 OH_AVMuxer 瞬时阻塞）而无限阻塞持锁线程，造成整个录制管线死锁
// （编码输出线程持 engine mutex_ → StopRecording/采集线程全部卡死 → APP_INPUT_BLOCK）。
static constexpr int64_t kWritePushTimeoutMs = 150; // 150ms 内没腾出空间则丢当前编码帧

void Mp4Recorder::WriteVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe) {
    if (!accepting_.load() || !videoOpen_.load() || data == nullptr || size <= 0) {
        return;
    }
    // 视频轨首个样本必须是同步样本（关键帧）：否则文件开头没有参考帧，播放器读不出画面。
    // 真机实测（先推流后录制）：录制起点落在 GOP 中间，轨道头 18 个样本 约 3.0s 全部不可解码，
    // ffmpeg 报 "Missing key frame while searching for timestamp: 0"，47 个样本只解出 29 帧；
    // 播放器只能黑屏/卡在第一帧，界面计时自然也不是从 0:00 开始。引擎在挂录制时已强制请求
    // IDR，这里再兜一道；等不到 IDR 也不能整段没画面，超时放行并告警。
    if (!videoStarted_.load()) {
        if (!isKeyframe) {
            if (NowSteadyMs() - videoGateStartMs_ < kIdrWaitMs) {
                int64_t n = droppedNoIdr_.fetch_add(1) + 1;
                if (n <= 3) {
                    MS_LOG_WARN("[MP4] video gate: drop non-sync sample while waiting IDR (n=%{public}lld)",
                                static_cast<long long>(n));
                }
                return;
            }
            MS_LOG_WARN("video gate: no IDR within %{public}lld ms, start video track with non-sync sample",
                        static_cast<long long>(kIdrWaitMs));
        }
        videoStarted_.store(true);
    }
    Sample s;
    s.data.assign(data, data + size);
    s.ptsUs = ptsUs;
    s.flags = isKeyframe ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    s.isVideo = true;
    if (queue_ && !queue_->TryPush(std::move(s), kWritePushTimeoutMs)) {
        MS_LOG_WARN("mp4 write queue full, drop video sample (size=%{public}d)", size);
    }
}

void Mp4Recorder::WriteAudio(const uint8_t *data, int32_t size, int64_t ptsUs) {
    if (audioTrack_ < 0) {
        return; // 无音频轨（系统内录无 ASC）
    }
    if (!accepting_.load() || data == nullptr || size <= 0) {
        return;
    }
    Sample s;
    s.data.assign(data, data + size);
    s.ptsUs = ptsUs;
    s.flags = AVCODEC_BUFFER_FLAGS_NONE;
    s.isVideo = false;
    if (queue_ && !queue_->TryPush(std::move(s), kWritePushTimeoutMs)) {
        MS_LOG_WARN("mp4 write queue full, drop audio sample (size=%{public}d)", size);
    }
}

int64_t Mp4Recorder::DurationMs() const {
    // 以真实墙钟计算录制时长（编码器绝对 pts 时钟不可靠，避免转换成极不合理的时长）。
    // 写线程收尾时记下 stopSteadyMs_，之后固定返回该终值：否则文件写完 moov 后，
    // 只要还有别的输出在继续，界面上的录制时长仍会随 now 一直涨。
    int64_t start = startSteadyMs_.load();
    if (start <= 0) {
        return 0;
    }
    int64_t stop = stopSteadyMs_.load();
    if (stop <= 0) {
        stop = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
    }
    return stop > start ? stop - start : 0;
}

void Mp4Recorder::Stop(int64_t audioDrainMs) {
    if (stopRequested_.exchange(true)) {
        return; // 已停止过（幂等）
    }
    // 视频轨立即关闭：排空只针对音频尾巴。若视频继续写入，文件尾部会多出「按下停止之后」的
    // 几秒画面，等于把「结尾只有画面没声音」换成「多一段停止后的画面」——两头都没修好。
    videoOpen_.store(false);
    // 时长定格在「用户按下停止」这一刻：此后写线程只是把在途样本与音频尾巴排空，不再代表
    // 真实录制内容。这样界面/事件里的时长与文件内容（约等于停止时刻）一致。
    {
        int64_t expect = 0;
        stopSteadyMs_.compare_exchange_strong(expect, NowSteadyMs());
    }
    // 音频尾巴排空：音频链路里还有 0.3~1.4s 已经采集、但尚未从编码器/队列出来的声音。
    // 立刻 Close 会让文件尾部丢掉这段时间的音频 —— 表现就是「结尾画面还在动，但没有声音」。
    // 因此保留音频写入一段有界时间，由写线程到点后自行收尾。
    if (audioDrainMs > 0 && hasAudioTrack_) {
        int64_t d = audioDrainMs > kMaxAudioDrainMs ? kMaxAudioDrainMs : audioDrainMs;
        drainUntilMs_.store(NowSteadyMs() + d);
        draining_.store(true);
        MS_LOG_WARN("mp4 Stop: drain audio tail %{public}lld ms before finalize (video closed now)",
                    static_cast<long long>(d));
        return;
    }
    accepting_.store(false);
    if (queue_) {
        queue_->Close(); // 唤醒写线程，排空后退出
    }
    // 核心修复：绝不在此同步 join 写线程。写线程可能阻塞在 OH_AVMuxer_WriteSampleBuffer / 
    // OH_AVMuxer_Stop 内部挂死，同步 join 会把调用线程（主/JS 线程）一起拖死，
    // 被系统 THREAD_BLOCK 看门狗判冻结并杀进程（正是“停止录屏后卡死闪退”的根因）。
    // 改为 detach：写线程在后台自行完成后序 muxer 收尾（写 moov→Destroy→close→onFinished）。
    // 调用方在销毁本对象前需 WaitWriteThreadDone() 确认写线程已退出，避免悬垂 this(UAF)。
    if (writeThread_.joinable()) {
        writeThread_.detach();
    }
}

bool Mp4Recorder::WaitWriteThreadDone(int64_t timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!writeThreadDone_.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return writeThreadDone_.load();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// 归零点确定前的入口：缓存首样本，等两轨齐（或超时）后取最小 ptsUs 作零点再落盘。
// 为什么不能用「首个写入的样本定零点」：录制起点的补帧是同步投递，比异步的音频编码回调
// 更早到达写线程；音频内容本身更早，却因此成为「负值」被钳到 0。实测一次 2.7s 录制里
// 4 帧音频（85ms 内容）被压进 t=0 的 0.6ms 内，播放器瞬间连播即爆音；同时音轨时间轴
// 总长比音频内容短 85ms，播放器必须全程补偿 —— 听感就是「电音」。
bool Mp4Recorder::WriteOneSample(const Sample &sample) {
    if (firstPtsUs_.load() < 0) {
        if (sample.isVideo) {
            firstVideoIn_ = true;
        } else {
            firstAudioIn_ = true;
        }
        if (pendingFirst_.empty()) {
            pendingFirstStartMs_ = NowSteadyMs();
        }
        pendingFirst_.push_back(sample);
        const bool videoReady = firstVideoIn_ || !hasVideoTrack_;
        const bool audioReady = firstAudioIn_ || !hasAudioTrack_;
        const bool timeout = (NowSteadyMs() - pendingFirstStartMs_) >= kFirstPtsWaitMs;
        // 单轨、或某轨长期无数据时不能无限等待：超时或缓存够多就用现有样本定零点
        if (!(videoReady && audioReady) && !timeout && pendingFirst_.size() < kMaxPendingFirst) {
            return true;
        }
        SealFirstPtsLocked();
        std::vector<Sample> pend = std::move(pendingFirst_);
        pendingFirst_.clear();
        for (const Sample &ps : pend) {
            if (!WriteSampleNow(ps)) {
                return false;
            }
        }
        return true;
    }
    return WriteSampleNow(sample);
}

// 由已缓存的首样本确定归零点：取两轨中最早的那个，保证没有任何一轨被钳位。
void Mp4Recorder::SealFirstPtsLocked() {
    if (firstSealed_) {
        return;
    }
    firstSealed_ = true;
    int64_t base = pendingFirst_.empty() ? 0 : pendingFirst_.front().ptsUs;
    for (const Sample &ps : pendingFirst_) {
        if (ps.ptsUs < base) {
            base = ps.ptsUs;
        }
    }
    if (base < 0) {
        base = 0;
    }
    firstPtsUs_.store(base);
    MS_LOG_WARN("[MP4-BASE] firstPtsUs=%{public}lld sealed (pending=%{public}zu video=%{public}d audio=%{public}d)",
                static_cast<long long>(base), pendingFirst_.size(), firstVideoIn_ ? 1 : 0,
                firstAudioIn_ ? 1 : 0);
}

bool Mp4Recorder::WriteSampleNow(const Sample &sample) {
    OH_AVBuffer *buffer = OH_AVBuffer_Create(static_cast<int32_t>(sample.data.size()));
    if (buffer == nullptr) {
        return false;
    }
    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);
    memcpy(addr, sample.data.data(), sample.data.size());
    OH_AVCodecBufferAttr attr = {};
    // pts 归零：以归零点为起点，单调递增
    const int64_t first = firstPtsUs_.load();
    if (sample.ptsUs < first) {
        // 早于归零点：其内容发生在本次输出起点之前，直接丢弃。
        // 不能像以前那样钳到 0 —— 多帧挤在同一时刻会被播放器瞬间连播，听感即爆音。
        int64_t n = droppedEarly_.fetch_add(1) + 1;
        if (n <= 5) {
            MS_LOG_WARN("[MP4-EARLY] drop %{public}s sample rawPtsUs=%{public}lld < firstPtsUs=%{public}lld (n=%{public}lld)",
                        sample.isVideo ? "video" : "audio", static_cast<long long>(sample.ptsUs),
                        static_cast<long long>(first), static_cast<long long>(n));
        }
        return true;
    }
    attr.pts = sample.ptsUs - first;
    // 临时诊断：首/尾及每1000样本记录 ptsUs 与归零后 pts，判断时间线是否异常
    int64_t cnt = writtenSamples_.load();
    if (cnt < 3 || cnt % 1000 == 0) {
        MS_LOG_WARN("[PTS] sample#%{public}lld rawPtsUs=%{public}lld firstPtsUs=%{public}lld normPts=%{public}lld",
                    cnt, sample.ptsUs, firstPtsUs_.load(), attr.pts);
    }
    attr.size = static_cast<int32_t>(sample.data.size());
    attr.offset = 0;
    attr.flags = sample.flags;
    OH_AVBuffer_SetBufferAttr(buffer, &attr);

    // 临时：dump 前3个视频样本首8字节，确认 AVCC(length prefix) 还是 Annex-B(start code)
    if (sample.isVideo && writtenSamples_.load() < 3 && sample.data.size() >= 8) {
        char h[17];
        for (int i = 0; i < 8; i++) {
            snprintf(h + i * 2, 3, "%02x", sample.data[i]);
        }
        h[16] = 0;
        MS_LOG_WARN("[HEX] video sample[%{public}lld] sz=%{public}zu head=%{public}s", writtenSamples_.load(),
                    sample.data.size(), h);
    }

    int32_t track = sample.isVideo ? videoTrack_ : audioTrack_;
    // 临时诊断：确认 OH_AVMuxer_WriteSampleBuffer 挂点（前/后各印一次）
    int64_t muxCnt = writtenSamples_.load();
    bool logThis = (muxCnt < 5 || muxCnt % 500 == 0) && muxCnt != 0;
    if (logThis) {
        MS_LOG_WARN("[MUX] WSB#%{public}lld before sz=%{public}lld track=%{public}d", muxCnt, sample.data.size(), track);
    }
    int32_t rc = OH_AVMuxer_WriteSampleBuffer(muxer_, track, buffer);
    if (logThis) {
        MS_LOG_WARN("[MUX] WSB#%{public}lld after  rc=%{public}d", muxCnt, rc);
    }
    OH_AVBuffer_Destroy(buffer);
    if (rc != AV_ERR_OK) {
        // 常见为存储不足
        MS_LOG_ERROR("WriteSampleBuffer failed rc=%{public}d track=%{public}d size=%{public}d", rc, track,
                     sample.data.size());
        storageError_ = true;
        return false;
    }
    writtenBytes_ += sample.data.size();
    writtenSamples_++;
    int64_t last = lastPtsUs_.load();
    if (sample.ptsUs > last) {
        lastPtsUs_.store(sample.ptsUs);
    }
    return true;
}

void Mp4Recorder::WriteThreadMain() {
    // 排空队列。两种模式：
    //  - 常规：Pop 阻塞；Close 且排空后返回 false → 退出。
    //  - 音频尾巴排空（Stop 带 drain）：PopTimed 轮询，到 drainUntilMs_ 后退出。必须带超时：
    //    排空期间音频若真断流（用户紧接着又停了推流），阻塞 Pop 会让文件永远不落盘。
    Sample s;
    for (;;) {
        bool got = false;
        if (draining_.load()) {
            if (!queue_) {
                break;
            }
            got = queue_->PopTimed(s, 30);
            if (!got && NowSteadyMs() >= drainUntilMs_.load()) {
                MS_LOG_WARN("audio drain deadline reached, finalize (in-flight=%{public}zu)",
                            queue_->Size());
                break;
            }
        } else {
            if (!queue_ || !queue_->Pop(s)) {
                break; // Close 且排空
            }
            got = true;
        }
        if (got && !WriteOneSample(s)) {
            break; // 存储错误：停止写入，走异常收尾
        }
    }
    // 收尾：不再接受写入（视频在 Stop 时已关，音频排空到点也关）
    accepting_.store(false);
    draining_.store(false);
    // 极短录制兜底：队列排空时若另一轨首样本始终没来，归零点仍未定，这里补一次落盘
    if (!pendingFirst_.empty()) {
        SealFirstPtsLocked();
        std::vector<Sample> pend = std::move(pendingFirst_);
        pendingFirst_.clear();
        for (const Sample &ps : pend) {
            if (!WriteSampleNow(ps)) {
                break;
            }
        }
    }

    // 安全收尾：Stop 写 moov → Destroy → close(fd)
    // 先定格录制时长：这一刻之后不再代表真实录制内容（后面只是封装收尾），
    // 也让 onFinished 给出的 durationMs 与界面上的终值完全一致。
    {
        // 时长终值：Stop() 已按「用户按下停止那一刻」定格；只有从未定格过（异常路径）才在此补。
        int64_t expect = 0;
        stopSteadyMs_.compare_exchange_strong(
            expect, std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
    }
    recording_.store(false);
    MS_LOG_INFO("mp4 tail stats: samples=%{public}lld droppedEarly=%{public}lld droppedNoIdr=%{public}lld",
                static_cast<long long>(writtenSamples_.load()),
                static_cast<long long>(droppedEarly_.load()),
                static_cast<long long>(droppedNoIdr_.load()));
    if (muxer_ != nullptr) {
        MS_LOG_WARN("[MUX] OH_AVMuxer_Stop before (moov)");
        int32_t s = OH_AVMuxer_Stop(muxer_);
        MS_LOG_WARN("[MUX] OH_AVMuxer_Stop after  rc=%{public}d", s);
        if (s != AV_ERR_OK) {
            MS_LOG_ERROR("OH_AVMuxer_Stop failed rc=%{public}d (moov 未写出，文件将不可播放)", s);
        } else {
            MS_LOG_INFO("OH_AVMuxer_Stop ok, moov written");
        }
        MS_LOG_WARN("[MUX] OH_AVMuxer_Destroy before");
        OH_AVMuxer_Destroy(muxer_);
        MS_LOG_WARN("[MUX] OH_AVMuxer_Destroy after");
        muxer_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    // 首次结束时才触发 onFinished；此后为兜底守卫（正常每周期仅一个写线程，防重复回调）
    if (finished_.exchange(true)) {
        writeThreadDone_ = true; // 即使提前返回也要标记写线程结束，防止调用方无限等待
        return;
    }
    Result result;
    result.fileName = fileName_;
    result.filePath = filePath_;
    result.durationMs = DurationMs();
    // 实际文件大小以磁盘为准
    struct stat st = {};
    if (stat(filePath_.c_str(), &st) == 0) {
        result.sizeBytes = st.st_size;
    } else {
        result.sizeBytes = writtenBytes_.load();
    }
    result.normal = !storageError_.load();
    MS_LOG_INFO("Mp4Recorder finished: %{public}s %{public}lldms %{public}lldB samples=%{public}lld normal=%{public}d",
                filePath_.c_str(), result.durationMs, result.sizeBytes, writtenSamples_.load(),
                result.normal ? 1 : 0);
    if (storageError_.load() && callbacks_.onError) {
        callbacks_.onError(kErrStorageFull, "存储空间不足，录制已停止");
    }
    if (callbacks_.onFinished) {
        callbacks_.onFinished(result);
    }
    // 最后设置：确保 onFinished（访问本对象成员）已执行完，调用方才可安全销毁本对象(UAF 防线)
    writeThreadDone_ = true;
}

} // namespace media_stream
