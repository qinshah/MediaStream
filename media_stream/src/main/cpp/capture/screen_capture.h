#ifndef MEDIA_STREAM_SCREEN_CAPTURE_H
#define MEDIA_STREAM_SCREEN_CAPTURE_H

// AVScreenCapture 原始流采集封装
// 要点（参考 Harmony-OBS-Studio/plugins/ohos-capture 已验证做法）：
//  - dataType=OH_ORIGINAL_STREAM，NV12（SURFACE_YUV）优先，Init 拒绝时回退 RGBA
//  - 视频缓冲回调：帧率节流（系统按刷新率投递，可能高于目标 fps）、按容量探测
//    首帧格式（RGBA=w*h*4 / NV12=w*h*1.5）、行距按 NativeBuffer 元数据/容量推断、
//    紧凑化拷贝后回调上层；回调 buffer 必须 OH_AVBuffer_Destroy 归还，否则缓冲池耗尽冻结
//  - 音频：内录 + 麦克风两路独立 PCM 回调（s16le 48k 双声道交错）
//  - 回调 timestamp 单位不可靠，统一取到达时刻（steady_clock ns）

#include <cstdint>
#include <functional>
#include <mutex>
#include <atomic>

struct OH_AVScreenCapture;
struct OH_AVBuffer;

namespace media_stream {

class ScreenCapture {
public:
    // 视频帧回调：data 为紧凑打包的 NV12 或 RGBA 数据（isNv12 区分），ptsNs 为到达时刻
    using VideoFrameCallback =
        std::function<void(const uint8_t *data, int width, int height, bool isNv12, int64_t ptsNs)>;
    // 音频帧回调：s16le 48kHz 双声道交错 PCM
    using AudioFrameCallback = std::function<void(const uint8_t *pcm, int32_t bytes, int64_t ptsNs)>;
    using ErrorCallback = std::function<void(int32_t errorCode)>;
    using UserStoppedCallback = std::function<void()>;

    struct Callbacks {
        VideoFrameCallback onVideoFrame;
        AudioFrameCallback onInnerAudio;
        AudioFrameCallback onMicAudio;
        ErrorCallback onError;
        UserStoppedCallback onUserStopped; // 用户经系统途径停止采集（如控制栏停止）
    };

    struct Config {
        int width = 0;      // 采集宽度（偶数，按屏幕短边等比缩放后由引擎给出）
        int height = 0;     // 采集高度（偶数）
        int fps = 30;       // 目标帧率
        bool enableMic = false;   // 是否采集麦克风
        bool enableInner = true;  // 是否采集系统内录
    };

    ScreenCapture() = default;
    ~ScreenCapture();

    // 启动采集（触发系统授权弹窗）；失败返回 false 并触发 onError
    bool Start(const Config &config, Callbacks callbacks);
    // 停止采集（幂等）
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // 实际采集尺寸（Init 前对齐屏幕，回调尺寸可能与请求不同由首帧探测修正）
    void GetSize(int &width, int &height) const {
        width = width_;
        height = height_;
    }

    // 系统显示尺寸查询（默认显示屏物理分辨率）
    static bool QueryDisplaySize(int32_t &width, int32_t &height);

    bool IsUsingMic() const { return enableMic_; }

    // 供 cpp 内文件静态回调桥接函数转发上层事件
    void DispatchError(int32_t errorCode) {
        if (callbacks_.onError) {
            callbacks_.onError(errorCode);
        }
    }
    void DispatchUserStopped() {
        if (callbacks_.onUserStopped) {
            callbacks_.onUserStopped();
        }
    }

    // 供 cpp 内文件静态回调桥接函数调用的缓冲处理入口
    // handle 函数参数统一使用 OH_AVBuffer *（与 SDK 原始流缓冲类型一致）
    void HandleVideoBuffer(OH_AVBuffer *buffer);
    void HandleAudioBuffer(OH_AVBuffer *buffer, bool isMic);

private:

    OH_AVScreenCapture *capture_ = nullptr;
    Callbacks callbacks_;
    std::atomic<bool> running_{false};

    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    bool enableMic_ = false;

    std::atomic<bool> wantNv12_{true};
    std::atomic<int> frameFormat_{0}; // 0=未定 1=RGBA 2=NV12（会话内锁存）
    std::atomic<int64_t> lastVideoNs_{0}; // 帧率节流：上一帧放行时刻
};

} // namespace media_stream

#endif // MEDIA_STREAM_SCREEN_CAPTURE_H
