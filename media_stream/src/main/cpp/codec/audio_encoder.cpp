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

// 时间轴重锚阈值（ns）：队列取空时，若「到达时刻」与「采样计数时间轴」相差超过该值才重新锚定
// （用于采集真断过、或长时运行后累积漂移的兜底）；小偏差一律保持采样计数，避免把回调抖动
// 写进时间轴。
static constexpr int64_t kPtsResyncThresholdNs = 100000000LL; // 100ms

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
    idleIndex_ = -1;
    idleMem_ = nullptr;
    pcmPtsNs_ = 0;
    ptsAnchored_ = false; // 本会话尚未锚定：首包真实 PCM 落到调用方给定的会话相对时间上
    outPtsUs_ = 0;
    outAnchored_ = false;
    firstFedPtsUs_ = 0;
    firstFedSet_ = false;
    latestFedPtsUs_ = 0;

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
    // 起播静音垫：等 OnNeedInputData 首次回调时投递一小段静音，把编码链路真正跑起来
    primed_ = false;
    realPcmSeen_ = false;
    MS_LOG_INFO("AudioEncoder started 48kHz stereo 128kbps AAC-LC");
    return true;
}

void AudioEncoder::InputPcm(const int16_t *pcm, int32_t bytes, int64_t ptsNs) {
    if (pcm == nullptr || bytes <= 0) {
        return;
    }
    // 若此前 onNeedInputData 已将某个输入槽 hold 住，这里优先把新增 PCM 填入该槽并 Push，
    // 使编码器及时拿到真实音频（OBS 式解耦喂音）。
    int32_t pushIdx = -1;
    int32_t pushPtsUs = 0;
    int32_t pushSize = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        realPcmSeen_ = true; // 真实 PCM 到达：此后静音垫产物正常透传
        // 时间轴：只锚定一次（或偏差过大时重锚），之后按采样点数推进，不吸收回调抖动。
        // 旧行为是「每次队列取空就 pcmPtsNs_ = 到达时刻」，而 InputPcm 每次都把队列灌进暂留槽、
        // 队列基本总是取空 —— 等于音频 PTS 直接取原始到达时间，20ms 包的到达抖动（数毫秒）
        // 全部写进时间轴，播放器为对齐而反复丢/补帧，听感就是电音。
        if (pcmQueue_.empty()) {
            const int64_t driftNs = ptsNs - pcmPtsNs_;
            if (!ptsAnchored_ || driftNs > kPtsResyncThresholdNs || driftNs < -kPtsResyncThresholdNs) {
                pcmPtsNs_ = ptsNs;
                ptsAnchored_ = true;
            }
        }
        size_t samples = static_cast<size_t>(bytes) / 2;
        pcmQueue_.insert(pcmQueue_.end(), pcm, pcm + samples);
        // 防积压：丢弃最旧数据（音频不阻塞采集）
        while (pcmQueue_.size() > kMaxQueueSamples) {
            pcmQueue_.pop_front();
            pcmPtsNs_ += 1000000000LL / (kSampleRate * kChannels); // 每采样点时间
        }
        // 用暂留槽直接推送一包，降低等待编码器再次回调的延迟
        if (idleIndex_ >= 0 && idleMem_ != nullptr) {
            uint8_t *addr = OH_AVMemory_GetAddr(idleMem_);
            int32_t capacity = OH_AVMemory_GetSize(idleMem_);
            size_t maxSamples = static_cast<size_t>(capacity) / 2;
            size_t n = pcmQueue_.size() < maxSamples ? pcmQueue_.size() : maxSamples;
            n &= ~static_cast<size_t>(1);
            if (addr != nullptr && capacity > 0 && n > 0) {
                for (size_t i = 0; i < n; i++) {
                    reinterpret_cast<int16_t *>(addr)[i] = pcmQueue_[i];
                }
                for (size_t i = 0; i < n; i++) {
                    pcmQueue_.pop_front();
                }
                pushPtsUs = static_cast<int32_t>(pcmPtsNs_ / 1000);
                MarkFedLocked(pushPtsUs); // 记录喂入时间轴：输出帧时间轴以它为准
                pcmPtsNs_ += static_cast<int64_t>(n) * 1000000000LL / (kSampleRate * kChannels);
                pushSize = static_cast<int32_t>(n * 2);
                pushIdx = idleIndex_;
                idleIndex_ = -1;
                idleMem_ = nullptr;
            }
        }
    }
    if (pushIdx >= 0) {
        OH_AVCodecBufferAttr attr{};
        attr.pts = pushPtsUs;
        attr.size = pushSize;
        attr.offset = 0;
        attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
        // 诊断：确认喂进编码器的会话相对时间轴（前 3 包 + 每 1000 包）
        static std::atomic<int> inCtr{0};
        int ic = inCtr.fetch_add(1);
        if (ic < 3 || ic % 1000 == 0) {
            MS_LOG_WARN("[AENC-IN] #%{public}d fedPtsUs=%{public}d samples=%{public}d", ic, pushPtsUs,
                        pushSize / 2);
        }
        OH_AudioEncoder_PushInputData(encoder_, static_cast<uint32_t>(pushIdx), attr);
    }
}

// 记录喂入时间轴：首个真实 PCM 的 pts 作为输出帧时间轴锚点，最近值用于前跳对齐。
void AudioEncoder::MarkFedLocked(int64_t ptsUs) {
    if (!firstFedSet_) {
        firstFedPtsUs_ = ptsUs;
        firstFedSet_ = true;
    }
    latestFedPtsUs_ = ptsUs;
}

void AudioEncoder::Stop() {
    // 与视频编码器一致：勿持 mutex_ 调用 OH_AudioEncoder_Stop/Destroy，避免与
    // 编码器工作线程（OnNeedInputData 锁本 mutex_）形成锁序反转死锁。
    OH_AVCodec *enc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enc = encoder_;
        encoder_ = nullptr;
        running_ = false;
        idleIndex_ = -1;
        idleMem_ = nullptr; // 已 hold 的输入槽随编解器销毁失效，置空避免悬垂
    }
    if (enc != nullptr) {
        OH_AudioEncoder_Stop(enc);
        OH_AudioEncoder_Destroy(enc);
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
    int32_t pushIdx = -1;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        // 已有暂留的输入槽（此前无数据时 hold 住），不重复占槽；待 InputPcm 有数据后填回。
        if (self->idleIndex_ >= 0) {
            return;
        }
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
                self->MarkFedLocked(attr.pts);
                // 推进队列首时间戳
                self->pcmPtsNs_ += static_cast<int64_t>(samples) * 1000000000LL / (kSampleRate * kChannels);
                attr.size = static_cast<int32_t>(samples * 2);
                attr.offset = 0;
                attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
                pushIdx = static_cast<int32_t>(index);
            }
        }
        if (pushIdx < 0 && self->running_ && !self->primed_) {
            // 起播静音垫：以零 PCM 把编码链路真正跑起来。产物在真实 PCM 到达前丢弃
            // （见 OnNewOutputData），因此不占用音频时间轴。
            size_t maxSamples = static_cast<size_t>(capacity) / 2;
            size_t samples = kPrimingSamples < maxSamples ? kPrimingSamples : maxSamples;
            samples &= ~static_cast<size_t>(1); // 双声道对齐
            if (samples > 0) {
                memset(addr, 0, samples * 2);
                attr.pts = 0;
                attr.size = static_cast<int32_t>(samples * 2);
                attr.offset = 0;
                attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
                pushIdx = static_cast<int32_t>(index);
                self->primed_ = true;
                MS_LOG_INFO("audio priming silence pushed (%{public}zu samples) to force ASC", samples);
            }
        }
        if (pushIdx < 0) {
            // 暂无音频：把当前输入槽 hold 住（不 Push），避免：
            //  1) 0 长缓冲 → PcmFillFrame 输入 0 采样刷屏 → APP_FREEZE；
            //  2) 静音垫底 → onNeedInputData 紧张循环 → Stop 长时间阻塞（主线程 THREAD_BLOCK_3S）。
            // 待 InputPcm 有真实 PCM 后再用此槽 Push，实现 OBS 式喂音解耦。
            self->idleIndex_ = static_cast<int32_t>(index);
            self->idleMem_ = data;
            return;
        }
    }
    OH_AudioEncoder_PushInputData(codec, static_cast<uint32_t>(pushIdx), attr);
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

    // 静音垫产物：真实 PCM 尚未喂入前全部丢弃，避免占用音频时间轴
    if (self->primed_ && !self->realPcmSeen_) {
        OH_AudioEncoder_FreeOutputData(codec, index);
        return;
    }

    if (size > 0 && self->callbacks_.onOutput) {
        // 输出帧时间轴自维护（编码器自报 pts 是它自己的帧计数器，见头文件说明）：
        // 首帧锚定到首包真实 PCM 的喂入时间，之后每帧推进一个 AAC 帧时长；
        // 喂入侧已明显跑到前面（采集真断过）时只前跳对齐，绝不回退。
        int64_t outPtsUs = 0;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (!self->outAnchored_) {
                self->outPtsUs_ = self->firstFedPtsUs_;
                self->outAnchored_ = true;
            } else if (self->latestFedPtsUs_ - self->outPtsUs_ > kOutResyncUs) {
                self->outPtsUs_ = self->latestFedPtsUs_; // 前跳（采集真断过留下的空洞）
            }
            outPtsUs = self->outPtsUs_;
            self->outPtsUs_ += kAacFrameUs;
        }
        // 诊断：确认输出时间轴已与会话时钟对齐（前 3 帧 + 每 1000 帧）
        static std::atomic<int> outCtr{0};
        int oc = outCtr.fetch_add(1);
        if (oc < 3 || oc % 1000 == 0) {
            MS_LOG_WARN("[AENC-OUT] #%{public}d outPtsUs=%{public}lld (encSelfPts=%{public}lld) size=%{public}d",
                        oc, static_cast<long long>(outPtsUs), static_cast<long long>(bufferAttr->pts), size);
        }
        self->callbacks_.onOutput(frame, size, outPtsUs);
    }
    OH_AudioEncoder_FreeOutputData(codec, index);
}

} // namespace media_stream
