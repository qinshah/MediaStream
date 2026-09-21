#ifndef MEDIA_STREAM_AUDIO_ENCODER_H
#define MEDIA_STREAM_AUDIO_ENCODER_H

// OH_AACEncoder AAC 硬件编码封装（AAC-LC 48kHz 双声道，128kbps）
// 音频编码器仅支持 async memory 回调模式：
//  - onNeedInputData：从内部 PCM 队列取数据填入 OH_AVMemory，PushInputData 提交
//  - onNewOutputData：AAC 原始帧（已禁用 ADTS），首帧提取 ASC（AudioSpecificConfig）
// ASC 同时供 MP4 音轨 OH_MD_KEY_CODEC_CONFIG 与 RTMP audio sequence header

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

struct OH_AVCodec;
struct OH_AVMemory;
struct OH_AVFormat;
struct OH_AVCodecBufferAttr;

namespace media_stream {

class AudioEncoder {
public:
    // AAC 帧输出回调：data 为原始 AAC（无 ADTS 头），ptsUs 微秒
    using OutputCallback = std::function<void(const uint8_t *data, int32_t size, int64_t ptsUs)>;
    // ASC 回调（AudioSpecificConfig 原始字节）
    using AscCallback = std::function<void(const std::vector<uint8_t> &asc)>;
    using ErrorCallback = std::function<void(int32_t errorCode)>;

    struct Callbacks {
        OutputCallback onOutput;
        AscCallback onAsc;
        ErrorCallback onError;
    };

    AudioEncoder() = default;
    ~AudioEncoder();

    bool Start(Callbacks callbacks);
    // 投递 PCM（s16le 48k 双声道交错）；队列满（>500ms）时丢弃最旧数据
    void InputPcm(const int16_t *pcm, int32_t bytes, int64_t ptsNs);
    void Stop();

    bool IsRunning() const { return running_; }

private:
    static void OnCodecError(OH_AVCodec *codec, int32_t errorCode, void *userData);
    static void OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData);
    static void OnNeedInputData(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, void *userData);
    static void OnNewOutputData(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data,
                                OH_AVCodecBufferAttr *attr, void *userData);

    OH_AVCodec *encoder_ = nullptr;
    Callbacks callbacks_;
    std::mutex mutex_;
    bool running_ = false;

    // PCM 输入队列（采样点为单位，s16）
    std::deque<int16_t> pcmQueue_;
    // 音频时间轴（ns）：锚定一次之后就**只按采样点数推进**，不随回调到达时刻漂移。
    // 锚点由调用方给定，且必须是「会话相对时间」（与视频 PTS 同原点），否则两侧时间轴错源，
    // 播放器为对齐而丢/补帧 → 电音；MP4 侧则表现为开头缺一段画面。
    int64_t pcmPtsNs_ = 0;
    bool ptsAnchored_ = false; // 是否已锚定（未锚定时首次真实 PCM 直接落锚）

    // ── 输出帧时间轴（μs）──
    // 关键：OH_AudioEncoder 输出缓冲的 pts **不是**我们喂进去的输入 pts。真机实测（[AENC-IN]/
    // [AENC-OUT]）：喂入会话相对时间 9060228μs，输出却是 21333μs（= 帧序号 × 1024/48000s）。
    // 也就是说编码器自持一套「从 0 起、按输出帧数推进」的计数器，与采集/会话时钟毫无关系。
    // 直接采信它会让音频与视频（会话时钟）时间轴错源：MP4 里音频早于视频首帧的那一大段被
    // 封装器钳到 0（`[MP4-NEG]`），成片音频全堆在起点 —— 听感即电音、音轨时长还会截断；
    // RTMP 侧同样表现为音视频错源。
    // 因此输出帧的 pts 由本类自维护：首帧用首包真实 PCM 的喂入时间锚定，之后每输出一帧推进
    // 一个 AAC 帧时长；若喂入侧时间已跑到前面（采集真断过/授权等待）则只**前跳**对齐，绝不回退。
    int64_t outPtsUs_ = 0;
    bool outAnchored_ = false;
    int64_t firstFedPtsUs_ = 0;  // 首个真实 PCM 的喂入 pts（输出帧时间轴的锚点）
    bool firstFedSet_ = false;
    int64_t latestFedPtsUs_ = 0; // 最近一次喂入的 pts（用于前跳对齐）
    static constexpr int64_t kAacFrameUs = 1024000000LL / 48000; // 1024 采样 = 21333μs
    static constexpr int64_t kOutResyncUs = 100000;              // 前跳阈值 100ms

    void MarkFedLocked(int64_t ptsUs); // 记录喂入时间轴（须持 mutex_）

    // OBS 式解耦：onNeedInputData 在无数据时不紧张循环（避免编码器 Stop 阻塞）。
    // 无数据时暂留一个输入槽（idleIndex_/idleMem_，不 Push），等 InputPcm 有真实音频后再填回，
    // 从而把音频喂入与编码器“要输入”回调解耦，杜绝静音垫底导致的 CPU/Stop 长时间阻塞。
    int32_t idleIndex_ = -1;
    OH_AVMemory *idleMem_ = nullptr;

    // ── 起播静音垫 ──
    // 编码器启动后的第一个输入槽先投一小段静音，把编码链路真正跑起来（首帧延迟、
    // 内部缓冲就位）。这段静音的编码产物在「尚未喂入真实 PCM」期间全部丢弃，
    // 不占用音频时间轴。
    bool primed_ = false;       // 是否已投递过静音垫
    bool realPcmSeen_ = false;  // 是否已喂入外部真实 PCM（true 后静音垫产物不再丢弃）
    static constexpr size_t kPrimingSamples = 960 * 2; // 20ms @48k 双声道

    bool ascEmitted_ = false;
    std::vector<uint8_t> asc_;
};

} // namespace media_stream

#endif // MEDIA_STREAM_AUDIO_ENCODER_H
