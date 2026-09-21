#include "audio_mixer.h"

#include <cmath>

#include "../common/logger.h"

namespace media_stream {

// 20ms @ 48kHz 双声道 = 960*2 采样
static constexpr size_t kChunkSamples = 960 * 2;
// s16 满幅边界
static constexpr int32_t kFullScale = 32767;
static constexpr int32_t kFullScaleNeg = -32768;

AudioMixer::AudioMixer(Mode mode) : mode_(mode) {}

void AudioMixer::SetOutputCallback(OutputCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ = std::move(cb);
}

void AudioMixer::PushInner(const uint8_t *pcm, int32_t bytes, int64_t ptsNs) {
    if (pcm == nullptr || bytes <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == Mode::kMicOnly) {
        return; // 仅麦克风档：丢弃内录
    }
    if (mode_ == Mode::kInnerOnly) {
        // 单输入直通
        if (output_) {
            output_(reinterpret_cast<const int16_t *>(pcm), bytes, ptsNs);
        }
        return;
    }
    // 双输入：入队待混
    size_t samples = static_cast<size_t>(bytes) / 2;
    const int16_t *in = reinterpret_cast<const int16_t *>(pcm);
    if (innerQueue_.empty()) {
        innerPtsNs_ = ptsNs;
    }
    innerQueue_.insert(innerQueue_.end(), in, in + samples);
    // 防积压：内录队列上限 200ms
    DropOldestLocked(innerQueue_, droppedInner_, "inner");
    TryMixLocked();
}

void AudioMixer::PushMic(const uint8_t *pcm, int32_t bytes, int64_t ptsNs) {
    if (pcm == nullptr || bytes <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == Mode::kInnerOnly) {
        return; // 仅内录档：丢弃麦克风
    }
    micEverReceived_ = true;
    if (mode_ == Mode::kMicOnly) {
        if (output_) {
            output_(reinterpret_cast<const int16_t *>(pcm), bytes, ptsNs);
        }
        return;
    }
    size_t samples = static_cast<size_t>(bytes) / 2;
    const int16_t *in = reinterpret_cast<const int16_t *>(pcm);
    if (micQueue_.empty()) {
        micPtsNs_ = ptsNs;
    }
    micQueue_.insert(micQueue_.end(), in, in + samples);
    DropOldestLocked(micQueue_, droppedMic_, "mic");
    TryMixLocked();
}

// 超上限则丢弃最旧样本。丢样本会在波形上留下跳变（听感为咔哒/电音），所以只在真的积压时
// 发生，并按「每累计丢满 200ms」打一条告警：这样「电音」类问题能直接从日志区分是混音器在丢，
// 还是编码器/时间戳的问题。
//
// 注意必须「成对」丢弃：每 2 个 int16 才是一个立体声采样帧，只丢奇数个会让此后所有样本的
// L/R 互换（整段立体声错位），听感同样是杂音/电音，且很难从时长上发现。
void AudioMixer::DropOldestLocked(std::deque<int16_t> &q, int64_t &dropped, const char *tag) {
    const size_t cap = kChunkSamples * 10; // 200ms = 19200 采样
    const size_t before = q.size();
    if (before <= cap) {
        return;
    }
    while (q.size() > cap) {
        q.pop_front();
        dropped++;
    }
    int64_t thisDrop = static_cast<int64_t>(before - q.size());
    // 补一个，保证本次丢弃是偶数个（不破坏 L/R 配对）
    if ((thisDrop & 1) && !q.empty()) {
        q.pop_front();
        dropped++;
        thisDrop++;
    }
    if ((dropped % static_cast<int64_t>(cap)) < thisDrop) {
        MS_LOG_WARN("[MIX] %{public}s queue backlog, dropped %{public}lld samples (200ms each %{public}d)",
                    tag, static_cast<long long>(dropped), static_cast<int>(cap));
    }
}

void AudioMixer::TryMixLocked() {
    if (mode_ != Mode::kMicAndInner || !output_) {
        return;
    }
    // 两路均有数据时按块混音；某一路缺失时短暂等待（200ms 上限由入队侧防积压兜底）
    while (innerQueue_.size() >= kChunkSamples && micQueue_.size() >= kChunkSamples) {
        mixScratch_.resize(kChunkSamples);
        // 诊断统计：两路能量与互相关。互相关高 = 两路是「同一个声音」的两个副本
        // （麦克风拾取扬声器外放，或系统把麦克风混进了内录通道）—— 这时相加会产生
        // 梳状滤波，听感正是「说话时才出现的电音」。spill = 相加溢出的样本数。
        int64_t eInner = 0, eMic = 0, cross = 0;
        int spill = 0;
        int32_t peak = 0;
        for (size_t i = 0; i < kChunkSamples; i++) {
            int32_t a = static_cast<int32_t>(innerQueue_[i]);
            int32_t b = static_cast<int32_t>(micQueue_[i]);
            eInner += static_cast<int64_t>(a) * a;
            eMic += static_cast<int64_t>(b) * b;
            cross += static_cast<int64_t>(a) * b;
            int32_t sum = a + b;
            if (sum > kFullScale || sum < kFullScaleNeg) {
                // 软限幅：溢出部分压缩到 1/8 再取界，避免硬钳位产生的平顶（高次谐波 = 电音）。
                // 两路都接近满幅时才会走到这里；普通音量下与直接相加完全一致。
                if (sum > kFullScale) {
                    sum = kFullScale + (sum - kFullScale) / 8;
                } else {
                    sum = kFullScaleNeg + (sum - kFullScaleNeg) / 8;
                }
                if (sum > kFullScale) {
                    sum = kFullScale;
                } else if (sum < kFullScaleNeg) {
                    sum = kFullScaleNeg;
                }
                spill++;
            }
            mixScratch_[i] = static_cast<int16_t>(sum);
            int32_t mag = sum < 0 ? -sum : sum;
            if (mag > peak) {
                peak = mag;
            }
        }
        DiagMixLocked(eInner, eMic, cross, spill, peak);
        for (size_t i = 0; i < kChunkSamples; i++) {
            innerQueue_.pop_front();
            micQueue_.pop_front();
        }
        int64_t pts = innerPtsNs_ < micPtsNs_ ? innerPtsNs_ : micPtsNs_;
        // 推进两路队列首采样时间戳（960 采样 = 20ms）
        innerPtsNs_ += 20000000;
        micPtsNs_ += 20000000;
        output_(mixScratch_.data(), static_cast<int32_t>(kChunkSamples * 2), pts);
    }
}

// 每 64 块（约 1.28s）打一条混音诊断。全部用整数格式化，避免 hilog 浮点格式差异。
//  - innerRms/micRms：两路各自的均方根（16bit 满幅 32767）。某一路常年为 0 说明该路没数据。
//  - corrMilli：两路互相关（×1000）。≈1000 表示两路是同一声音的副本（麦克风听到了外放 /
//    内录里混了麦克风），相加即梳状滤波 → 说话时出现「电音」；≈0 表示两路互相独立。
//  - spill：相加溢出被限幅的样本数。持续非 0 说明两路音量叠加确实在削波。
//  - qInner/qMic：两路队列深度（采样数）。两者差值即两路的时间错位（约 48000 采样 = 1s），
//    持续拉大说明两路投递速率不一致，混音等于把不同时刻的声音叠在一起。
void AudioMixer::DiagMixLocked(int64_t eInner, int64_t eMic, int64_t cross, int spill, int32_t peak) {
    diagBlocks_++;
    if ((diagBlocks_ & 0x3F) != 1) {
        return;
    }
    const double n = static_cast<double>(kChunkSamples);
    int64_t ri = static_cast<int64_t>(std::sqrt(static_cast<double>(eInner) / n));
    int64_t rm = static_cast<int64_t>(std::sqrt(static_cast<double>(eMic) / n));
    int corrMilli = 0;
    if (eInner > 0 && eMic > 0) {
        double denom = std::sqrt(static_cast<double>(eInner) * static_cast<double>(eMic));
        corrMilli = static_cast<int>(static_cast<double>(cross) / denom * 1000.0);
    }
    MS_LOG_WARN("[MIX-DIAG] innerRms=%{public}lld micRms=%{public}lld corrMilli=%{public}d "
                "spill=%{public}d peak=%{public}d qInner=%{public}zu qMic=%{public}zu",
                static_cast<long long>(ri), static_cast<long long>(rm), corrMilli, spill, peak,
                innerQueue_.size(), micQueue_.size());
}

void AudioMixer::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    innerQueue_.clear();
    micQueue_.clear();
    micEverReceived_ = false;
    droppedInner_ = 0;
    droppedMic_ = 0;
    diagBlocks_ = 0;
}

} // namespace media_stream
