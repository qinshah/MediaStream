#include "video_encoder.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_buffer/native_buffer.h>

#include "../common/logger.h"

namespace media_stream {

// H.264 NALU type
static constexpr int kNalSps = 7;
static constexpr int kNalPps = 8;

// 临时：dump 一段字节的 hex，用于排查 avcC 是否完整(是否含 SPS/PPS, numSPS 是否=1)
static void DumpHex(const char *tag, const uint8_t *p, size_t n) {
    if (p == nullptr || n == 0) {
        MS_LOG_WARN("[HEX] %{public}s empty", tag);
        return;
    }
    const size_t kShow = n > 16 ? 16 : n;
    char buf[64];
    for (size_t i = 0; i < kShow; i++) {
        snprintf(buf + i * 2, 3, "%02x", p[i]);
    }
    buf[kShow * 2] = 0;
    uint8_t ns = n >= 6 ? (p[5] & 0x1F) : 0xFF; // numSPS byte(索引5)
    MS_LOG_WARN("[HEX] %{public}s n=%{public}zu prefix=%{public}s numSPS=%{public}d", tag, n, buf, ns);
}

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

    // OH_AVCodecCallback 四个字段必须显式给定，否则缺省为 nullptr 导致 RegisterCallback 失败(rc=3, onNewOutputBuffer is nullptr)。
    // 输入采用 QueryInputBuffer 拉取式主动投递，onNeedInputBuffer 只需空实现占位。
    OH_AVCodecCallback cb = {OnCodecError, OnStreamChanged, OnNeedInputBuffer, OnNeedOutputBuffer};
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
    aliveFlag_.store(true);
    lastKeyReqMs_.store(0); // 首帧即请求 IDR，保证流一开始就有可解码的随机访问点
    MS_LOG_INFO("VideoEncoder started %{public}dx%{public}d@%{public}d %{public}dkbps", width, height, fps,
                bitrateKbps);
    return true;
}

void VideoEncoder::InputFrame(const uint8_t *nv12, int64_t ptsUs) {
    if (nv12 == nullptr) {
        return;
    }
    OH_AVCodec *enc = nullptr;
    uint32_t pushIdx = 0;
    bool push = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || encoder_ == nullptr) {
            return;
        }
        enc = encoder_; // 供锁外请求 IDR（Push 路径也会赋值，此处保证无 Push 时同样可用）
        int32_t need = width_ * height_ * 3 / 2;
        // 队列有界：队满丢最旧一帧（不背压采集）
        if (pendingFrames_.size() >= kMaxPendingFrames) {
            pendingFrames_.pop_front();
            pendingPtsUs_.pop_front();
        }
        pendingFrames_.emplace_back(nv12, nv12 + need);
        pendingPtsUs_.push_back(ptsUs);
        // 已有暂留输入槽 → 立即填回并投递，避免真实帧在队列里空等下一次回调
        if (idleIndex_ >= 0 && idleBuffer_ != nullptr) {
            uint32_t idx = static_cast<uint32_t>(idleIndex_);
            OH_AVBuffer *buf = idleBuffer_;
            idleIndex_ = -1;
            idleBuffer_ = nullptr;
            if (FillSlotLocked(idx, buf)) {
                enc = encoder_;
                pushIdx = idx;
                push = true;
            }
        }
    }
    // Push 必须在锁外：OH_VideoEncoder_PushInputBuffer 可能同步触发编码器回调，
    // 而回调要取本 mutex_，持锁 Push 会自锁（与 OnNeedOutputBuffer 的锁序修复同源）。
    if (push) {
        OH_VideoEncoder_PushInputBuffer(enc, pushIdx);
    }
    // 按时间请求 IDR（锁外，避免与编码器回调线程形成锁序问题）
    MaybeRequestKeyFrame(enc);
}

// 将队首帧写入输入槽（须持 mutex_）。容量不足时保留该帧、交由调用方转为暂留槽。
bool VideoEncoder::FillSlotLocked(uint32_t index, OH_AVBuffer *buffer) {
    if (buffer == nullptr || pendingFrames_.empty()) {
        return false;
    }
    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);
    int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
    const std::vector<uint8_t> &frame = pendingFrames_.front();
    if (addr == nullptr || capacity < static_cast<int32_t>(frame.size())) {
        MS_LOG_WARN("video input slot too small cap=%{public}d need=%{public}zu", capacity, frame.size());
        return false;
    }

    // 输入槽可能带行距填充（stride > width）。编码器按 stride 逐行寻址，若我们按紧凑
    // width 连续整块写入，Y/UV 平面立即错位：画面斜向撕裂，且尾部未被覆盖的区域整段读到 0
    // （Y=U=V=0 → 解码纯绿）。真机实测 720x1632 时底部约 1/6 解出绿块、上方严重斜条纹。
    // NV12 的 Y 与 UV 都是 1 字节/样本，因此 stride 无论以字节还是像素给出，只要 >= width
    // 就可直接当作「字节/行」使用。
    size_t yStride = static_cast<size_t>(width_);
    size_t uvStride = static_cast<size_t>(width_);
    OH_NativeBuffer *nb = OH_AVBuffer_GetNativeBuffer(buffer);
    if (nb != nullptr) {
        OH_NativeBuffer_Config cfg = {};
        OH_NativeBuffer_GetConfig(nb, &cfg);
        if (slotLogged_ < 2) {
            slotLogged_++;
            MS_LOG_WARN("[VENC] input slot cfg w=%{public}d h=%{public}d stride=%{public}d fmt=%{public}d "
                        "usage=%{public}d cap=%{public}d need=%{public}zu",
                        cfg.width, cfg.height, cfg.stride, cfg.format, cfg.usage, capacity, frame.size());
        }
        if (cfg.stride >= width_) {
            yStride = static_cast<size_t>(cfg.stride);
            uvStride = yStride;
        }
    }
    const size_t yBytes = yStride * static_cast<size_t>(height_);
    const size_t uvBytes = uvStride * (static_cast<size_t>(height_) / 2);
    if (uvStride == static_cast<size_t>(width_) || capacity < static_cast<int32_t>(yBytes + uvBytes)) {
        // 无行距填充（或槽容量装不下带行距的布局）：按紧凑布局整块写入
        memcpy(addr, frame.data(), frame.size());
    } else {
        // 带行距：逐行搬运 Y 与 UV 两个平面
        const uint8_t *srcY = frame.data();
        const uint8_t *srcUV = frame.data() + static_cast<size_t>(width_) * height_;
        for (int row = 0; row < height_; row++) {
            memcpy(addr + static_cast<size_t>(row) * yStride, srcY + static_cast<size_t>(row) * width_,
                   static_cast<size_t>(width_));
        }
        uint8_t *dstUV = addr + yBytes;
        for (int row = 0; row < height_ / 2; row++) {
            memcpy(dstUV + static_cast<size_t>(row) * uvStride, srcUV + static_cast<size_t>(row) * width_,
                   static_cast<size_t>(width_));
        }
    }
    OH_AVCodecBufferAttr attr = {};
    attr.pts = pendingPtsUs_.front();
    attr.size = static_cast<int32_t>(frame.size());
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(buffer, &attr);
    pendingFrames_.pop_front();
    pendingPtsUs_.pop_front();
    if (pushedCnt_ < 3) {
        pushedCnt_++;
        MS_LOG_INFO("pushed input frame #%{public}d via callback (index=%{public}u size=%{public}zu)", pushedCnt_,
                    index, frame.size());
    }
    return true;
}

void VideoEncoder::Stop() {
    // 勿在持有 mutex_ 时调用 OH_VideoEncoder_Stop/Destroy：编码器工作线程（OnNeedInputBuffer
    // 等）也会锁本 mutex_，若 Stop 等待该工作线程返回，会形成锁序反转死锁。
    // 因此在锁内仅置空指针并摘除运行标志，真正的 OH 停止/销毁放到锁外执行。
    OH_AVCodec *enc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enc = encoder_;
        encoder_ = nullptr;
        running_ = false;
        aliveFlag_.store(false); // 先失效再销毁，避免锁外请求 IDR 触到已释放编码器
        // 暂留槽随编码器销毁失效，置空避免悬垂
        idleIndex_ = -1;
        idleBuffer_ = nullptr;
        pendingFrames_.clear();
        pendingPtsUs_.clear();
    }
    if (enc != nullptr) {
        OH_VideoEncoder_Stop(enc);
        OH_VideoEncoder_Destroy(enc);
        MS_LOG_INFO("VideoEncoder stopped");
    }
}

void VideoEncoder::MaybeRequestKeyFrame(OH_AVCodec *enc) {
    if (enc == nullptr || !aliveFlag_.load()) {
        return;
    }
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    int64_t last = lastKeyReqMs_.load();
    if (last != 0 && nowMs - last < kKeyFrameIntervalMs) {
        return;
    }
    lastKeyReqMs_.store(nowMs);
    OH_AVFormat *fmt = OH_AVFormat_Create();
    if (fmt == nullptr) {
        return;
    }
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_REQUEST_I_FRAME, 1);
    OH_AVErrCode rc = OH_VideoEncoder_SetParameter(enc, fmt);
    OH_AVFormat_Destroy(fmt);
    if (rc != AV_ERR_OK) {
        MS_LOG_WARN("request IDR failed rc=%{public}d", rc);
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

// 输入缓冲回调：从待编码队列取出一帧 NV12，拷入编码器输入缓冲并 Push。
// 队列为空时**暂留**该输入槽（不 Push）——见头文件 idleIndex_ 处对绿屏根因的说明。
void VideoEncoder::OnNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData) {
    auto *self = static_cast<VideoEncoder *>(userData);
    if (self == nullptr || buffer == nullptr) {
        return;
    }
    bool push = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (!self->running_ || self->encoder_ == nullptr) {
            return;
        }
        push = self->FillSlotLocked(index, buffer);
        if (!push) {
            // 无帧可编（或槽容量不足）：把槽暂留下来，等 InputFrame 有真实帧再填回。
            // 已在暂留中则忽略本次回调（最多只持一个槽，与 AudioEncoder 策略一致）。
            if (self->idleIndex_ < 0) {
                self->idleIndex_ = static_cast<int32_t>(index);
                self->idleBuffer_ = buffer;
            }
            return;
        }
    }
    OH_VideoEncoder_PushInputBuffer(codec, index);
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
        // 若该 config 是 Annex-B(起始码开头)，则用 ExtractAvcc 正规提取 SPS/PPS；
        // 否则视为已是标准 AVCC record，直接采用。
        bool isAnnexB = configSize >= 4 && config[0] == 0 && config[1] == 0 &&
                        (config[2] == 1 || (config[2] == 0 && config[3] == 1));
        if (isAnnexB) {
            self->ExtractAvcc(config, configSize);
        } else {
            self->avcc_.assign(config, config + configSize);
            self->avccEmitted_ = true;
            if (self->callbacks_.onCodecConfig) {
                self->callbacks_.onCodecConfig(self->avcc_);
            }
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

    // —— 锁序安全（修复“停止录屏后卡死闪退”死锁）——
    // 本回调运行在编码器输出线程。旧实现把 callbacks_.onOutput（→引擎 OnEncodedVideo，会取引擎
    // mutex_）也放在 self->mutex_(编码器锁) 内。与此同时采集线程在持有引擎 mutex_ 时又调
    // InputFrame 来取本编码器 mutex_：两条路径互相等待对方手里的锁 → 环形等待死锁 → 所有引擎
    // 线程被拖住，主线程停止录屏时卡在 StopRecording → 被 THREAD_BLOCK_3S 看门狗判冻结并杀进程。
    // 这与参考 OBS 的适配约束一致：绝不在持有本侧锁时回调上层/下层（避免锁序反转）。
    // 修复：在锁内仅拷贝出最终样本并释放输出缓冲，把真正分发到引擎(onOutput→OnEncodedVideo)
    // 移到锁外执行，切断 engine↔encoder 的锁序反转。
    std::vector<uint8_t> out;
    int64_t ptsUs = 0;
    bool isKey = false;
    bool dispatch = false;
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
            // codec config 缓冲：OH 编码器返回的通常是「Annex-B 起始码 + SPS/PPS NALU」，
            // 并非标准 AVCC record。若直接当作 OH_MD_KEY_CODEC_CONFIG 传给封装器，会写出
            // 缺少 SPS/PPS 的 avcC，导致任何解码器都无法初始化（黑屏/绿屏/无法播放）。
            // 因此这里用 ExtractAvcc 从该 Annex-B 里正规提取 SPS/PPS 并构造标准 avcC。
            if (!self->avccEmitted_ && size > 0) {
                self->ExtractAvcc(data, size); // 内部成功时会设置 avccEmitted_ 并回调 onCodecConfig
            }
            OH_VideoEncoder_FreeOutputBuffer(codec, index);
            return;
        }

        isKey = (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0;
        ptsUs = attr.pts;

        // Annex-B → AVCC（编码器输出为 Annex-B 时转换；已是 AVCC 则透传）
        const uint8_t *finalData = data;
        int32_t finalSize = size;
        if (size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
            if (!self->avccEmitted_) {
                self->ExtractAvcc(data, size);
            }
            if (self->ConvertAnnexBToAvcc(data, size, self->convertScratch_)) {
                finalData = self->convertScratch_.data();
                finalSize = static_cast<int32_t>(self->convertScratch_.size());
            }
        }

        // 锁内先拷贝最终样本（样本通常很小；拷贝后可释放输出缓冲并放开编码器锁，
        // 避免跨锁引用 convertScratch_/OH_AVBuffer 的悬垂），随后在锁外安全分发到引擎。
        out.assign(finalData, finalData + finalSize);
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
        dispatch = true;
    }
    // 锁外分发到引擎（OnEncodedVideo 取引擎 mutex_ 不再与本编码器 mutex_ 相交，死锁解除）
    if (dispatch && self->callbacks_.onOutput) {
        self->callbacks_.onOutput(out.data(), static_cast<int32_t>(out.size()), ptsUs, isKey);
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
    DumpHex("avcC extractAvcc", avcc_.data(), avcc_.size());
    if (callbacks_.onCodecConfig) {
        callbacks_.onCodecConfig(avcc_);
    }
    MS_LOG_INFO("avcC extracted from Annex-B, %{public}zu bytes", avcc_.size());
}

} // namespace media_stream
