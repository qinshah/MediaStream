#include "audio_encoder.h"

#include <cstring>

#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avcodec_audioencoder.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmemory.h>

#include "../common/logger.h"

namespace media_stream {

static constexpr int kSampleRate = 48000;
static constexpr int kChannels = 2;
static constexpr int kBitrate = 128000;
// 队列上限：500ms = 24000 采样/声道
static constexpr size_t kMaxQueueSamples = kSampleRate / 2 * kChannels;

AudioEncoder::~AudioEncoder() {
    Stop();
}

bool AudioEncoder::Start(Callbacks callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return true;
    }
    callbacks_ = std::move(callbacks);
    pcmQueue_.clear();
    ascEmitted_ = false;
    asc_.clear();

    encoder_ = OH_AudioEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_AUDIO_AAC);
    if (encoder_ == nullptr) {
        MS_LOG_ERROR("OH_AudioEncoder_CreateByMime failed");
        return false;
    }

    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, kSampleRate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, kChannels);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, kBitrate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PROFILE, AAC_PROFILE_LC);
    // 输出原始 AAC 帧（不带 ADTS），MP4/FLV 均使用裸 AAC
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AAC_IS_ADTS, 0);

    int32_t rc = OH_AudioEncoder_Configure(encoder_, format);
    OH_AVFormat_Destroy(format);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_AudioEncoder_Configure failed rc=%{public}d", rc);
        OH_AudioEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }

    OH_AVCodecAsyncCallback cb = {OnCodecError, OnStreamChanged, OnNeedInputData, OnNewOutputData};
    rc = OH_AudioEncoder_SetCallback(encoder_, cb, this);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_AudioEncoder_SetCallback failed rc=%{public}d", rc);
        OH_AudioEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }
    rc = OH_AudioEncoder_Prepare(encoder_);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_AudioEncoder_Prepare failed rc=%{public}d", rc);
        OH_AudioEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }
    rc = OH_AudioEncoder_Start(encoder_);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_AudioEncoder_Start failed rc=%{public}d", rc);
        OH_AudioEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }
    running_ = true;
    MS_LOG_INFO("AudioEncoder started 48kHz stereo 128kbps AAC-LC");
    return true;
}

void AudioEncoder::InputPcm(const int16_t *pcm, int32_t bytes, int64_t ptsNs) {
    if (pcm == nullptr || bytes <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    if (pcmQueue_.empty()) {
        pcmPtsNs_ = ptsNs;
    }
    size_t samples = static_cast<size_t>(bytes) / 2;
    pcmQueue_.insert(pcmQueue_.end(), pcm, pcm + samples);
    // 防积压：丢弃最旧数据（音频不阻塞采集）
    while (pcmQueue_.size() > kMaxQueueSamples) {
        pcmQueue_.pop_front();
        pcmPtsNs_ += 1000000000LL / (kSampleRate * kChannels); // 每采样点时间
    }
}

void AudioEncoder::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ != nullptr) {
        if (running_) {
            OH_AudioEncoder_Stop(encoder_);
            running_ = false;
        }
        OH_AudioEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        MS_LOG_INFO("AudioEncoder stopped");
    }
}

// —— 回调桥接 ——

void AudioEncoder::OnCodecError(OH_AVCodec *codec, int32_t errorCode, void *userData) {
    (void)codec;
    auto *self = static_cast<AudioEncoder *>(userData);
    MS_LOG_ERROR("AudioEncoder error %{public}d", errorCode);
    if (self != nullptr && self->callbacks_.onError) {
        self->callbacks_.onError(errorCode);
    }
}

void AudioEncoder::OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData) {
    (void)codec;
    auto *self = static_cast<AudioEncoder *>(userData);
    if (self == nullptr || format == nullptr) {
        return;
    }
    // 输出描述中直接携带 ASC（OH_MD_KEY_CODEC_CONFIG）
    uint8_t *config = nullptr;
    size_t configSize = 0;
    if (OH_AVFormat_GetBuffer(format, OH_MD_KEY_CODEC_CONFIG, &config, &configSize) && config != nullptr &&
        configSize > 0 && !self->ascEmitted_) {
        self->asc_.assign(config, config + configSize);
        self->ascEmitted_ = true;
        if (self->callbacks_.onAsc) {
            self->callbacks_.onAsc(self->asc_);
        }
        MS_LOG_INFO("ASC from output description, %{public}zu bytes", configSize);
    }
}

void AudioEncoder::OnNeedInputData(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, void *userData) {
    auto *self = static_cast<AudioEncoder *>(userData);
    if (self == nullptr || data == nullptr) {
        return;
    }
    uint8_t *addr = OH_AVMemory_GetAddr(data);
    int32_t capacity = OH_AVMemory_GetSize(data);
    if (addr == nullptr || capacity <= 0) {
        return;
    }

    OH_AVCodecBufferAttr attr = {};
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->running_ && !self->pcmQueue_.empty()) {
            // 填充尽可能多的 PCM（偶数采样对齐，保证声道对完整）
            size_t availSamples = self->pcmQueue_.size();
            size_t maxSamples = static_cast<size_t>(capacity) / 2;
            size_t samples = availSamples < maxSamples ? availSamples : maxSamples;
            samples &= ~static_cast<size_t>(1); // 双声道对齐
            if (samples > 0) {
                for (size_t i = 0; i < samples; i++) {
                    reinterpret_cast<int16_t *>(addr)[i] = self->pcmQueue_[i];
                }
                for (size_t i = 0; i < samples; i++) {
                    self->pcmQueue_.pop_front();
                }
                attr.pts = self->pcmPtsNs_ / 1000; // ns → μs
                // 推进队列首时间戳
                self->pcmPtsNs_ += static_cast<int64_t>(samples) * 1000000000LL / (kSampleRate * kChannels);
                attr.size = static_cast<int32_t>(samples * 2);
                attr.offset = 0;
                attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
                OH_AudioEncoder_PushInputData(codec, index, attr);
                return;
            }
        }
    }
    // 无数据：送入 0 长度跳过该缓冲（避免编码器等待）
    attr.pts = 0;
    attr.size = 0;
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_AudioEncoder_PushInputData(codec, index, attr);
}

void AudioEncoder::OnNewOutputData(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data,
                                   OH_AVCodecBufferAttr *bufferAttr, void *userData) {
    auto *self = static_cast<AudioEncoder *>(userData);
    if (self == nullptr || data == nullptr || bufferAttr == nullptr) {
        return;
    }
    uint8_t *addr = OH_AVMemory_GetAddr(data);
    if (addr == nullptr) {
        OH_AudioEncoder_FreeOutputData(codec, index);
        return;
    }
    const uint8_t *frame = addr + bufferAttr->offset;
    int32_t size = bufferAttr->size;

    if ((bufferAttr->flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) != 0) {
        // codec config 缓冲：内容即 ASC
        if (!self->ascEmitted_ && size > 0) {
            self->asc_.assign(frame, frame + size);
            self->ascEmitted_ = true;
            if (self->callbacks_.onAsc) {
                self->callbacks_.onAsc(self->asc_);
            }
            MS_LOG_INFO("ASC from codec data buffer, %{public}d bytes", size);
        }
        OH_AudioEncoder_FreeOutputData(codec, index);
        return;
    }

    if (size > 0 && self->callbacks_.onOutput) {
        self->callbacks_.onOutput(frame, size, bufferAttr->pts);
    }
    OH_AudioEncoder_FreeOutputData(codec, index);
}

} // namespace media_stream
