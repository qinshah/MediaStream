#ifndef MEDIA_STREAM_AUDIO_MIXER_H
#define MEDIA_STREAM_AUDIO_MIXER_H

// 双路 PCM 饱和混音器（内录 + 麦克风 → 单路 s16le 48kHz 双声道）
//  - 双输入模式：按采样数对齐做 s16 饱和相加；两路按到达顺序各自入队，混音输出取两路较小可用长度
//  - 单输入模式：直通（零拷贝转发）
// 输出以固定块（20ms = 1920 采样 = 3840 字节）投递，便于编码器稳定取流

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace media_stream {

class AudioMixer {
public:
    enum class Mode {
        kMicOnly,
        kInnerOnly,
        kMicAndInner,
    };

    // 混音输出回调：s16le 48k 双声道交错 PCM；ptsNs 为该块首采样时间
    using OutputCallback = std::function<void(const int16_t *pcm, int32_t bytes, int64_t ptsNs)>;

    explicit AudioMixer(Mode mode = Mode::kInnerOnly);

    void SetOutputCallback(OutputCallback cb);

    // 输入：内录 / 麦克风 PCM（s16le 48k 双声道）
    void PushInner(const uint8_t *pcm, int32_t bytes, int64_t ptsNs);
    void PushMic(const uint8_t *pcm, int32_t bytes, int64_t ptsNs);

    // 清空队列（会话停止时）
    void Reset();

    // 统计：麦克风是否收到过数据（供降级检测）
    bool MicEverReceived() const { return micEverReceived_; }

private:
    void TryMixLocked();

    Mode mode_;
    OutputCallback output_;
    std::mutex mutex_;
    std::deque<int16_t> innerQueue_;
    std::deque<int16_t> micQueue_;
    bool micEverReceived_ = false;
    int64_t innerPtsNs_ = 0;
    int64_t micPtsNs_ = 0;
    std::vector<int16_t> mixScratch_;

    // 队列溢出丢弃计数（每路）。两路节奏一致时永远为 0；一旦持续增长，说明某一路投递速率
    // 高于混音消费速率（采样率/包长与 48kHz 假设不符），此时会持续抽掉样本产生咔哒声。
    // 打日志而不是静默丢弃，是为了让「电音」这类听感问题在日志里有据可查。
    int64_t droppedInner_ = 0;
    int64_t droppedMic_ = 0;
    void DropOldestLocked(std::deque<int16_t> &q, int64_t &dropped, const char *tag);

    // 混音诊断（每 64 块打一条 [MIX-DIAG]）：用来区分「电音」是两路同源相加的梳状滤波，
    // 还是真的削波/丢样本。diagBlocks_ 为累计块数。
    int64_t diagBlocks_ = 0;
    void DiagMixLocked(int64_t eInner, int64_t eMic, int64_t cross, int spill, int32_t peak);
};

} // namespace media_stream

#endif // MEDIA_STREAM_AUDIO_MIXER_H
