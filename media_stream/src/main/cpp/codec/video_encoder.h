#ifndef MEDIA_STREAM_VIDEO_ENCODER_H
#define MEDIA_STREAM_VIDEO_ENCODER_H

// OH_VideoEncoder H.264 硬件编码封装（buffer 模式）
//  - 输入：NV12 帧（QueryInputBuffer → GetInputBuffer → 拷贝 → SetBufferAttr(pts μs) → PushInputBuffer）
//  - 输出：RegisterCallback onNeedOutputBuffer 回调；Annex-B → AVCC 转换（4 字节长度前缀），
//    SPS/PPS 提取生成 avcC（同时供 MP4 OH_MD_KEY_CODEC_CONFIG 与 RTMP sequence header）
//  - 关键帧间隔 2s（GOP），CBR 码率控制

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

struct OH_AVCodec;
struct OH_AVBuffer;
struct OH_AVFormat;

namespace media_stream {

class VideoEncoder {
public:
    // 编码输出回调：data 为 AVCC 格式 H.264（无 start code），ptsUs 微秒
    using OutputCallback =
        std::function<void(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe)>;
    // 编码参数集回调：avcC（AVCDecoderConfigurationRecord 原始字节）
    using CodecConfigCallback = std::function<void(const std::vector<uint8_t> &avcC)>;
    using ErrorCallback = std::function<void(int32_t errorCode)>;

    struct Callbacks {
        OutputCallback onOutput;
        CodecConfigCallback onCodecConfig;
        ErrorCallback onError;
    };

    VideoEncoder() = default;
    ~VideoEncoder();

    // 配置并启动；失败返回 false
    bool Start(int width, int height, int fps, int bitrateKbps, Callbacks callbacks);
    // 投递一帧 NV12（紧凑打包 w*h*1.5）；编码器未运行或暂无输入缓冲时丢弃
    void InputFrame(const uint8_t *nv12, int64_t ptsUs);
    // 停止并释放（幂等）
    void Stop();

    bool IsRunning() const { return running_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    // OH_AVCodecCallback 静态桥接
    static void OnCodecError(OH_AVCodec *codec, int32_t errorCode, void *userData);
    static void OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData);
    // 输入缓冲回调：硬件编码器仅支持回调驱动投递（QueryInputBuffer 拉取在真机返回 rc=2 不可用），
    // 在此回调中取出一帧排队的 NV12 帧推入编码器
    // 签名与 OH_AVCodecOnNeedInputBuffer 对齐：(codec, index, OH_AVBuffer*, userData)
    static void OnNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData);
    static void OnNeedOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData);

    // Annex-B → AVCC：扫描 start code 切分 NALU，输出 4 字节大端长度前缀
    // 返回 false 表示输入已是 AVCC（直接透传）
    bool ConvertAnnexBToAvcc(const uint8_t *data, int32_t size, std::vector<uint8_t> &out);
    // 按时间周期性请求 IDR 关键帧（锁外调用）
    void MaybeRequestKeyFrame(OH_AVCodec *enc);
    // 从 Annex-B NALU 流提取 SPS/PPS 并构建 avcC
    void ExtractAvcc(const uint8_t *data, int32_t size);
    // 去重发布 avcC（「仅发布一次」）。cfgMutex_ 只覆盖发布状态，回调 onCodecConfig
    // （→引擎 OnAvccReady）在锁外执行，避免把引擎锁纳入本侧锁的嵌套范围。
    void PublishAvcc(const std::vector<uint8_t> &record);

    OH_AVCodec *encoder_ = nullptr;
    Callbacks callbacks_;
    // 关键帧（IDR）按时间主动请求，不依赖 I_FRAME_INTERVAL 的按帧数语义：
    // 该参数文档为「每 (frameRate * value)/1000 帧一个关键帧」，而本场景投递受屏幕内容变化
    // 驱动、实际帧率可低至 1~4fps，按帧数折算出的 GOP 时间跨度可达数十秒 —— 新接入的播放器
    // 等不到 IDR 就无法解码（真机实测 8s 推流仅含 1 个 I 帧，ffplay 表现为 vq=0KB 卡住黑屏）。
    static constexpr int64_t kKeyFrameIntervalMs = 2000;
    std::atomic<int64_t> lastKeyReqMs_{0};
    std::atomic<bool> aliveFlag_{false}; // 锁外调用 OH 接口前的存活校验
    std::mutex mutex_; // 保护 encoder_ 生命周期（Stop 与回调并发）
    bool running_ = false;
    int width_ = 0;
    int height_ = 0;
    bool avccEmitted_ = false;
    std::vector<uint8_t> avcc_;
    // 保护 avcC 的发布状态。解析入口有两个、分别跑在编码器不同回调线程
    // （onStreamChanged 与 onNeedOutputBuffer），并发写 avcc_ 属 UB。
    // 锁序：mutex_(编码器锁) → cfgMutex_，不存在反向获取。
    std::mutex cfgMutex_;
    std::vector<uint8_t> convertScratch_; // Annex-B→AVCC 转换暂存
    // 将 pendingFrames_ 队首帧写入指定输入槽并 SetBufferAttr（须持 mutex_）。
    // 成功返回 true 并弹出该帧；槽容量不足返回 false（该帧保留，槽转为暂留）。
    bool FillSlotLocked(uint32_t index, OH_AVBuffer *buffer);

    // 待编码帧队列：采集线程入队，onNeedInputBuffer 回调线程出队（drop-if-busy，上限 4 帧）
    static constexpr size_t kMaxPendingFrames = 4;
    std::deque<std::vector<uint8_t>> pendingFrames_;
    std::deque<int64_t> pendingPtsUs_;

    // 输入槽暂留（关键修复：绿屏根因）
    // 队列为空时**绝不** Push 0 长输入缓冲：OH 硬件编码器会把未写入（全 0）的输入
    // 缓冲当成真实帧编码，以数百 fps 自转产出 45B 空白帧。这些垃圾帧会污染参考帧链
    // 并占据时间轴，播放时整段解码为 YUV(0,0,0) → RGB(0,135,0) 纯绿（真机实测
    // 2256/6403 帧为空白帧，文件前 10s 全绿）。改为暂留输入槽，等 InputFrame 拿到
    // 真实帧后填回再 Push（与 AudioEncoder 同构）。
    int32_t idleIndex_ = -1;
    OH_AVBuffer *idleBuffer_ = nullptr;
    int pushedCnt_ = 0;   // 调试：已成功投递帧计数
    int slotLogged_ = 0;  // 调试：输入槽 native buffer 配置只打前 2 次
};

} // namespace media_stream

#endif // MEDIA_STREAM_VIDEO_ENCODER_H
