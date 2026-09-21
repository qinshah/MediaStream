#ifndef MEDIA_STREAM_SCREEN_CAPTURE_H
#define MEDIA_STREAM_SCREEN_CAPTURE_H

// AVScreenCapture 原始流采集封装
// 要点（参考 Harmony-OBS-Studio/plugins/ohos-capture 已验证做法）：
//  - dataType=OH_ORIGINAL_STREAM，NV12（SURFACE_YUV）优先，Init 拒绝时回退 RGBA
//  - 视频缓冲回调：帧率节流（系统按刷新率投递，可能高于目标 fps）、按容量探测
//    首帧格式（RGBA=w*h*4 / NV12=w*h*1.5）、行距按 NativeBuffer 元数据/容量推断、
//    紧凑化拷贝后回调上层。**缓冲由框架回收**：SetDataCallback 文档明确「回调触发后
//    buffer 即失效」，应用不得 OH_AVBuffer_Destroy（销毁会打乱框架缓冲池记账 →
//    投递被压到 0.4fps 且缓冲被提前清零，YUV 全 0 解码成纯绿）
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
    // 采集真正开始（系统授权通过、录屏服务拉起完成）。注意它比 Start() 返回晚很多 ——
    // 中间隔着用户点「允许」的等待，实测 12~14s。任何「某路音频/视频多久没数据」的超时判定
    // 都必须以此为基准，否则会在用户还没授权时就判超时。
    using StartedCallback = std::function<void()>;

    struct Callbacks {
        VideoFrameCallback onVideoFrame;
        AudioFrameCallback onInnerAudio;
        AudioFrameCallback onMicAudio;
        ErrorCallback onError;
        UserStoppedCallback onUserStopped; // 用户经系统途径停止采集（如控制栏停止）
        StartedCallback onStarted;         // 采集真正开始（授权通过）
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

    // 在 STATE_STARTED 回调里限制系统投递帧率上限。不设时系统按屏幕刷新率投递，
    // 而本设备 SURFACE_YUV 会被拒 → 走 RGBA 软件通路（1224x2776 每帧 14MB），
    // 高频投递会把采集回调线程压满、有效帧率掉到个位数（真机实测）。
    void ApplyMaxFrameRate(OH_AVScreenCapture *capture);

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
    void DispatchStarted() {
        if (callbacks_.onStarted) {
            callbacks_.onStarted();
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

    // 投递速率探针（每 5s 汇总一条）：recv=系统投递帧数、pass=节流放行帧数。
    // 二者差值即节流丢弃量；pass 明显低于目标 fps 说明系统按「簇发」投递
    // （真机实测：十余帧挤在数毫秒内投递，随后空窗百余毫秒 —— 最小间隔式节流
    // 会把一簇里除首帧外全部丢掉，有效帧率掉到 5~6fps）。计数为 per-instance，
    // 不用 static，避免跨会话污染（真机踩过：static 首帧计数被前一会话带偏）。
    void ProbeVideoFrame(int64_t nowNs, bool passed);
    std::atomic<int64_t> probeRecv_{0};
    std::atomic<int64_t> probePass_{0};
    std::atomic<int64_t> probeLastLogNs_{0};
    std::atomic<int64_t> probeWinRecv_{0};
    std::atomic<int64_t> probeWinPass_{0};

    // 音频到达节奏诊断（每 64 包一条日志）：包长 ÷ 到达间隔 应与声明的 48000Hz 自洽。
    // 若实测偏离，说明系统投递的实际采样率/包长与我们的假设不符 —— 混音器按固定 20ms
    // 块对齐两路输入时，节奏不一致会反复空转（一路饿死）或持续丢样本，听感就是电音/断续。
    void LogAudioCadence(bool isMic, int32_t bytes, int64_t nowNs);
    std::atomic<int64_t> audioLastNs_[2];
    std::atomic<int64_t> audioAccNs_[2];
    std::atomic<int> audioCount_[2];
};

} // namespace media_stream

#endif // MEDIA_STREAM_SCREEN_CAPTURE_H
