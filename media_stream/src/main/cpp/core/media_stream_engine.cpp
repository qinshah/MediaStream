#include "media_stream_engine.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include <multimedia/player_framework/native_avscreen_capture_base.h>
#include <multimedia/player_framework/native_avscreen_capture_errors.h>

#include "../napi/js_event_emitter.h"
#include "../common/logger.h"

namespace media_stream {

// 音频源降级检测阈值：连续 1s 麦克风无数据
static constexpr int kMicDegradeThresholdMs = 1000;

// 单调时钟：ns（供 fps 节流与输出 PTS 生成使用）
static int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 单调时钟：ms（供 ASC 等待超时判定）
static int64_t NowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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

// —— 采集/编码尺寸 ——

bool MediaStreamEngine::ComputeCaptureSize(int &width, int &height) {
    int32_t dw = 0, dh = 0;
    if (!ScreenCapture::QueryDisplaySize(dw, dh)) {
        MS_LOG_ERROR("QueryDisplaySize failed");
        return false;
    }
    // 原始流（OH_ORIGINAL_STREAM）模式下，videoFrameWidth/Height 必须与显示器的原生分辨率一致，
    // 否则 OH_AVScreenCapture_Init 会返回 OPERATE_NOT_PERMIT(rc=2)。因此这里直接返回原生显示尺寸
    // （自然方向，不做缩放/剪裁——此前按预设短边缩放并翻转了方向，导致 Init 被拒，回退 RGBA 又是占位灰帧，最终录成全黑）。
    width = dw & ~1;
    height = dh & ~1;
    return true;
}

// —— NV12 缩放与编码尺寸 ——
// 编码长边上限：缩到该尺寸以内，保证 16 对齐 + 解码器可解（高于此 OH H.264 编码器
// 对非 16 对齐大帧会输出错乱流，玩家绿屏/失败）
static constexpr int kEncodeLongMax = 1632;

void MediaStreamEngine::ComputeEncodeSize(int nativeW, int nativeH, int &encW, int &encH) {
    const int longEdge = nativeW > nativeH ? nativeW : nativeH;
    double ratio = 1.0;
    if (longEdge > kEncodeLongMax) {
        ratio = static_cast<double>(kEncodeLongMax) / static_cast<double>(longEdge);
    }
    // 等比缩放后四舍五入并 16 对齐（H.264 宏块边界，编码器/解码器要求）
    int w = static_cast<int>(std::lround(nativeW * ratio));
    int h = static_cast<int>(std::lround(nativeH * ratio));
    encW = (w + 8) & ~15;
    encH = (h + 8) & ~15;
    if (encW < 2) {
        encW = 2;
    }
    if (encH < 2) {
        encH = 2;
    }
    MS_LOG_INFO("encode size %{public}dx%{public}d from native %{public}dx%{public}d", encW, encH, nativeW, nativeH);
}

// NV12 双线性缩放：src(sw×sh) → dst(dw×dh)。Y 平面双线性；UV 为交错平面
// （每行 sw 字节，偶奇=U/V，共 sh/2 行），按逻辑色度平面(sw/2 × sh/2)双线性采样。
static void ScaleNv12(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    const int srcYSize = sw * sh;
    const int dstYSize = dw * dh;
    const uint8_t *srcUv = src + srcYSize;

    // Y 平面
    for (int dy = 0; dy < dh; ++dy) {
        float sy = static_cast<float>(dy) * sh / dh;
        int y0 = static_cast<int>(sy);
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float fy = sy - y0;
        const uint8_t *srow = src + y0 * sw;
        const uint8_t *srow2 = src + y1 * sw;
        uint8_t *drow = dst + dy * dw;
        for (int dx = 0; dx < dw; ++dx) {
            float sx = static_cast<float>(dx) * sw / dw;
            int x0 = static_cast<int>(sx);
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float fx = sx - x0;
            float top = srow[x0] + fx * (srow[x1] - srow[x0]);
            float bot = srow2[x0] + fx * (srow2[x1] - srow2[x0]);
            drow[dx] = static_cast<uint8_t>(top + fy * (bot - top));
        }
    }

    // U/V 平面：源逻辑色度尺寸(sw/2 × sh/2)，交错步长 sw；目标(dw/2 × dh/2)
    const int suw = sw / 2, suh = sh / 2;
    const int duw = dw / 2, duh = dh / 2;
    // 采样逻辑色度坐标(clamp+双线性)；off=0 取 U，off=1 取 V
    auto chromaAt = [&](float x, float y, int off) -> int {
        float gx = x < 0 ? 0 : (x > suw - 1 ? suw - 1 : x);
        float gy = y < 0 ? 0 : (y > suh - 1 ? suh - 1 : y);
        int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
        int x1 = x0 + 1 < suw ? x0 + 1 : x0;
        int y1 = y0 + 1 < suh ? y0 + 1 : y0;
        float fx = gx - x0, fy = gy - y0;
        auto at = [&](int cx, int cy) -> int { return srcUv[cy * sw + cx * 2 + off]; };
        float top = at(x0, y0) + fx * (at(x1, y0) - at(x0, y0));
        float bot = at(x0, y1) + fx * (at(x1, y1) - at(x0, y1));
        return static_cast<int>(top + fy * (bot - top));
    };
    for (int dy = 0; dy < duh; ++dy) {
        float sy = static_cast<float>(dy) * suh / duh;
        uint8_t *duv = dst + dstYSize + dy * dw;
        for (int dx = 0; dx < duw; ++dx) {
            float sx = static_cast<float>(dx) * suw / duw;
            duv[dx * 2] = static_cast<uint8_t>(chromaAt(sx, sy, 0));
            duv[dx * 2 + 1] = static_cast<uint8_t>(chromaAt(sx, sy, 1));
        }
    }
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

bool MediaStreamEngine::StartRecording(const Config &config, int &errCode, std::string &errMsg) {
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
    // 录制使用调用方传入的会话配置（含 audioMode/preset/fps/bitrate），确保麦克风等音频源真正启用。
    // 此前在此沿用 config_（仅 StartStreaming 会更新），纯录制场景下恒为默认 "inner"，导致
    // 选"麦克风"录制时 mic=0、无声。
    config_ = config;
    lastConfig_ = config;
    // 若无推流，需独立拉起管线（录制是首个输出）
    if (!EnsureCapturePipelineLocked(errCode, errMsg)) {
        return false;
    }
    // 每段录制从 0 起草 pts 基线（避免跨段复用旧起点导致原始 pts 膨胀，虽 muxer 归一化无碍，仍保持整洁）
    outPtsStartNs_.store(-1, std::memory_order_relaxed);
    captureLastNs_.store(0, std::memory_order_relaxed);
    MS_LOG_WARN("[CFG] startRecording fps=%{public}d bitrate=%{public}d mode=%{public}s", config_.fps,
                config_.videoBitrateKbps, config_.audioMode.c_str());

    // 挂起录制并启动 muxer：muxer 需在 AddTrack 前拿到音频 ASC（通常晚于视频 avcC），
    // 因此在 asc 就绪（或等待超时降级仅视频轨）前先把视频帧缓存，避免漏掉音频轨导致录制静音。
    pendingRecord_ = true;
    recordRequestSteadyMs_ = NowSteadyMs();
    pendingVideo_.clear();
    mp4Started_ = false;
    StartMuxerLocked();

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
    // 勿在持有 mutex_ 时调用 mp4Recorder_->Stop()：其内部会置停止标记并 detach 写线程，
    // 写线程收尾会回调 onFinished 并重新 lock(mutex_)。因此先短暂上锁更新状态并取出指针，
    // 释放锁后再调非阻塞 Stop()（不做同步 join，绝不拖死调用线程）。
    Mp4Recorder *recorder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingRecord_ = false;
        recordRequestSteadyMs_ = 0;
        pendingVideo_.clear(); // muxer 未启动即停止：丢弃缓存的待写视频帧
        // 立即反映停止意图：即使写线程在 OH_AVMuxer 内挂死、onFinished 迟迟不来，
        // 也能让 UI 立刻回到 stopped，避免卡在“录制中”状态（配合 Stop() 非阻塞 detach）。
        recordState_ = "stopped";
        EmitRecordState("stopped");
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
    // 同样在锁外 join 写线程，避免 onFinished 重新加锁造成死锁。
    // Stop() 已改为非阻塞(detach)：此处用超时等待写线程真正退出后再 reset，避免销毁
    // 仍被 OH_AVMuxer 挂住的写线程上的 this 造成悬垂(UAF)。
    if (recorder != nullptr) {
        recorder->Stop();
        recorder->WaitWriteThreadDone(3000); // 有界等待：不因 muxer 挂死而无限阻塞
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

    // 采集尺寸 = 屏幕原生分辨率（原始流要求与显示器一致，否则 Init 失败/假占位）
    int capW = 0, capH = 0;
    if (!ComputeCaptureSize(capW, capH)) {
        errCode = static_cast<int>(EngineError::kCaptureInitFail);
        errMsg = "获取屏幕尺寸失败";
        return false;
    }
    // 编码尺寸 = 原生长边裁剪 + 16 对齐（OH H.264 对非 16 对齐的大尺寸帧会输出错乱流 → 播放绿屏/失败）
    ComputeEncodeSize(capW, capH, encodeWidth_, encodeHeight_);
    scaleNv12Scratch_.resize(static_cast<size_t>(encodeWidth_) * encodeHeight_ * 3 / 2);

    bool micEnabled = (config_.audioMode == "mic" || config_.audioMode == "micInner");
    bool innerEnabled = (config_.audioMode == "inner" || config_.audioMode == "micInner");
    mixer_ = std::make_unique<AudioMixer>(
        config_.audioMode == "mic" ? AudioMixer::Mode::kMicOnly
        : config_.audioMode == "micInner" ? AudioMixer::Mode::kMicAndInner
                                          : AudioMixer::Mode::kInnerOnly);

    // 编码器先就绪，采集回调进来时可直接投递（以缩小后的可解码编码尺寸启动）
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
    if (!videoEncoder_->Start(encodeWidth_, encodeHeight_, config_.fps, config_.videoBitrateKbps, std::move(vcb))) {
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
    if (!capture_->Start({capW, capH, config_.fps, micEnabled, innerEnabled}, std::move(scb))) {
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
    MS_LOG_INFO("Capture pipeline up cap=%{public}dx%{public}d enc=%{public}dx%{public}d", capW, capH,
                encodeWidth_, encodeHeight_);
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
        pendingVideo_.clear();
        recordRequestSteadyMs_ = 0;
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
    // 关键修复：管线收尾（OH_AVScreenCapture_Stop / OH_*_Stop / Destroy）改为在独立后台线程执行，
    // 而非在执行本函数的线程上同步执行。原因是停止录制时主线程会在 mp4Recorder_->Stop() 里
    // writeThread_.join() 等待写线程退出；而写线程收尾回调 onFinished 又会走到这里做整条管线
    // 的 OH_*_Stop。若这些 Stop 任一个耗时/阻塞，主线程会一直卡在 join → APP THREAD_BLOCK_3S 被
    // 判冻结并杀进程。把整段 OH_*_Stop 挪到 detached 线程后，主线程立即返回、永不被编码器 Stop 阻塞。
    // TeardownPipelineUnlocked 锁内 std::move 剥离对象、锁外执行 Stop，天然线程安全且幂等。
    std::thread teardownThread([this]() {
        TeardownPipelineUnlocked();
        std::lock_guard<std::mutex> lock(mutex_);
        if (captureState_ != "idle") {
            captureState_ = "idle";
            EmitCaptureState("idle");
        }
        StopStatsLocked();
    });
    teardownThread.detach();
}

// —— 采集回调 ——

void MediaStreamEngine::OnCapturedVideo(const uint8_t *data, int width, int height, bool isNv12,
                                        int64_t ptsNs) {
    // 帧率节流：原始流按屏幕刷新率投递（可能远高于目标 fps），在引擎侧再兜底一次，
    // 控制编码输入速率≈目标 fps（采集回调侧节流在部分设备上不生效，实测仍 ~198fps）。
    int fps = config_.fps > 0 ? config_.fps : 30;
    if (fps > 0) {
        int64_t intervalNs = 1000000000LL / fps;
        int64_t now = NowNs();
        int64_t last = captureLastNs_.load(std::memory_order_relaxed);
        // 诊断：打印前若干帧的节流判定，确认本分支确实在执行
        static int diagThr = 0;
        bool drop = (last != 0 && now - last < intervalNs - intervalNs / 10);
        if (diagThr++ < 5) {
            MS_LOG_WARN("[THR] fps=%{public}d interval=%{public}lld last=%{public}lld now=%{public}lld drop=%{public}d",
                        fps, static_cast<long long>(intervalNs), static_cast<long long>(last),
                        static_cast<long long>(now), drop ? 1 : 0);
        }
        if (drop) {
            return; // 弃帧，保持目标帧率
        }
        captureLastNs_.store(now, std::memory_order_relaxed);
    }

    // 采集回调与管线收尾可能并发（收尾会清空 videoEncoder_），此处统一加锁读取并判空，
    // 避免悬垂指针；RGBA 暂存缓冲也在同锁内访问。
    std::lock_guard<std::mutex> lock(mutex_);
    if (videoEncoder_ == nullptr) {
        return;
    }
    int64_t ptsUs = ptsNs / 1000;
    int ew = encodeWidth_, eh = encodeHeight_;
    if (ew <= 0 || eh <= 0) {
        ew = width;
        eh = height;
    }

    // 先得到原生尺寸的 NV12（RGBA 时软件转换）
    const uint8_t *nv12 = data;
    if (!isNv12) {
        size_t need = static_cast<size_t>(width) * height * 3 / 2;
        if (rgbaToNv12Scratch_.size() < need) {
            rgbaToNv12Scratch_.resize(need);
        }
        extern void RgbaToNv12(const uint8_t *, size_t, int, int, uint8_t *, uint8_t *);
        RgbaToNv12(data, static_cast<size_t>(width) * 4, width, height, rgbaToNv12Scratch_.data(),
                   rgbaToNv12Scratch_.data() + static_cast<size_t>(width) * height);
        nv12 = rgbaToNv12Scratch_.data();
    }

    // 缩放到编码尺寸（原生 → 16 对齐小尺寸），保证编码流可被播放器解码
    if (ew != width || eh != height) {
        if (scaleNv12Scratch_.size() < static_cast<size_t>(ew) * eh * 3 / 2) {
            scaleNv12Scratch_.resize(static_cast<size_t>(ew) * eh * 3 / 2);
        }
        ScaleNv12(nv12, width, height, scaleNv12Scratch_.data(), ew, eh);
        nv12 = scaleNv12Scratch_.data();
    }

    // [DBG] 进编码器前的最终缓冲均值/中心采样（判断是否黑/绿帧）。
    // 新增色度抽样：绿色=亮Y(正确)+错UV。此处打印原始RGBA中心、缩放后NV12中心Y与UV，
    // 一次定位绿色是采集就错(RGBA)还是转换/编码(RGB→UV)错。
    {
        long sum = 0; int n = ew * eh; const uint8_t *p = nv12;
        for (int i = 0; i < n; i += ew) sum += p[i];
        int r0 = p[0], rMid = p[(eh / 2) * ew + (ew / 2)];
        // 缩放到编码尺寸后 NV12 的中心 UV（UV 平面在 Y 之后，交错 U,V）
        uint8_t cu = 0, cv = 0;
        if (ew > 1 && eh > 1) {
            const uint8_t *uvp = p + static_cast<size_t>(ew) * eh;
            const int cy = (eh / 2), cx = (ew / 2);
            cu = uvp[(cy / 2) * ew + (cx / 2) * 2];
            cv = uvp[(cy / 2) * ew + (cx / 2) * 2 + 1];
        }
        MS_LOG_WARN("[DBG] encY meancol=%{public}ld midY=%{public}d topY=%{public}d midU=%{public}d midV=%{public}d isNv12=%{public}d",
                    sum / (ew ? eh : 1), rMid, r0, cu, cv, isNv12 ? 1 : 0);
        // 采集原始 RGBA 抽样（非 NV12 采集时）：中心/左上角各4个通道，判断原始帧是否真内容
        if (!isNv12 && dumpFrames_.load() < 3) {
            auto px = [&](int x, int y) {
                if (x >= width || y >= height) return;
                const uint8_t *q = data + (static_cast<size_t>(y) * width + x) * 4;
                MS_LOG_WARN("[DBG] rgba[%{public}d,%{public}d]=RGBA(%{public}u,%{public}u,%{public}u,%{public}u)",
                            x, y, q[0], q[1], q[2], q[3]);
            };
            px(width / 2, height / 2);
            px(0, 0);
            px(width / 4, height / 4);
            dumpFrames_.fetch_add(1);
        }
    }

    videoEncoder_->InputFrame(nv12, ptsUs);
}

void MediaStreamEngine::OnCapturedInnerAudio(const uint8_t *pcm, int32_t bytes, int64_t ptsNs) {
    // [AUD] 内录音频到达监控（限 64 帧打一条，确认无声源时是否真无内录数据）
    static std::atomic<int> innerCtr{0};
    if ((innerCtr.fetch_add(1) & 0x3F) == 0) {
        MS_LOG_INFO("[AUD] inner audio bytes=%{public}d total=%{public}d", bytes, innerCtr.load());
    }
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
    // [AUD] 麦克风音频到达监控（限 64 帧打一条，确认 mic 模式数据是否真正流入）
    static std::atomic<int> micCtr{0};
    if ((micCtr.fetch_add(1) & 0x3F) == 0) {
        MS_LOG_INFO("[AUD] mic audio bytes=%{public}d total=%{public}d", bytes, micCtr.load());
    }
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
    Mp4Recorder *recorder = nullptr;
    {
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
        // 取出录制器指针，稍后锁外 finalize（写 moov）
        if (mp4Recorder_) {
            recorder = mp4Recorder_.get();
        }
    }
    // 用户通过系统胶囊停止录屏：必须在锁外调用 Mp4Recorder::Stop()，让写入线程执行
    // OH_AVMuxer_Stop 写出 moov。此前只发状态不改 muxer，导致文件只有 mdat 没有 moov，
    // 任何播放器都无法播放（无轨道/编码/时长信息）。
    if (recorder != nullptr) {
        recorder->Stop();
    }
}

// —— 编码回调 ——

void MediaStreamEngine::OnEncodedVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe) {
    encodedVideoFrames_++;
    encodedBytes_ += size;
    // 编码器输出 pts 实测恒为 0，不能用于时间线；改为以「输出墙钟时刻」相对「首帧输出时刻」
    // 的差值作为 PTS（μs）。这样 MP4 时长/播放速度与实际录制经过时间一致，不依赖实际帧率
    // （本机原始流按屏幕刷新率高频投递，采集侧节流不生效时帧率仍偏高，但 PTS 依然正确）。
    int64_t nowNs = NowNs();
    int64_t start = outPtsStartNs_.load();
    if (start < 0) {
        outPtsStartNs_.compare_exchange_strong(start, nowNs);
        start = nowNs;
    }
    int64_t newPtsUs = (nowNs - start) / 1000;
    // 分发：RTMP + MP4
    if (rtmpClient_) {
        rtmpClient_->SendVideo(data, size, newPtsUs, isKeyframe);
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (mp4Recorder_ && mp4Started_) {
            mp4Recorder_->WriteVideo(data, size, newPtsUs, isKeyframe);
        } else if (pendingRecord_ && !mp4Started_) {
            // 等待音频 ASC（muxer 未启动）期间缓存视频帧，待 muxer 启动后统一灌入
            PendingVideoSample s;
            s.data.assign(data, data + size);
            s.ptsUs = newPtsUs;
            s.isKeyframe = isKeyframe;
            pendingVideo_.push_back(std::move(s));
            while (pendingVideo_.size() > kPendingVideoCap) {
                pendingVideo_.pop_front();
            }
        }
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
    StartMuxerLocked();
}

void MediaStreamEngine::StartMuxerLocked() {
    // 真正启动 MP4 封装器的三个前置：
    //  1) 已有录制请求 pendingRecord_；2) muxer 尚未启动；3) 视频 avcC 已就绪（视频轨必需）。
    // 音频是可选轨：只要有 ASC 就加音频轨；无 ASC（如内录无播放、音频持续未产出）时在等待
    // kAscWaitTimeoutMs 后降级为仅视频轨，避免静音录制永远无法开始。
    if (!pendingRecord_ || mp4Started_ || avcC_.empty()) {
        return;
    }
    if (asc_.empty()) {
        int64_t waited = NowSteadyMs() - recordRequestSteadyMs_;
        if (waited < kAscWaitTimeoutMs) {
            return; // 音频 ASC 未到且未超时：继续缓存视频帧等待音频轨
        }
        MS_LOG_WARN("audio ASC not ready within %{public}lldms, record video-only", 
                    static_cast<long long>(kAscWaitTimeoutMs));
    } else {
        MS_LOG_INFO("StartMuxer with audio ASC %{public}zu bytes", asc_.size());
    }

    // 复用/替换 Mp4Recorder 前，先确保上一个录制器的写线程已退出，避免 reset 销毁
    // 仍在使用 this 的挂死写线程 (UAF)。stop 请求未发起时先 Stop() 让其 detach 后台收尾，
    // 再超时等待其真正退出。
    if (mp4Recorder_) {
        Mp4Recorder *old = mp4Recorder_.get();
        old->Stop();
        old->WaitWriteThreadDone(3000);
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
    if (!mp4Recorder_->Start(videosDir_, w, h, avcC_, asc_, std::move(mcb))) {
        mp4Recorder_.reset();
        pendingRecord_ = false;
        recordState_ = "error";
        EmitRecordState("error");
        return;
    }
    mp4Started_ = true;
    // muxer 已启动：把等待 ASC 期间缓存的视频帧灌入写队列（flush）
    const size_t flushed = pendingVideo_.size();
    while (!pendingVideo_.empty()) {
        PendingVideoSample &s = pendingVideo_.front();
        mp4Recorder_->WriteVideo(s.data.data(), static_cast<int32_t>(s.data.size()), s.ptsUs, s.isKeyframe);
        pendingVideo_.pop_front();
    }
    if (flushed > 0) {
        MS_LOG_INFO("muxer started, flushed %{public}zu buffered video samples", flushed);
    } else {
        MS_LOG_INFO("muxer started (no buffered samples)");
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

        // 等待音频 ASC 超时兜底：muxer 尚未启动（无声源或音频迟迟未产出）时按时启动，避免录制悬空
        if (pendingRecord_ && !mp4Started_) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (asc_.empty() && NowSteadyMs() - recordRequestSteadyMs_ >= kAscWaitTimeoutMs) {
                MS_LOG_WARN("stats: ASC wait timeout, force video-only muxer start");
            }
            StartMuxerLocked();
        }

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