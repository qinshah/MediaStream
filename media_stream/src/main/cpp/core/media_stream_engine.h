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
#include <deque>
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
    // 录制使用传入的配置（含 audioMode 等），避免录制时沿用陈旧/默认配置导致麦克风等音频源未启用
    bool StartRecording(const Config &config, int &errCode, std::string &errMsg);
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
    // 预建最近邻采样索引表（尺寸变化时重建），并把 RGBA 单趟转换+缩放到编码尺寸
    void BuildScaleTables(int sw, int sh, int dw, int dh);
    void RgbaToNv12Scaled(const uint8_t *src, size_t srcStride, uint8_t *dst);
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
    void StartMuxerLocked();            // 音视频编码配置齐备（或音频等待超时）后真正启动 muxer

    // 取 rtmpClient_ 的快照（锁内拷贝 shared_ptr）。
    // 停止/销毁路径会在锁内把 rtmpClient_ 置空并释放对象，而编码回调线程与统计线程
    // 是不持锁访问它的：直接裸读会「读到非空但对象已被释放」→ UAF 崩溃（表现为点停止
    // 推流时闪退）。改为短锁取快照后使用，对象生命周期由 shared_ptr 保证。
    std::shared_ptr<RtmpClient> RtmpSnapshotLocked();

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
    // 组装并上报一条统计事件（须持 mutex_ 调用）。withRates=false 用于停止回调里只推终值，
    // 不带瞬时速率（瞬时速率由 UI 在状态回到空闲时清零）。
    void EmitStatsLocked(bool withRates);
    // 在「推流已停止/出错」的回调里冻结推流时长终值并即时上报（幂等）
    void FreezeStreamDurationLocked();
    // 单调时钟毫秒（与 streamStartMs_/Mp4Recorder 的时长口径一致）
    static int64_t SteadyMs();
    // 采集回调的绝对时间戳（steady ns）换算到会话原点；音频侧与视频共用同一原点
    int64_t SessionRelativeNs(int64_t absNs);

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
    std::shared_ptr<RtmpClient> rtmpClient_; // shared_ptr：调用方可持快照跨锁使用（见 RtmpSnapshotLocked）
    std::unique_ptr<Mp4Recorder> mp4Recorder_;

    // 编码配置缓存（MP4 AddTrack / RTMP sequence header / 重连重发三处共享）
    std::vector<uint8_t> avcC_;
    std::vector<uint8_t> asc_;
    bool mp4Started_ = false;
    bool pendingRecord_ = false; // 编码配置未齐时挂起录制请求

    // 无声音根因：MP4 muxer 需在 AddTrack 前确定音频 ASC，而 ASC 要待真实音频采集后才由编码器产出，
    // 通常晚于视频 avcC。若在 avcC 就绪时立即建 muxer，会因 asc 尚空而丢失音频轨（录制静音）。
    // 因此启动 muxer 前先缓存已编码视频样本到 pendingVideo_，等待 asc（或等待超时降级为仅视频轨）。
    struct PendingVideoSample {
        std::vector<uint8_t> data;
        int64_t ptsUs = 0;
        bool isKeyframe = false;
    };
    std::deque<PendingVideoSample> pendingVideo_;
    static constexpr size_t kPendingVideoCap = 120; // 等待期视频缓存上限（~4s@30fps）

    // RGBA 兜底转换暂存
    std::vector<uint8_t> rgbaToNv12Scratch_;

    // 编码尺寸（可能小于采集原生尺寸）与 NV12 缩放暂存
    int encodeWidth_ = 0;
    int encodeHeight_ = 0;
    std::vector<uint8_t> scaleNv12Scratch_;

    // ── 输出起点补帧 ──
    // 录屏采集由「屏幕内容变化」驱动：屏幕静止时采集侧根本不产帧。于是「采集已在跑、之后
    // 才点录制」时，视频轨开头会空一段（真机实测：文件里第一帧出现在 11.3s，播放时前 11s
    // 只有声音没有画面，用户看到的就是「录制的第一秒，屏幕上的计时已经跑到 3s」）。
    // 这里缓存最近一帧编码尺寸 NV12，在输出（录制）起点补投一次，让视频轨从 0 就有画面。
    std::vector<uint8_t> lastFrame_;
    int lastFrameW_ = 0;
    int lastFrameH_ = 0;
    bool lastFrameValid_ = false;
    // 补投缓存帧（须持 mutex_）
    void SubmitLastFrameLocked(const char *why);

    // 最近邻采样索引表（目标行/列 → 源行/列）。预建一次即可，避免内层循环里做整数除法：
    // debug 构建(-O0)下每像素一次除法就能把单帧转换拖到数百毫秒。
    int scaleTableSrcW_ = 0, scaleTableSrcH_ = 0, scaleTableDstW_ = 0, scaleTableDstH_ = 0;
    std::vector<int> scaleColIdx_;
    std::vector<int> scaleRowIdx_;
    

    // 统计
    std::thread statsThread_;
    std::atomic<bool> statsRunning_{false};
    std::atomic<int64_t> encodedVideoFrames_{0};
    std::atomic<int64_t> encodedBytes_{0};
    std::atomic<int64_t> streamStartMs_{-1};
    // 推流时长冻结时刻（steady ms；0=未冻结）。只在「推流已停止/出错」回调里落值：
    // 若照旧每 500ms 按 now 计算，推流一旦意外中断（RTMP 报错、采集被系统停止）rtmpClient_
    // 仍在、统计线程仍在跑，界面上的时长会一直涨下去。
    std::atomic<int64_t> streamStopMs_{0};
    // 录制时长终值（onFinished 的 result.durationMs；-1=本段录制尚未产出终值）
    std::atomic<int64_t> recordFinalMs_{-1};
    double lastFps_ = 0;
    double lastBitrateKbps_ = 0;
    int64_t lastStatsFrames_ = 0;
    int64_t lastStatsBytes_ = 0;

    // 帧率节流 + 会话时间轴原点。
    // 全链路只共用这一把时钟：视频 PTS = (输出墙钟 - 原点)，音频的采集时间戳也换算到同一
    // 原点后再喂编码器。此前视频用「本会话首帧输出墙钟」、音频则沿用编码器自有时钟，两者
    // 原点能相差几十秒（真机抓到同一录制文件里音频 rawPts=88.85s、视频 rawPts=0），时间戳
    // 被夹到 0 或跳到远端 → 播放器为对齐而反复丢/补帧（听感即电音），录制文件开头也缺画面。
    std::atomic<int64_t> captureLastNs_{0};
    std::atomic<int> dumpFrames_{0}; // [DBG] 采集原始 RGBA 抽样帧计数（限前3帧）
    std::atomic<int> dbgFrameCtr_{0}; // [DBG] 进编码器缓冲采样计数（降频：每 30 帧一条）
    std::atomic<int64_t> sessionStartNs_{-1}; // 管线启动即确定，会话内不再重置

    // 麦克风降级检测
    std::atomic<bool> micDegraded_{false};
    // 采集真正开始的时刻（steady_clock ns，来自 AVScreenCapture 的 STARTED 回调）。
    // 必须以此而非管线创建时刻做超时基准：Start() 返回到用户授权存在 12~14s 空窗，
    // 以管线创建为基准会让「麦克风无数据」判定在用户还没授权时就误报。
    std::atomic<int64_t> captureActiveNs_{-1};
};

} // namespace media_stream

#endif // MEDIA_STREAM_ENGINE_H
