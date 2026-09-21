#ifndef MEDIA_STREAM_VIDEO_ENCODER_H
#define MEDIA_STREAM_VIDEO_ENCODER_H

// OH_VideoEncoder H.264 硬件编码封装（buffer 模式）
//  - 输入：NV12 帧（QueryInputBuffer → GetInputBuffer → 拷贝 → SetBufferAttr(pts μs) → PushInputBuffer）
//  - 输出：RegisterCallback onNeedOutputBuffer 回调；Annex-B → AVCC 转换（4 字节长度前缀），
//    SPS/PPS 提取生成 avcC（同时供 MP4 OH_MD_KEY_CODEC_CONFIG 与 RTMP sequence header）
//  - 关键帧间隔 2s（GOP），CBR 码率控制

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
    // 从 Annex-B NALU 流提取 SPS/PPS 并构建 avcC
    void ExtractAvcc(const uint8_t *data, int32_t size);

    OH_AVCodec *encoder_ = nullptr;
    Callbacks callbacks_;
    std::mutex mutex_; // 保护 encoder_ 生命周期（Stop 与回调并发）
    bool running_ = false;
    int width_ = 0;
    int height_ = 0;
    bool avccEmitted_ = false;
    std::vector<uint8_t> avcc_;
    std::vector<uint8_t> convertScratch_; // Annex-B→AVCC 转换暂存
    // 待编码帧队列：采集线程入队，onNeedInputBuffer 回调线程出队（drop-if-busy，上限 4 帧）
    static constexpr size_t kMaxPendingFrames = 4;
    std::deque<std::vector<uint8_t>> pendingFrames_;
    std::deque<int64_t> pendingPtsUs_;
    int pushedCnt_ = 0; // 调试：已成功投递帧计数
};

} // namespace media_stream

#endif // MEDIA_STREAM_VIDEO_ENCODER_H
