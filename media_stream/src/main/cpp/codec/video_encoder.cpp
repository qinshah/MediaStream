#include "video_encoder.h"

#include <cstring>

#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>

#include "../common/logger.h"

namespace media_stream {

// H.264 NALU type
static constexpr int kNalSps = 7;
static constexpr int kNalPps = 8;

VideoEncoder::~VideoEncoder() {
    Stop();
}

bool VideoEncoder::Start(int width, int height, int fps, int bitrateKbps, Callbacks callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return true;
    }
    callbacks_ = std::move(callbacks);
    width_ = width;
    height_ = height;
    avccEmitted_ = false;
    avcc_.clear();

    encoder_ = OH_VideoEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (encoder_ == nullptr) {
        MS_LOG_ERROR("OH_VideoEncoder_CreateByMime failed");
        return false;
    }

    OH_AVFormat *format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, fps);
    OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, static_cast<int64_t>(bitrateKbps) * 1000);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, BITRATE_MODE_CBR);
    // 关键帧间隔 2s（单位毫秒）；直播端可快速起播
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, 2000);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PROFILE, AVC_PROFILE_HIGH);

    int32_t rc = OH_VideoEncoder_Configure(encoder_, format);
    OH_AVFormat_Destroy(format);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_VideoEncoder_Configure failed rc=%{public}d", rc);
        OH_VideoEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }

    OH_AVCodecCallback cb = {OnCodecError, OnStreamChanged, OnNeedOutputBuffer};
    rc = OH_VideoEncoder_RegisterCallback(encoder_, cb, this);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_VideoEncoder_RegisterCallback failed rc=%{public}d", rc);
        OH_VideoEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }

    rc = OH_VideoEncoder_Prepare(encoder_);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_VideoEncoder_Prepare failed rc=%{public}d", rc);
        OH_VideoEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }
    rc = OH_VideoEncoder_Start(encoder_);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_VideoEncoder_Start failed rc=%{public}d", rc);
        OH_VideoEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        return false;
    }
    running_ = true;
    MS_LOG_INFO("VideoEncoder started %{public}dx%{public}d@%{public}d %{public}dkbps", width, height, fps,
                bitrateKbps);
    return true;
}

void VideoEncoder::InputFrame(const uint8_t *nv12, int64_t ptsUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || encoder_ == nullptr || nv12 == nullptr) {
        return;
    }
    // 查询可用输入缓冲（非阻塞，0 超时——无空闲缓冲则丢帧，不背压采集）
    uint32_t index = 0;
    int32_t rc = OH_VideoEncoder_QueryInputBuffer(encoder_, &index, 0);
    if (rc != AV_ERR_OK) {
        return;
    }
    OH_AVBuffer *buffer = OH_VideoEncoder_GetInputBuffer(encoder_, index);
    if (buffer == nullptr) {
        return;
    }
    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);
    int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
    int32_t need = width_ * height_ * 3 / 2;
    if (addr == nullptr || capacity < need) {
        return;
    }
    memcpy(addr, nv12, need);
    OH_AVCodecBufferAttr attr = {};
    attr.pts = ptsUs;
    attr.size = need;
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(buffer, &attr);
    OH_VideoEncoder_PushInputBuffer(encoder_, index);
}

void VideoEncoder::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ != nullptr) {
        if (running_) {
            OH_VideoEncoder_Stop(encoder_);
            running_ = false;
        }
        OH_VideoEncoder_Destroy(encoder_);
        encoder_ = nullptr;
        MS_LOG_INFO("VideoEncoder stopped");
    }
}

// —— 回调桥接 ——

void VideoEncoder::OnCodecError(OH_AVCodec *codec, int32_t errorCode, void *userData) {
    (void)codec;
    auto *self = static_cast<VideoEncoder *>(userData);
    MS_LOG_ERROR("VideoEncoder error %{public}d", errorCode);
    if (self != nullptr && self->callbacks_.onError) {
        self->callbacks_.onError(errorCode);
    }
}

void VideoEncoder::OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *fmt, void *userData) {
    (void)codec;
    auto *self = static_cast<VideoEncoder *>(userData);
    if (self == nullptr || fmt == nullptr) {
        return;
    }
    // 部分实现在输出描述里直接携带 avcC（OH_MD_KEY_CODEC_CONFIG）
    uint8_t *config = nullptr;
    size_t configSize = 0;
    if (OH_AVFormat_GetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG, &config, &configSize) && config != nullptr &&
        configSize > 0 && !self->avccEmitted_) {
        self->avcc_.assign(config, config + configSize);
        self->avccEmitted_ = true;
        if (self->callbacks_.onCodecConfig) {
            self->callbacks_.onCodecConfig(self->avcc_);
        }
        MS_LOG_INFO("avcC from output description, %{public}zu bytes", configSize);
    }
}

void VideoEncoder::OnNeedOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *avBuffer,
                                      void *userData) {
    auto *self = static_cast<VideoEncoder *>(userData);
    if (self == nullptr || avBuffer == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->encoder_ == nullptr || !self->running_) {
            return;
        }
        OH_AVCodecBufferAttr attr = {};
        if (OH_AVBuffer_GetBufferAttr(avBuffer, &attr) != AV_ERR_OK || attr.size <= 0) {
            OH_VideoEncoder_FreeOutputBuffer(codec, index);
            return;
        }
        uint8_t *addr = OH_AVBuffer_GetAddr(avBuffer);
        if (addr == nullptr) {
            OH_VideoEncoder_FreeOutputBuffer(codec, index);
            return;
        }
        const uint8_t *data = addr + attr.offset;
        int32_t size = attr.size;

        if ((attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) != 0) {
            // codec config 缓冲：内容即 avcC
            if (!self->avccEmitted_ && size > 0) {
                self->avcc_.assign(data, data + size);
                self->avccEmitted_ = true;
                if (self->callbacks_.onCodecConfig) {
                    self->callbacks_.onCodecConfig(self->avcc_);
                }
                MS_LOG_INFO("avcC from codec data buffer, %{public}d bytes", size);
            }
            OH_VideoEncoder_FreeOutputBuffer(codec, index);
            return;
        }

        bool isKeyframe = (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0;

        // Annex-B → AVCC（编码器输出为 Annex-B 时转换；已是 AVCC 则透传）
        const uint8_t *outData = data;
        int32_t outSize = size;
        if (size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
            if (!self->avccEmitted_) {
                self->ExtractAvcc(data, size);
            }
            if (self->ConvertAnnexBToAvcc(data, size, self->convertScratch_)) {
                outData = self->convertScratch_.data();
                outSize = static_cast<int32_t>(self->convertScratch_.size());
            }
        }

        if (self->callbacks_.onOutput) {
            self->callbacks_.onOutput(outData, outSize, attr.pts, isKeyframe);
        }
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
    }
}

// —— Annex-B / AVCC 工具 ——

// 查找下一个 start code（00 00 01 或 00 00 00 01）；返回 NALU 起始偏移，*pPrefixLen 为前缀长度
static int32_t FindStartCode(const uint8_t *data, int32_t size, int32_t from, int32_t &prefixLen) {
    for (int32_t i = from; i + 2 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                prefixLen = 3;
                return i + 3;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                prefixLen = 4;
                return i + 4;
            }
        }
    }
    prefixLen = 0;
    return -1;
}

bool VideoEncoder::ConvertAnnexBToAvcc(const uint8_t *data, int32_t size, std::vector<uint8_t> &out) {
    out.clear();
    int32_t prefixLen = 0;
    int32_t naluStart = FindStartCode(data, size, 0, prefixLen);
    if (naluStart < 0) {
        return false; // 无 start code，按 AVCC 透传
    }
    while (naluStart >= 0) {
        int32_t nextPrefix = 0;
        int32_t nextNalu = FindStartCode(data, size, naluStart, nextPrefix);
        int32_t naluEnd = nextNalu >= 0 ? nextNalu - nextPrefix : size;
        int32_t naluSize = naluEnd - naluStart;
        if (naluSize > 0) {
            uint32_t be = (static_cast<uint32_t>(naluSize) >> 24) | ((static_cast<uint32_t>(naluSize) >> 8) & 0xFF00) |
                          ((static_cast<uint32_t>(naluSize) << 8) & 0xFF0000) |
                          (static_cast<uint32_t>(naluSize) << 24);
            size_t oldSize = out.size();
            out.resize(oldSize + 4 + naluSize);
            memcpy(out.data() + oldSize, &be, 4);
            memcpy(out.data() + oldSize + 4, data + naluStart, naluSize);
        }
        naluStart = nextNalu;
    }
    return !out.empty();
}

void VideoEncoder::ExtractAvcc(const uint8_t *data, int32_t size) {
    // 从 Annex-B 流提取 SPS/PPS 构建 avcC（AVCDecoderConfigurationRecord）
    std::vector<uint8_t> sps, pps;
    int32_t prefixLen = 0;
    int32_t naluStart = FindStartCode(data, size, 0, prefixLen);
    while (naluStart >= 0) {
        int32_t nextPrefix = 0;
        int32_t nextNalu = FindStartCode(data, size, naluStart, nextPrefix);
        int32_t naluEnd = nextNalu >= 0 ? nextNalu - nextPrefix : size;
        int32_t naluSize = naluEnd - naluStart;
        if (naluSize > 0) {
            int nalType = data[naluStart] & 0x1F;
            if (nalType == kNalSps && sps.empty()) {
                sps.assign(data + naluStart, data + naluStart + naluSize);
            } else if (nalType == kNalPps && pps.empty()) {
                pps.assign(data + naluStart, data + naluStart + naluSize);
            }
        }
        if (!sps.empty() && !pps.empty()) {
            break;
        }
        naluStart = nextNalu;
    }
    if (sps.size() < 4 || pps.empty()) {
        return;
    }
    // avcC 结构：configurationVersion(1) AVCProfileIndication(1) profile_compatibility(1)
    // AVCLevelIndication(1) lengthSizeMinusOne(1|0xFC) numSPS(1|0xE0) [spsLen(2) sps] numPPS(1) [ppsLen(2) pps]
    std::vector<uint8_t> avcc;
    avcc.push_back(0x01);
    avcc.push_back(sps[1]);
    avcc.push_back(sps[2]);
    avcc.push_back(sps[3]);
    avcc.push_back(0xFF); // 6 bits reserved + lengthSizeMinusOne=3（4 字节长度）
    avcc.push_back(0xE1); // 3 bits reserved + numSPS=1
    avcc.push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(0x01); // numPPS
    avcc.push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    avcc.insert(avcc.end(), pps.begin(), pps.end());

    avcc_ = std::move(avcc);
    avccEmitted_ = true;
    if (callbacks_.onCodecConfig) {
        callbacks_.onCodecConfig(avcc_);
    }
    MS_LOG_INFO("avcC extracted from Annex-B, %{public}zu bytes", avcc_.size());
}

} // namespace media_stream
