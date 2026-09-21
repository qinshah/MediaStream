#include "screen_capture.h"

#include <chrono>
#include <cstring>
#include <vector>

#include <multimedia/player_framework/native_avscreen_capture.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <native_buffer/native_buffer.h>
#include <window_manager/oh_display_manager.h>

#include "../common/logger.h"

namespace media_stream {

// AVScreenCapture 状态码（与 OH_AVScreenCaptureStateCode 对齐，避免依赖整头文件枚举名差异）
static constexpr int kStateStoppedByUser = OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER;
static constexpr int kStateStoppedByUserSwitches = OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER_SWITCHES;

// AVScreenCapture C 回调文件静态桥接函数（SDK 要求与 OH_AVScreenCapture_On* 精确匹配）
static void OnBufferAvailable(OH_AVScreenCapture *capture, OH_AVBuffer *buffer,
                              OH_AVScreenCaptureBufferType bufferType, int64_t timestamp, void *userData);
static void OnError(OH_AVScreenCapture *capture, int32_t errorCode, void *userData);
static void OnStateChange(OH_AVScreenCapture *capture, OH_AVScreenCaptureStateCode stateCode, void *userData);

ScreenCapture::~ScreenCapture() {
    Stop();
}

bool ScreenCapture::QueryDisplaySize(int32_t &width, int32_t &height) {
    int32_t w = 0, h = 0;
    bool ok = OH_NativeDisplayManager_GetDefaultDisplayWidth(&w) == DISPLAY_MANAGER_OK &&
              OH_NativeDisplayManager_GetDefaultDisplayHeight(&h) == DISPLAY_MANAGER_OK && w >= 64 && h >= 64;
    if (ok) {
        width = w;
        height = h;
    }
    return ok;
}

static int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ScreenCapture::Start(const Config &config, Callbacks callbacks) {
    if (running_.exchange(true)) {
        MS_LOG_WARN("ScreenCapture already running, ignore Start");
        return true;
    }
    callbacks_ = std::move(callbacks);
    width_ = config.width;
    height_ = config.height;
    fps_ = config.fps > 0 ? config.fps : 30;
    enableMic_ = config.enableMic;
    frameFormat_.store(0);
    lastVideoNs_.store(0);
    wantNv12_.store(true);

    capture_ = OH_AVScreenCapture_Create();
    if (capture_ == nullptr) {
        MS_LOG_ERROR("OH_AVScreenCapture_Create failed");
        running_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(AV_SCREEN_CAPTURE_ERR_NO_MEMORY);
        }
        return false;
    }

    // HOME_SCREEN 模式要求 videoFrameWidth/Height 与真实显示器一致（否则 Init 可能返回
    // OPERATE_NOT_PERMIT）；以显示尺寸为准做偶数对齐
    int32_t dw = 0, dh = 0;
    if (QueryDisplaySize(dw, dh)) {
        MS_LOG_INFO("display size %{public}dx%{public}d, requested %{public}dx%{public}d", dw, dh, width_,
                    height_);
    }

    OH_AVScreenCaptureConfig capConfig = {};
    capConfig.captureMode = OH_CAPTURE_HOME_SCREEN;
    capConfig.dataType = OH_ORIGINAL_STREAM;

    // 音频：内录必填（audioSource=OH_SOURCE_INVALID 过不了配置校验）；
    // micCapInfo 恒填 OH_MIC，实际开关由 SetMicrophoneEnabled 控制
    capConfig.audioInfo.innerCapInfo.audioSampleRate = 48000;
    capConfig.audioInfo.innerCapInfo.audioChannels = 2;
    capConfig.audioInfo.innerCapInfo.audioSource = config.enableInner ? OH_ALL_PLAYBACK : OH_APP_PLAYBACK;
    capConfig.audioInfo.micCapInfo.audioSampleRate = 48000;
    capConfig.audioInfo.micCapInfo.audioChannels = 2;
    capConfig.audioInfo.micCapInfo.audioSource = OH_MIC;
    capConfig.audioInfo.audioEncInfo.audioBitrate = 128000;
    capConfig.audioInfo.audioEncInfo.audioCodecformat = OH_AAC_LC;

    // 采集尺寸以引擎缩放结果为准（短边预设映射后的偶数尺寸）
    capConfig.videoInfo.videoCapInfo.videoFrameWidth = width_;
    capConfig.videoInfo.videoCapInfo.videoFrameHeight = height_;
    capConfig.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_YUV;
    // 原始流模式下编码参数被忽略，但校验要求合法枚举
    capConfig.videoInfo.videoEncInfo.videoCodec = OH_H264;
    capConfig.videoInfo.videoEncInfo.videoBitrate = 20000000;
    capConfig.videoInfo.videoEncInfo.videoFrameRate = fps_;

    int32_t rc = OH_AVScreenCapture_Init(capture_, capConfig);
    if (rc != AV_SCREEN_CAPTURE_ERR_OK) {
        // SURFACE_YUV 被拒时回退 RGBA 重 Init（部分设备原始流仅接受 RGBA）
        MS_LOG_WARN("Init rc=%{public}d with SURFACE_YUV, fallback to RGBA", rc);
        wantNv12_.store(false);
        capConfig.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;
        rc = OH_AVScreenCapture_Init(capture_, capConfig);
    }
    if (rc != AV_SCREEN_CAPTURE_ERR_OK) {
        MS_LOG_ERROR("OH_AVScreenCapture_Init failed rc=%{public}d", rc);
        OH_AVScreenCapture_Release(capture_);
        capture_ = nullptr;
        running_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(rc);
        }
        return false;
    }

    OH_AVScreenCapture_SetMicrophoneEnabled(capture_, enableMic_);
    OH_AVScreenCapture_SetDataCallback(capture_, OnBufferAvailable, this);
    OH_AVScreenCapture_SetErrorCallback(capture_, OnError, this);
    OH_AVScreenCapture_SetStateCallback(capture_, OnStateChange, this);

    // StartScreenCapture 触发系统录屏授权弹窗；用户拒绝时经错误回调上报
    rc = OH_AVScreenCapture_StartScreenCapture(capture_);
    if (rc != AV_SCREEN_CAPTURE_ERR_OK) {
        MS_LOG_ERROR("OH_AVScreenCapture_StartScreenCapture failed rc=%{public}d", rc);
        OH_AVScreenCapture_Release(capture_);
        capture_ = nullptr;
        running_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(rc);
        }
        return false;
    }
    MS_LOG_INFO("ScreenCapture started %{public}dx%{public}d@%{public}d mic=%{public}d nv12=%{public}d", width_,
                height_, fps_, enableMic_ ? 1 : 0, wantNv12_.load() ? 1 : 0);
    return true;
}

void ScreenCapture::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (capture_ != nullptr) {
        OH_AVScreenCapture_StopScreenCapture(capture_);
        OH_AVScreenCapture_Release(capture_);
        capture_ = nullptr;
    }
    MS_LOG_INFO("ScreenCapture stopped");
}

// —— C 回调桥接 ——

static void OnBufferAvailable(OH_AVScreenCapture *capture, OH_AVBuffer *buffer,
                              OH_AVScreenCaptureBufferType bufferType, int64_t timestamp, void *userData) {
    (void)capture;
    (void)timestamp; // 单位不可靠，统一取到达时刻
    auto *self = static_cast<ScreenCapture *>(userData);
    if (self == nullptr || buffer == nullptr) {
        if (buffer != nullptr) {
            OH_AVBuffer_Destroy(buffer);
        }
        return;
    }
    if (!self->IsRunning()) {
        OH_AVBuffer_Destroy(buffer);
        return;
    }
    switch (bufferType) {
        case OH_SCREEN_CAPTURE_BUFFERTYPE_VIDEO:
            self->HandleVideoBuffer(buffer);
            break;
        case OH_SCREEN_CAPTURE_BUFFERTYPE_AUDIO_INNER:
            self->HandleAudioBuffer(buffer, false);
            break;
        case OH_SCREEN_CAPTURE_BUFFERTYPE_AUDIO_MIC:
            if (self->IsUsingMic()) {
                self->HandleAudioBuffer(buffer, true);
            } else {
                OH_AVBuffer_Destroy(buffer);
            }
            break;
        default:
            OH_AVBuffer_Destroy(buffer);
            break;
    }
}

static void OnError(OH_AVScreenCapture *capture, int32_t errorCode, void *userData) {
    (void)capture;
    auto *self = static_cast<ScreenCapture *>(userData);
    MS_LOG_WARN("AVScreenCapture error %{public}d", errorCode);
    if (self != nullptr) {
        self->DispatchError(errorCode);
    }
}

static void OnStateChange(OH_AVScreenCapture *capture, OH_AVScreenCaptureStateCode stateCode, void *userData) {
    (void)capture;
    auto *self = static_cast<ScreenCapture *>(userData);
    MS_LOG_INFO("AVScreenCapture state -> %{public}d", static_cast<int>(stateCode));
    if (self == nullptr) {
        return;
    }
    if (stateCode == static_cast<OH_AVScreenCaptureStateCode>(kStateStoppedByUser) ||
        stateCode == static_cast<OH_AVScreenCaptureStateCode>(kStateStoppedByUserSwitches)) {
        self->DispatchUserStopped();
    }
}

// —— 视频缓冲处理：节流 → 格式探测 → 行距紧凑化 → 回调 → 归还 buffer ——

void ScreenCapture::HandleVideoBuffer(OH_AVBuffer *buffer) {
    // 帧率节流：原始流按屏幕刷新率投递（可能 ~50-120fps），按目标间隔提前丢弃
    // 门限 85% 防时钟抖动丢帧
    int64_t now = NowNs();
    if (fps_ > 0) {
        int64_t interval = 1000000000LL / fps_;
        int64_t last = lastVideoNs_.load(std::memory_order_relaxed);
        bool drop = (last != 0 && now - last < interval - interval / 10);
        static int diagSc = 0;
        if (diagSc++ < 5) {
            MS_LOG_WARN("[SC] fps=%{public}d last=%{public}lld now=%{public}lld drop=%{public}d", fps_,
                        static_cast<long long>(last), static_cast<long long>(now), drop ? 1 : 0);
        }
        if (drop) {
            OH_AVBuffer_Destroy(buffer);
            return;
        }
        lastVideoNs_.store(now, std::memory_order_relaxed);
    }

    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);
    int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
    if (addr == nullptr || capacity <= 0) {
        OH_AVBuffer_Destroy(buffer);
        return;
    }

    const int w = width_;
    const int h = height_;

    // 首帧格式探测（会话内锁存）：按容量判别 RGBA(w*h*4) / NV12(w*h*1.5)
    int fmt = frameFormat_.load(std::memory_order_relaxed);
    if (fmt == 0) {
        if (static_cast<size_t>(capacity) >= static_cast<size_t>(w) * h * 4) {
            fmt = 1;
        } else if (static_cast<size_t>(capacity) >= static_cast<size_t>(w) * h * 3 / 2) {
            fmt = 2;
        } else {
            fmt = -1;
        }
        frameFormat_.store(fmt, std::memory_order_relaxed);
        MS_LOG_INFO("frame format detected: %{public}s (capacity=%{public}d %{public}dx%{public}d)",
                    fmt == 1 ? "RGBA" : fmt == 2 ? "NV12" : "unknown", capacity, w, h);
    }
    size_t need = fmt == 2 ? static_cast<size_t>(w) * h * 3 / 2 : static_cast<size_t>(w) * h * 4;
    if (fmt == -1 || static_cast<size_t>(capacity) < need) {
        // 分辨率尚未同步等过渡期：跳帧但必须归还 buffer
        OH_AVBuffer_Destroy(buffer);
        return;
    }

    // 紧凑化拷贝缓冲（回调线程复用，避免每帧分配）
    static thread_local std::vector<uint8_t> packed;
    if (packed.size() < need) {
        packed.resize(need);
    }

    if (fmt == 2) {
        // NV12：Y 平面 w*h + UV 交错平面 w*h/2。行距优先取 NativeBuffer 元数据真值，
        // 容量推断在原生分辨率尺寸下可能算错（如 1224x2776 会推出的行距≠真实值），导致斜条纹撕裂。
        size_t yStride = 0;
        OH_NativeBuffer *nb = OH_AVBuffer_GetNativeBuffer(buffer);
        if (nb != nullptr) {
            OH_NativeBuffer_Config nbConfig = {};
            OH_NativeBuffer_GetConfig(nb, &nbConfig);
            if (nbConfig.stride >= w) {
                yStride = static_cast<size_t>(nbConfig.stride);
            }
        }
        if (yStride == 0) {
            yStride = (static_cast<size_t>(capacity) * 2) / (static_cast<size_t>(h) * 3);
            yStride = (yStride + 15u) & ~static_cast<size_t>(15u);
            if (yStride < static_cast<size_t>(w)) {
                yStride = static_cast<size_t>(w);
            }
        }
        uint8_t *dstY = packed.data();
        for (int row = 0; row < h; row++) {
            memcpy(dstY + static_cast<size_t>(row) * w, addr + static_cast<size_t>(row) * yStride, w);
        }
        const uint8_t *uvSrc = addr + yStride * h;
        uint8_t *dstUV = packed.data() + static_cast<size_t>(w) * h;
        for (int row = 0; row < h / 2; row++) {
            memcpy(dstUV + static_cast<size_t>(row) * w, uvSrc + static_cast<size_t>(row) * yStride, w);
        }
    } else {
        // RGBA：行距取 NativeBuffer 元数据真值（容量推断在部分尺寸下估错会导致斜条纹撕裂）
        size_t stride = 0;
        OH_NativeBuffer *nb = OH_AVBuffer_GetNativeBuffer(buffer);
        if (nb != nullptr) {
            OH_NativeBuffer_Config nbConfig = {};
            OH_NativeBuffer_GetConfig(nb, &nbConfig);
            if (nbConfig.stride >= w * 4) {
                stride = static_cast<size_t>(nbConfig.stride);
            }
        }
        if (stride == 0) {
            stride = (static_cast<size_t>(capacity) / h) & ~static_cast<size_t>(15);
            if (stride < static_cast<size_t>(w) * 4) {
                stride = static_cast<size_t>(w) * 4;
            }
        }
        for (int row = 0; row < h; row++) {
            memcpy(packed.data() + static_cast<size_t>(row) * w * 4,
                   addr + static_cast<size_t>(row) * stride, static_cast<size_t>(w) * 4);
        }
    }

    if (callbacks_.onVideoFrame) {
        callbacks_.onVideoFrame(packed.data(), w, h, fmt == 2, now);
    }
    // 原始流协议：buffer 用完必须归还，否则缓冲池耗尽后投递停止
    OH_AVBuffer_Destroy(buffer);
}

void ScreenCapture::HandleAudioBuffer(OH_AVBuffer *buffer, bool isMic) {
    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);
    int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
    if (addr != nullptr && capacity > 0) {
        const auto &cb = isMic ? callbacks_.onMicAudio : callbacks_.onInnerAudio;
        if (cb) {
            cb(addr, capacity, NowNs());
        }
    }
    OH_AVBuffer_Destroy(buffer);
}

} // namespace media_stream
