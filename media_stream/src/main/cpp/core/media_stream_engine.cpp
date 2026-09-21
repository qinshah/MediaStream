#include "media_stream_engine.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include <multimedia/player_framework/native_avscreen_capture_base.h>
#include <multimedia/player_framework/native_avscreen_capture_errors.h>

#include "../napi/js_event_emitter.h"
#include "../common/logger.h"

namespace media_stream {

// 音频源降级检测阈值：连续 1s 麦克风无数据
static constexpr int kMicDegradeThresholdMs = 1000;

MediaStreamEngine &MediaStreamEngine::Instance() {
    static MediaStreamEngine engine;
    return engine;
}

void MediaStreamEngine::SetEventEmitter(JsEventEmitter *emitter) {
    emitter_ = emitter;
}

void MediaStreamEngine::Init(const std::string &filesDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        return;
    }
    filesDir_ = filesDir;
    videosDir_ = filesDir_ + "/videos";
    inited_ = true;
    MS_LOG_INFO("Engine init filesDir=%{public}s", filesDir_.c_str());
}

// —— 状态事件上报 ——

void MediaStreamEngine::EmitCaptureState(const char *state) {
    if (emitter_ != nullptr) {
        EventData e;
        e.type = EventType::kCaptureState;
        e.state = state;
        emitter_->Emit(std::move(e));
    }
}

void MediaStreamEngine::EmitStreamState(const char *state, int reconnectAttempt) {
    if (emitter_ != nullptr) {
        EventData e;
        e.type = EventType::kStreamState;
        e.state = state;
        e.hasReconnectAttempt = true;
        e.reconnectAttempt = reconnectAttempt;
        emitter_->Emit(std::move(e));
    }
}

void MediaStreamEngine::EmitRecordState(const char *state) {
    if (emitter_ != nullptr) {
        EventData e;
        e.type = EventType::kRecordState;
        e.state = state;
        emitter_->Emit(std::move(e));
    }
}

void MediaStreamEngine::EmitError(EngineError code, const std::string &message, const char *source) {
    if (emitter_ != nullptr) {
        EventData e;
        e.type = EventType::kError;
        e.errorCode = static_cast<int32_t>(code);
        e.message = message;
        e.source = source;
        emitter_->Emit(std::move(e));
    }
}

void MediaStreamEngine::EmitMicDegraded() {
    if (emitter_ != nullptr) {
        EventData e;
        e.type = EventType::kMicDegraded;
        e.degradedTo = "inner";
        e.message = "麦克风无数据，已自动降级为仅系统内录";
        emitter_->Emit(std::move(e));
    }
}

// —— 预设与尺寸 ——

int MediaStreamEngine::PresetShortEdge(const std::string &preset) {
    if (preset == "1080p") {
        return 1080;
    }
    if (preset == "480p") {
        return 480;
    }
    return 720;
}

bool MediaStreamEngine::ComputeCaptureSize(int &width, int &height) {
    int32_t dw = 0, dh = 0;
    if (!ScreenCapture::QueryDisplaySize(dw, dh)) {
        MS_LOG_ERROR("QueryDisplaySize failed");
        return false;
    }
    // 以短边为目标，长边按屏幕宽高比等比缩放
    int targetShort = PresetShortEdge(config_.preset);
    bool portrait = dw < dh;
    int shortEdge = portrait ? dw : dh;
    int longEdge = portrait ? dh : dw;
    double ratio = static_cast<double>(targetShort) / static_cast<double>(shortEdge);
    int newLong = static_cast<int>(std::lround(longEdge * ratio));
    // 偶数对齐
    width = (portrait ? (newLong & ~1) : (targetShort & ~1));
    height = (portrait ? (targetShort & ~1) : (newLong & ~1));
    return true;
}

// —— 对外 API ——

bool MediaStreamEngine::StartStreaming(const Config &config, int &errCode, std::string &errMsg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        errCode = static_cast<int>(EngineError::kInternalError);
        errMsg = "引擎未初始化";
        return false;
    }
    if (streamState_ == "connecting" || streamState_ == "streaming" || streamState_ == "reconnecting") {
        errCode = static_cast<int>(EngineError::kBusy);
        errMsg = "推流已在进行中";
        return false;
    }
    config_ = config;
    lastConfig_ = config;
    // 参数校验 + 错误码映射
    if (config.rtmpUrl.rfind("rtmp://", 0) != 0) {
        errCode = static_cast<int>(EngineError::kRtmpUrlInvalid);
        errMsg = "RTMP 地址需以 rtmp:// 开头";
        return false;
    }

    if (!EnsureCapturePipelineLocked(errCode, errMsg)) {
        return false;
    }

    streamState_ = "connecting";
    EmitStreamState("connecting");

    rtmpClient_ = std::make_unique<RtmpClient>();
    RtmpClient::Callbacks cb;
    cb.onState = [this](RtmpClient::State state, int attempt) {
        std::lock_guard<std::mutex> lk(mutex_);
        switch (state) {
            case RtmpClient::State::kStreaming:
                streamState_ = "streaming";
                EmitStreamState("streaming");
                streamStartMs_ = -1; // 下个统计周期重新计时
                break;
            case RtmpClient::State::kReconnecting:
                streamState_ = "reconnecting";
                EmitStreamState("reconnecting", attempt);
                break;
            case RtmpClient::State::kStopped:
                if (streamState_ != "error") {
                    streamState_ = "idle";
                    EmitStreamState("idle");
                }
                break;
            case RtmpClient::State::kError:
                streamState_ = "error";
                EmitStreamState("error");
                break;
            default:
                break;
        }
    };
    cb.onError = [this](int code, const std::string &message) {
        std::lock_guard<std::mutex> lk(mutex_);
        streamState_ = "error";
        EmitStreamState("error");
        EmitError(static_cast<EngineError>(code), message, "rtmp");
        MS_LOG_ERROR("rtmp error code=%{public}d %{public}s", code, message.c_str());
    };
    if (!rtmpClient_->Start(config.rtmpUrl, std::move(cb))) {
        // URL 已在上面校验；此处仅防御
        rtmpClient_.reset();
        streamState_ = "error";
        EmitStreamState("error");
        errCode = static_cast<int>(EngineError::kRtmpUrlInvalid);
        errMsg = "RTMP 地址非法";
        return false;
    }

    EmitCaptureState("active");
    captureState_ = "active";
    StartStatsLocked();
    MS_LOG_INFO("StartStreaming ok");
    return true;
}

void MediaStreamEngine::StopStreaming() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rtmpClient_) {
            rtmpClient_->Stop();
            rtmpClient_.reset();
        }
        if (streamState_ != "error") {
            streamState_ = "idle";
            EmitStreamState("stopped");
        }
    }
    // 管线收尾放到锁外（OH_*_Stop 在锁外执行，避免与采集/编码回调线程锁互斥量死锁）
    MaybeStopPipelineUnlocked();
}

bool MediaStreamEngine::StartRecording(int &errCode, std::string &errMsg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        errCode = static_cast<int>(EngineError::kInternalError);
        errMsg = "引擎未初始化";
        return false;
    }
    if (recordState_ == "recording") {
        errCode = static_cast<int>(EngineError::kBusy);
        errMsg = "录制已在进行中";
        return false;
    }
    // 若无推流，需独立拉起管线（录制是首个输出）
    if (!EnsureCapturePipelineLocked(errCode, errMsg)) {
        return false;
    }

    // 编码配置齐备才真正启动 MP4；否则先标记 pendingRecord
    // 仅视频轨即可启动（系统内录音频无播放时无 ASC，录制静音画面也需能落盘）
    if (!avcC_.empty() && !mp4Started_) {
        mp4Recorder_ = std::make_unique<Mp4Recorder>();
        Mp4Recorder::Callbacks mcb;
        mcb.onFinished = [this](const Mp4Recorder::Result &result) {
            // 仅在该锁内更新状态并派发事件，随后释放锁；管线（OH_*_Stop）收尾放到锁外执行，
            // 避免在写线程（正在被主线程 join）上持锁调用阻断式 Stop 造成锁序反转死锁。
            {
                std::lock_guard<std::mutex> lk(mutex_);
                recordState_ = "stopped";
                EmitRecordState("stopped");
                EmitRecordFinishedEvent(result);
            }
            MaybeStopPipelineUnlocked();
        };
        mcb.onError = [this](int code, const std::string &message) {
            std::lock_guard<std::mutex> lk(mutex_);
            recordState_ = "error";
            EmitRecordState("error");
            EmitError(static_cast<EngineError>(code), message, "mp4");
        };
        int w = videoEncoder_ ? videoEncoder_->Width() : 720;
        int h = videoEncoder_ ? videoEncoder_->Height() : 1280;
        if (!mp4Recorder_->Start(videosDir_, w, h, avcC_, asc_, std::move(mcb))) {
            mp4Recorder_.reset();
            errCode = static_cast<int>(EngineError::kInternalError);
            errMsg = "启动录制失败";
            return false;
        }
        mp4Started_ = true;
    } else {
        pendingRecord_ = true;
    }

    recordState_ = "recording";
    EmitRecordState("recording");
    if (captureState_ != "active") {
        captureState_ = "active";
        EmitCaptureState("active");
    }
    StartStatsLocked();
    MS_LOG_INFO("StartRecording ok");
    return true;
}

void MediaStreamEngine::StopRecording() {
    // 勿在持有 mutex_ 时调用 mp4Recorder_->Stop()：Stop 内部会 join 写线程，
    // 而写线程收尾会回调 onFinished 并重新 lock(mutex_)。若持锁 join 必然死锁，
    // 导致主/JS 线程无限阻塞，被系统 THREAD_BLOCK_6S 判冻结并杀进程。
    // 因此这里先短暂上锁更新状态并取出指针，再释放锁后 join。
    Mp4Recorder *recorder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingRecord_ = false;
        recorder = mp4Recorder_.get();
    }
    if (recorder != nullptr) {
        recorder->Stop(); // 触发 onFinished（在写线程上，锁已释放，不会死锁）
    }
    // 状态在 onFinished 里置 stopped，这里先不重复发
}

void MediaStreamEngine::Destroy() {
    Mp4Recorder *recorder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingRecord_ = false;
        statsRunning_ = false;
        if (statsThread_.joinable()) {
            statsThread_.join();
        }
        if (rtmpClient_) {
            rtmpClient_->Stop();
            rtmpClient_.reset();
        }
        recorder = mp4Recorder_.get();
    }
    // 同样在锁外 join 写线程，避免 onFinished 重新加锁造成死锁
    if (recorder != nullptr) {
        recorder->Stop();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mp4Recorder_.reset();
        inited_ = false;
    }
    // 采集/编码在锁外收尾（OH_*_Stop 在锁外执行）
    TeardownPipelineUnlocked();
}

// —— 采集+编码管线 ——

bool MediaStreamEngine::EnsureCapturePipelineLocked(int &errCode, std::string &errMsg) {
    // 参数校验：捕获非法 fps/bitrate
    if (config_.fps < 1 || config_.fps > 60) {
        errCode = static_cast<int>(EngineError::kInvalidArg);
        errMsg = "帧率需在 1-60 之间";
        return false;
    }
    if (config_.videoBitrateKbps < 200 || config_.videoBitrateKbps > 20000) {
        errCode = static_cast<int>(EngineError::kInvalidArg);
        errMsg = "码率需在 200-20000 kbps 之间";
        return false;
    }

    // 若已有采集/编码器在运行，直接复用
    if (capture_ && videoEncoder_) {
        return true;
    }
    if (capture_ || videoEncoder_ || audioEncoder_) {
        errCode = static_cast<int>(EngineError::kBusy);
        errMsg = "会话占用中，请稍后再试";
        return false;
    }

    int width = 0, height = 0;
    if (!ComputeCaptureSize(width, height)) {
        errCode = static_cast<int>(EngineError::kCaptureInitFail);
        errMsg = "获取屏幕尺寸失败";
        return false;
    }

    bool micEnabled = (config_.audioMode == "mic" || config_.audioMode == "micInner");
    bool innerEnabled = (config_.audioMode == "inner" || config_.audioMode == "micInner");
    mixer_ = std::make_unique<AudioMixer>(
        config_.audioMode == "mic" ? AudioMixer::Mode::kMicOnly
        : config_.audioMode == "micInner" ? AudioMixer::Mode::kMicAndInner
                                          : AudioMixer::Mode::kInnerOnly);

    // 编码器先就绪，采集回调进来时可直接投递
    videoEncoder_ = std::make_unique<VideoEncoder>();
    VideoEncoder::Callbacks vcb;
    vcb.onOutput = [this](const uint8_t *d, int32_t s, int64_t pts, bool key) {
        OnEncodedVideo(d, s, pts, key);
    };
    vcb.onCodecConfig = [this](const std::vector<uint8_t> &config) { OnAvccReady(config); };
    vcb.onError = [this](int32_t code) {
        std::lock_guard<std::mutex> lk(mutex_);
        EmitError(EngineError::kEncoderError, "视频编码器出错", "videoEncoder");
    };
    if (!videoEncoder_->Start(width, height, config_.fps, config_.videoBitrateKbps, std::move(vcb))) {
        videoEncoder_.reset();
        errCode = static_cast<int>(EngineError::kEncoderInitFail);
        errMsg = "视频编码器初始化失败";
        return false;
    }

    audioEncoder_ = std::make_unique<AudioEncoder>();
    AudioEncoder::Callbacks acb;
    acb.onOutput = [this](const uint8_t *d, int32_t s, int64_t pts) {
        OnEncodedAudio(d, s, pts);
    };
    acb.onAsc = [this](const std::vector<uint8_t> &asc) { OnAscReady(asc); };
    acb.onError = [this](int32_t code) {
        std::lock_guard<std::mutex> lk(mutex_);
        EmitError(EngineError::kEncoderError, "音频编码器出错", "audioEncoder");
    };
    if (!audioEncoder_->Start(std::move(acb))) {
        audioEncoder_.reset();
        videoEncoder_.reset();
        errCode = static_cast<int>(EngineError::kEncoderInitFail);
        errMsg = "音频编码器初始化失败";
        return false;
    }

    mixer_->SetOutputCallback([this](const int16_t *pcm, int32_t bytes, int64_t ptsNs) {
        // 丢掉的音频（mixer 直通）——统一反馈到编码器
        audioEncoder_->InputPcm(pcm, bytes, ptsNs);
    });

    capture_ = std::make_unique<ScreenCapture>();
    ScreenCapture::Callbacks scb;
    scb.onVideoFrame = [this](const uint8_t *d, int w, int h, bool nv12, int64_t pts) {
        OnCapturedVideo(d, w, h, nv12, pts);
    };
    scb.onInnerAudio = [this](const uint8_t *pcm, int32_t bytes, int64_t pts) {
        OnCapturedInnerAudio(pcm, bytes, pts);
    };
    scb.onMicAudio = [this](const uint8_t *pcm, int32_t bytes, int64_t pts) {
        OnCapturedMicAudio(pcm, bytes, pts);
    };
    scb.onError = [this](int32_t code) { OnCaptureError(code); };
    scb.onUserStopped = [this]() { OnCaptureUserStopped(); };
    if (!capture_->Start({width, height, config_.fps, micEnabled, innerEnabled}, std::move(scb))) {
        capture_.reset();
        audioEncoder_.reset();
        videoEncoder_.reset();
        mixer_.reset();
        errCode = static_cast<int>(EngineError::kCaptureInitFail);
        errMsg = "采集启动失败";
        return false;
    }

    captureState_ = "active";
    EmitCaptureState("active");
    MS_LOG_INFO("Capture pipeline up %{public}dx%{public}d", width, height);
    return true;
}

void MediaStreamEngine::TeardownPipelineUnlocked() {
    // 两段式收尾避免锁序反转死锁：
    //  1) 锁内将采集/编码对象从引擎剥离并在引擎侧置空，回调线程（OnCapturedVideo 等）随即可见空指针安全退出；
    //  2) 锁外再执行 OH_AVScreenCapture_StopScreenCapture / OH_*_Stop / Destroy。
    // 若持引擎锁调用阻断式 Stop，而采集/编码回调线程又在回调里锁同一把 mutex_，
    // 会让 Stop 等到回调线程返回、回调线程又等锁 → 死锁（表现为 APP THREAD_BLOCK 冻结）。
    std::unique_ptr<ScreenCapture> cap;
    std::unique_ptr<AudioEncoder> aen;
    std::unique_ptr<VideoEncoder> ven;
    std::unique_ptr<AudioMixer> mix;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cap = std::move(capture_);
        aen = std::move(audioEncoder_);
        ven = std::move(videoEncoder_);
        mix = std::move(mixer_);
        avcC_.clear();
        asc_.clear();
        mp4Started_ = false;
    }
    if (cap) {
        cap->Stop();
    }
    if (aen) {
        aen->Stop();
    }
    if (ven) {
        ven->Stop();
    }
    // mix 无后台线程，随局部对象析构释放即可
}

void MediaStreamEngine::MaybeStopPipelineUnlocked() {
    bool teardown = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool streaming = streamState_ == "streaming" || streamState_ == "connecting" ||
                         streamState_ == "reconnecting";
        bool recording = recordState_ == "recording";
        teardown = !streaming && !recording;
    }
    if (!teardown) {
        return;
    }
    TeardownPipelineUnlocked();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (captureState_ != "idle") {
            captureState_ = "idle";
            EmitCaptureState("idle");
        }
        StopStatsLocked();
    }
}

// —— 采集回调 ——

void MediaStreamEngine::OnCapturedVideo(const uint8_t *data, int width, int height, bool isNv12,
                                        int64_t ptsNs) {
    // 采集回调与管线收尾可能并发（收尾会清空 videoEncoder_），此处统一加锁读取并判空，
    // 避免悬垂指针；RGBA 暂存缓冲也在同锁内访问。
    std::lock_guard<std::mutex> lock(mutex_);
    if (videoEncoder_ == nullptr) {
        return;
    }
    int64_t ptsUs = ptsNs / 1000;
    if (isNv12) {
        videoEncoder_->InputFrame(data, ptsUs);
    } else {
        // RGBA 兜底：软件转 NV12 进编码器
        size_t need = static_cast<size_t>(width) * height * 3 / 2;
        if (rgbaToNv12Scratch_.size() < need) {
            rgbaToNv12Scratch_.resize(need);
        }
        extern void RgbaToNv12(const uint8_t *, size_t, int, int, uint8_t *, uint8_t *);
        RgbaToNv12(data, static_cast<size_t>(width) * 4, width, height, rgbaToNv12Scratch_.data(),
                   rgbaToNv12Scratch_.data() + static_cast<size_t>(width) * height);
        videoEncoder_->InputFrame(rgbaToNv12Scratch_.data(), ptsUs);
    }
}

void MediaStreamEngine::OnCapturedInnerAudio(const uint8_t *pcm, int32_t bytes, int64_t ptsNs) {
    if (audioEncoder_) {
        // 双输入由 mixer 混音，单输入直通
        if (mixer_ && config_.audioMode == "micInner") {
            mixer_->PushInner(pcm, bytes, ptsNs);
        } else {
            audioEncoder_->InputPcm(reinterpret_cast<const int16_t *>(pcm), bytes, ptsNs);
        }
    }
}

void MediaStreamEngine::OnCapturedMicAudio(const uint8_t *pcm, int32_t bytes, int64_t ptsNs) {
    if (audioEncoder_) {
        if (mixer_ && config_.audioMode == "micInner") {
            mixer_->PushMic(pcm, bytes, ptsNs);
        } else {
            audioEncoder_->InputPcm(reinterpret_cast<const int16_t *>(pcm), bytes, ptsNs);
        }
    }
}

void MediaStreamEngine::OnCaptureError(int32_t errorCode) {
    // errorCode 为 AVScreenCapture 错误码：操作被拒（弹窗拒绝）转权限错误
    MS_LOG_ERROR("capture error code=%{public}d", (int)errorCode);
    bool permissionDenied = (errorCode == AV_SCREEN_CAPTURE_ERR_OPERATE_NOT_PERMIT) ||
                            (errorCode == AV_SCREEN_CAPTURE_ERR_INVALID_STATE);
    std::lock_guard<std::mutex> lock(mutex_);
    if (captureState_ == "error") {
        return;
    }
    captureState_ = "error";
    EmitCaptureState("error");
    if (permissionDenied) {
        EmitError(EngineError::kCapturePermissionDenied, "录屏授权被拒绝，已停止输出", "capture");
    } else {
        EmitError(EngineError::kCaptureInitFail, "采集意外中断，已停止输出", "capture");
    }
}

void MediaStreamEngine::OnCaptureUserStopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (captureState_ == "error") {
        return;
    }
    captureState_ = "idle";
    recordState_ = "stopped";
    streamState_ = "error";
    EmitCaptureState("stopped");
    EmitRecordState("stopped");
    EmitStreamState("error");
    MS_LOG_INFO("capture stopped by user, all outputs halted");
}

// —— 编码回调 ——

void MediaStreamEngine::OnEncodedVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe) {
    encodedVideoFrames_++;
    encodedBytes_ += size;
    // 分发：RTMP + MP4
    if (rtmpClient_) {
        rtmpClient_->SendVideo(data, size, ptsUs, isKeyframe);
    }
    if (mp4Recorder_ && mp4Started_) {
        mp4Recorder_->WriteVideo(data, size, ptsUs, isKeyframe);
    }
}

void MediaStreamEngine::OnEncodedAudio(const uint8_t *data, int32_t size, int64_t ptsUs) {
    if (rtmpClient_) {
        rtmpClient_->SendAudio(data, size, ptsUs);
    }
    if (mp4Recorder_ && mp4Started_) {
        mp4Recorder_->WriteAudio(data, size, ptsUs);
    }
}

void MediaStreamEngine::OnAvccReady(const std::vector<uint8_t> &avcC) {
    std::lock_guard<std::mutex> lock(mutex_);
    avcC_ = avcC;
    if (rtmpClient_) {
        rtmpClient_->SetVideoConfig(avcC);
    }
    TryStartPendingRecordLocked();
}

void MediaStreamEngine::OnAscReady(const std::vector<uint8_t> &asc) {
    std::lock_guard<std::mutex> lock(mutex_);
    asc_ = asc;
    if (rtmpClient_) {
        rtmpClient_->SetAudioConfig(asc);
    }
    TryStartPendingRecordLocked();
}

void MediaStreamEngine::TryStartPendingRecordLocked() {
    // 仅需视频 avcC 到位即可启动（音频可选，静音画面正常落盘）
    if (!pendingRecord_ || mp4Started_ || avcC_.empty()) {
        return;
    }
    mp4Recorder_ = std::make_unique<Mp4Recorder>();
    Mp4Recorder::Callbacks mcb;
    mcb.onFinished = [this](const Mp4Recorder::Result &result) {
        // 锁外收尾管线，避免写线程持锁调用阻断式 Stop 与回调加锁死锁
        {
            std::lock_guard<std::mutex> lk(mutex_);
            recordState_ = "stopped";
            EmitRecordState("stopped");
            EmitRecordFinishedEvent(result);
        }
        MaybeStopPipelineUnlocked();
    };
    mcb.onError = [this](int code, const std::string &message) {
        std::lock_guard<std::mutex> lk(mutex_);
        recordState_ = "error";
        EmitRecordState("error");
        EmitError(static_cast<EngineError>(code), message, "mp4");
    };
    int w = videoEncoder_ ? videoEncoder_->Width() : 720;
    int h = videoEncoder_ ? videoEncoder_->Height() : 1280;
    if (mp4Recorder_->Start(videosDir_, w, h, avcC_, asc_, std::move(mcb))) {
        mp4Started_ = true;
    } else {
        mp4Recorder_.reset();
        pendingRecord_ = false;
        recordState_ = "error";
        EmitRecordState("error");
    }
}

void MediaStreamEngine::EmitRecordFinishedEvent(const Mp4Recorder::Result &result) {
    if (emitter_ == nullptr) {
        return;
    }
    EventData e;
    e.type = EventType::kRecordFinished;
    e.fileName = result.fileName;
    e.filePath = result.filePath;
    e.durationMs = result.durationMs;
    e.sizeBytes = result.sizeBytes;
    e.createdAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    e.reason = result.normal ? "normal" : "error";
    emitter_->Emit(std::move(e));
}

void MediaStreamEngine::OnRecordFinished(const Mp4Recorder::Result &result) {
    EmitRecordFinishedEvent(result);
}

// —— 统计线程 ——

void MediaStreamEngine::StatsThreadMain() {
    while (statsRunning_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!statsRunning_.load() || emitter_ == nullptr) {
            continue;
        }
        EventData e;
        e.type = EventType::kStats;

        int64_t frames = encodedVideoFrames_.load();
        int64_t bytes = encodedBytes_.load();
        double dt = 0.5;
        e.hasVideoFps = true;
        e.videoFps = (frames - lastStatsFrames_) / dt;
        e.hasVideoBitrate = true;
        e.videoBitrateKbps = (bytes - lastStatsBytes_) * 8.0 / 1000.0 / dt;
        lastStatsFrames_ = frames;
        lastStatsBytes_ = bytes;

        if (rtmpClient_) {
            e.hasSentBytes = true;
            e.sentBytes = rtmpClient_->SentBytes();
            e.hasDroppedFrames = true;
            e.droppedVideoFrames = rtmpClient_->DroppedVideoFrames();
            if (streamStartMs_ < 0) {
                streamStartMs_ = 0;
            }
            e.hasStreamDuration = true;
            e.streamDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count() -
                                 streamStartMs_;
        }
        if (mp4Recorder_ && mp4Started_) {
            e.hasRecordDuration = true;
            e.recordDurationMs = mp4Recorder_->DurationMs();
        }
        emitter_->Emit(std::move(e));

        // 麦克风降级检测（仅 micInner 模式）
        if (config_.audioMode == "micInner" && !micDegraded_.load() && audioEncoder_ &&
            !mixer_->MicEverReceived()) {
            // Mic 持续无数据，降级为 inner
            std::lock_guard<std::mutex> lk(mutex_);
            if (!micDegraded_.load()) {
                micDegraded_ = true;
                EmitMicDegraded();
            }
        }
    }
}

void MediaStreamEngine::StartStatsLocked() {
    if (statsRunning_.exchange(true)) {
        return;
    }
    lastStatsFrames_ = 0;
    lastStatsBytes_ = 0;
    streamStartMs_ = -1;
    statsThread_ = std::thread(&MediaStreamEngine::StatsThreadMain, this);
}

void MediaStreamEngine::StopStatsLocked() {
    statsRunning_ = false;
    if (statsThread_.joinable()) {
        statsThread_.join();
    }
}

} // namespace media_stream