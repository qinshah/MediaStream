#include "audio_mixer.h"

#include "../common/logger.h"

namespace media_stream {

// 20ms @ 48kHz 双声道 = 960*2 采样
static constexpr size_t kChunkSamples = 960 * 2;

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
    while (innerQueue_.size() > kChunkSamples * 10) {
        innerQueue_.pop_front();
    }
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
    while (micQueue_.size() > kChunkSamples * 10) {
        micQueue_.pop_front();
    }
    TryMixLocked();
}

void AudioMixer::TryMixLocked() {
    if (mode_ != Mode::kMicAndInner || !output_) {
        return;
    }
    // 两路均有数据时按块混音；某一路缺失时短暂等待（200ms 上限由入队侧防积压兜底）
    while (innerQueue_.size() >= kChunkSamples && micQueue_.size() >= kChunkSamples) {
        mixScratch_.resize(kChunkSamples);
        for (size_t i = 0; i < kChunkSamples; i++) {
            int32_t sum = static_cast<int32_t>(innerQueue_[i]) + static_cast<int32_t>(micQueue_[i]);
            // s16 饱和
            if (sum > 32767) {
                sum = 32767;
            } else if (sum < -32768) {
                sum = -32768;
            }
            mixScratch_[i] = static_cast<int16_t>(sum);
        }
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

void AudioMixer::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    innerQueue_.clear();
    micQueue_.clear();
    micEverReceived_ = false;
}

} // namespace media_stream
