#ifndef MEDIA_STREAM_ENGINE_H
#define MEDIA_STREAM_ENGINE_H

// 会话引擎（控制面核心）
//  - 三平面状态机：capture（idle/starting/active/stopping/error）、stream（idle/connecting/
//    streaming/reconnecting/stopped/error）、record（idle/recording/stopped/error）
//  - 生命周期：首个输出启动时拉起采集+编码；末个输出停止后全量释放（FR-014）
//  - FanOut 分发：编码输出同时供 RTMP 与 MP4 两路
//  - 时间戳三层换算：采集 ns → 编码 μs → MP4 μs / RTMP ms
//  - 防重入幂等；麦克风无数据降级检测（micDegraded）；destroy 幂等

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../audio/audio_mixer.h"
#include "../capture/screen_capture.h"
#include "../codec/audio_encoder.h"
#include "../codec/video_encoder.h"
#include "../muxer/mp4_recorder.h"
#include "../rtmp/rtmp_client.h"

namespace media_stream {

class JsEventEmitter;

// ErrorCode 与 Index.d.ts 对齐
enum class EngineError {
    kInvalidArg = 0,
    kBusy = 1,
    kCaptureInitFail = 2,
    kCapturePermissionDenied = 3,
    kMicPermissionDenied = 4,
    kEncoderInitFail = 5,
    kEncoderError = 6,
    kRtmpUrlInvalid = 7,
    kRtmpConnectFail = 8,
    kRtmpTimeout = 9,
    kRtmpAuthFail = 10,
    kStorageFull = 11,
    kInternalError = 12,
};

class MediaStreamEngine {
public:
    struct Config {
        std::string rtmpUrl;
        std::string preset = "720p"; // '1080p' | '720p' | '480p'（短边目标值）
        int fps = 30;
        int videoBitrateKbps = 4000;
        std::string audioMode = "inner"; // 'mic' | 'inner' | 'micInner'
    };

    static MediaStreamEngine &Instance();

    void SetEventEmitter(JsEventEmitter *emitter);
    void Init(const std::string &filesDir);

    // 全部幂等防重入；失败返回 false 并填充 errCode/errMsg
    bool StartStreaming(const Config &config, int &errCode, std::string &errMsg);
    void StopStreaming();
    bool StartRecording(int &errCode, std::string &errMsg);
    void StopRecording();
    void Destroy();

private:
    MediaStreamEngine() = default;

    // 采集+编码管线拉起/释放
    bool EnsureCapturePipelineLocked(int &errCode, std::string &errMsg);
    void TeardownPipelineUnlocked(); // 锁内剥离指针、锁外执行 OH_*_Stop（避免回调加锁死锁）
    void MaybeStopPipelineUnlocked(); // 两输出均停 → 释放采集编码（须在未持有互斥量时调用）

    // 采集回调挂接
    void OnCapturedVideo(const uint8_t *data, int width, int height, bool isNv12, int64_t ptsNs);
    void OnCapturedInnerAudio(const uint8_t *pcm, int32_t bytes, int64_t ptsNs);
    void OnCapturedMicAudio(const uint8_t *pcm, int32_t bytes, int64_t ptsNs);
    void OnCaptureError(int32_t errorCode);
    void OnCaptureUserStopped();

    // 编码回调挂接
    void OnEncodedVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe);
    void OnEncodedAudio(const uint8_t *data, int32_t size, int64_t ptsUs);
    void OnAvccReady(const std::vector<uint8_t> &avcC);
    void OnAscReady(const std::vector<uint8_t> &asc);

    // 录制完成回调
    void OnRecordFinished(const Mp4Recorder::Result &result);
    void EmitRecordFinishedEvent(const Mp4Recorder::Result &result);
    void TryStartPendingRecordLocked(); // 编码配置齐全后启动挂起的录制

    // 事件上报
    void EmitCaptureState(const char *state);
    void EmitStreamState(const char *state, int reconnectAttempt = 0);
    void EmitRecordState(const char *state);
    void EmitError(EngineError code, const std::string &message, const char *source);
    void EmitMicDegraded();

    // 统计线程：500ms 节流上报
    void StatsThreadMain();
    void StartStatsLocked();
    void StopStatsLocked();

    // 按屏幕原生分辨率确定采集尺寸（原始流要求与显示器一致）
    bool ComputeCaptureSize(int &width, int &height);
    // 计算编码/MP4 目标尺寸：原生长边裁剪到 kEncodeLongMax，宽高取 16 对齐
    // （OH H.264 编码器对非 16 对齐的大尺寸帧会输出错乱流 → 播放绿屏/失败）
    static void ComputeEncodeSize(int nativeW, int nativeH, int &encW, int &encH);

    std::mutex mutex_; // 保护全部状态与对象生命周期

    JsEventEmitter *emitter_ = nullptr; // 不持有所有权（由 NAPI 层管理）
    std::string filesDir_;
    std::string videosDir_;
    bool inited_ = false;

    Config config_;
    Config lastConfig_; // startRecording 复用最近一次 config

    std::string captureState_ = "idle";
    std::string streamState_ = "idle";
    std::string recordState_ = "idle";

    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<VideoEncoder> videoEncoder_;
    std::unique_ptr<AudioEncoder> audioEncoder_;
    std::unique_ptr<AudioMixer> mixer_;
    std::unique_ptr<RtmpClient> rtmpClient_;
    std::unique_ptr<Mp4Recorder> mp4Recorder_;

    // 编码配置缓存（MP4 AddTrack / RTMP sequence header / 重连重发三处共享）
    std::vector<uint8_t> avcC_;
    std::vector<uint8_t> asc_;
    bool mp4Started_ = false;
    bool pendingRecord_ = false; // 编码配置未齐时挂起录制请求

    // RGBA 兜底转换暂存
    std::vector<uint8_t> rgbaToNv12Scratch_;

    // 编码尺寸（可能小于采集原生尺寸）与 NV12 缩放暂存
    int encodeWidth_ = 0;
    int encodeHeight_ = 0;
    std::vector<uint8_t> scaleNv12Scratch_;
    

    // 统计
    std::thread statsThread_;
    std::atomic<bool> statsRunning_{false};
    std::atomic<int64_t> encodedVideoFrames_{0};
    std::atomic<int64_t> encodedBytes_{0};
    std::atomic<int64_t> streamStartMs_{-1};
    double lastFps_ = 0;
    double lastBitrateKbps_ = 0;
    int64_t lastStatsFrames_ = 0;
    int64_t lastStatsBytes_ = 0;

    // 帧率节流 + 输出 PTS 基线：PTS 按「输出时刻 - 首帧输出时刻」的墙钟差生成（μs），
    // 保证 MP4 时长/播放速度正确（编码器透传 pts 恒为 0；按帧号×间隔会因实际帧率≠fps 而失真）
    std::atomic<int64_t> captureLastNs_{0};
    std::atomic<int64_t> outPtsStartNs_{ -1 };

    // 麦克风降级检测
    std::atomic<bool> micDegraded_{false};
};

} // namespace media_stream

#endif // MEDIA_STREAM_ENGINE_H
