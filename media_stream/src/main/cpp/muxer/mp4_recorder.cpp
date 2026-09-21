#include "mp4_recorder.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmuxer.h>

#include "../common/bounded_queue.h"
#include "../common/logger.h"

namespace media_stream {

// ErrorCode 与 Index.d.ts 对齐
static constexpr int kErrStorageFull = 11;
static constexpr int kErrInternal = 12;

static constexpr size_t kWriteQueueCap = 600; // 阻塞策略；磁盘写快，常规不满

Mp4Recorder::~Mp4Recorder() {
    Stop();
}

bool Mp4Recorder::Start(const std::string &dirPath, int width, int height, const std::vector<uint8_t> &avcC,
                        const std::vector<uint8_t> &asc, Callbacks callbacks) {
    if (recording_.exchange(true)) {
        MS_LOG_WARN("Mp4Recorder already recording, ignore Start");
        return true;
    }
    callbacks_ = std::move(callbacks);
    width_ = width;
    height_ = height;
    avcC_ = avcC;
    asc_ = asc;
    stopRequested_ = false;
    storageError_ = false;
    finished_ = false;
    firstPtsUs_ = -1;
    lastPtsUs_ = 0;
    writtenBytes_ = 0;
    // 录制起点（真实墙钟，用于时长统计）
    startSteadyMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    // 确保录制目录存在
    (void)mkdir(dirPath.c_str(), 0755);

    // 文件名含录制起始时间戳
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = {};
    localtime_r(&t, &tm);
    char name[64];
    snprintf(name, sizeof(name), "screen_%04d%02d%02d_%02d%02d%02d.mp4", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    fileName_ = name;
    filePath_ = dirPath + "/" + fileName_;

    fd_ = open(filePath_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd_ < 0) {
        MS_LOG_ERROR("open %{public}s failed errno=%{public}d", filePath_.c_str(), errno);
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "创建录制文件失败");
        }
        return false;
    }

    muxer_ = OH_AVMuxer_Create(fd_, AV_OUTPUT_FORMAT_MPEG_4);
    if (muxer_ == nullptr) {
        MS_LOG_ERROR("OH_AVMuxer_Create failed");
        close(fd_);
        fd_ = -1;
        unlink(filePath_.c_str());
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "创建 MP4 封装器失败");
        }
        return false;
    }

    // 视频轨：AVC + 宽高 + avcC
    OH_AVFormat *vfmt = OH_AVFormat_Create();
    OH_AVFormat_SetStringValue(vfmt, OH_MD_KEY_CODEC_MIME, OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    OH_AVFormat_SetIntValue(vfmt, OH_MD_KEY_WIDTH, width_);
    OH_AVFormat_SetIntValue(vfmt, OH_MD_KEY_HEIGHT, height_);
    if (!avcC_.empty()) {
        OH_AVFormat_SetBuffer(vfmt, OH_MD_KEY_CODEC_CONFIG, avcC_.data(), avcC_.size());
    }
    int32_t rc = OH_AVMuxer_AddTrack(muxer_, &videoTrack_, vfmt);
    OH_AVFormat_Destroy(vfmt);
    if (rc != AV_ERR_OK || videoTrack_ < 0) {
        MS_LOG_ERROR("AddTrack video failed rc=%{public}d", rc);
        OH_AVMuxer_Destroy(muxer_);
        muxer_ = nullptr;
        close(fd_);
        fd_ = -1;
        unlink(filePath_.c_str());
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "添加视频轨失败");
        }
        return false;
    }

    // 音频轨（可选）：系统内录音频在无播放时无 ASC，此时仅视频轨，静音录制也能正常落盘
    audioTrack_ = -1;
    if (!asc_.empty()) {
        OH_AVFormat *afmt = OH_AVFormat_Create();
        OH_AVFormat_SetStringValue(afmt, OH_MD_KEY_CODEC_MIME, OH_AVCODEC_MIMETYPE_AUDIO_AAC);
        OH_AVFormat_SetIntValue(afmt, OH_MD_KEY_AUD_SAMPLE_RATE, 48000);
        OH_AVFormat_SetIntValue(afmt, OH_MD_KEY_AUD_CHANNEL_COUNT, 2);
        OH_AVFormat_SetBuffer(afmt, OH_MD_KEY_CODEC_CONFIG, asc_.data(), asc_.size());
        rc = OH_AVMuxer_AddTrack(muxer_, &audioTrack_, afmt);
        OH_AVFormat_Destroy(afmt);
        if (rc != AV_ERR_OK || audioTrack_ < 0) {
            // 音频轨添加失败 → 降级为仅视频轨，不中断录制
            audioTrack_ = -1;
            MS_LOG_WARN("AddTrack audio failed rc=%{public}d, record video-only", rc);
        }
    } else {
        MS_LOG_INFO("no ASC (no system audio), record video-only");
    }

    rc = OH_AVMuxer_Start(muxer_);
    if (rc != AV_ERR_OK) {
        MS_LOG_ERROR("OH_AVMuxer_Start failed rc=%{public}d", rc);
        OH_AVMuxer_Destroy(muxer_);
        muxer_ = nullptr;
        close(fd_);
        fd_ = -1;
        unlink(filePath_.c_str());
        recording_ = false;
        if (callbacks_.onError) {
            callbacks_.onError(kErrInternal, "启动 MP4 封装失败");
        }
        return false;
    }

    queue_ = std::make_unique<BoundedQueue<Sample>>(kWriteQueueCap, OverflowPolicy::kBlockOnFull);
    writeThread_ = std::thread(&Mp4Recorder::WriteThreadMain, this);
    MS_LOG_INFO("Mp4Recorder started: %{public}s", filePath_.c_str());
    return true;
}

void Mp4Recorder::WriteVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe) {
    if (!recording_.load() || stopRequested_.load() || data == nullptr || size <= 0) {
        return;
    }
    Sample s;
    s.data.assign(data, data + size);
    s.ptsUs = ptsUs;
    s.flags = isKeyframe ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    s.isVideo = true;
    if (queue_ && !queue_->Push(std::move(s))) {
        MS_LOG_WARN("mp4 write queue closed, drop video sample");
    }
}

void Mp4Recorder::WriteAudio(const uint8_t *data, int32_t size, int64_t ptsUs) {
    if (audioTrack_ < 0) {
        return; // 无音频轨（系统内录无 ASC）
    }
    if (!recording_.load() || stopRequested_.load() || data == nullptr || size <= 0) {
        return;
    }
    Sample s;
    s.data.assign(data, data + size);
    s.ptsUs = ptsUs;
    s.flags = AVCODEC_BUFFER_FLAGS_NONE;
    s.isVideo = false;
    if (queue_ && !queue_->Push(std::move(s))) {
        MS_LOG_WARN("mp4 write queue closed, drop audio sample");
    }
}

int64_t Mp4Recorder::DurationMs() const {
    // 以真实墙钟计算录制时长（编码器绝对 pts 时钟不可靠，避免转换成极不合理的时长）
    int64_t start = startSteadyMs_.load();
    if (start <= 0) {
        return 0;
    }
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    return now - start;
}

void Mp4Recorder::Stop() {
    if (!recording_.exchange(false)) {
        return;
    }
    stopRequested_ = true;
    if (queue_) {
        queue_->Close(); // 唤醒写线程，排空后退出
    }
    if (writeThread_.joinable()) {
        writeThread_.join();
    }
    recording_ = false;
}

bool Mp4Recorder::WriteOneSample(const Sample &sample) {
    OH_AVBuffer *buffer = OH_AVBuffer_Create(static_cast<int32_t>(sample.data.size()));
    if (buffer == nullptr) {
        return false;
    }
    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);
    memcpy(addr, sample.data.data(), sample.data.size());
    OH_AVCodecBufferAttr attr = {};
    // pts 归零：以首采样为起点，单调递增
    int64_t first = firstPtsUs_.load();
    if (first < 0) {
        firstPtsUs_.compare_exchange_strong(first, sample.ptsUs);
        first = sample.ptsUs;
    }
    attr.pts = sample.ptsUs - first;
    if (attr.pts < 0) {
        attr.pts = 0;
    }
    attr.size = static_cast<int32_t>(sample.data.size());
    attr.offset = 0;
    attr.flags = sample.flags;
    OH_AVBuffer_SetBufferAttr(buffer, &attr);

    int32_t track = sample.isVideo ? videoTrack_ : audioTrack_;
    int32_t rc = OH_AVMuxer_WriteSampleBuffer(muxer_, track, buffer);
    OH_AVBuffer_Destroy(buffer);
    if (rc != AV_ERR_OK) {
        // 常见为存储不足
        MS_LOG_ERROR("WriteSampleBuffer failed rc=%{public}d", rc);
        storageError_ = true;
        return false;
    }
    writtenBytes_ += sample.data.size();
    int64_t last = lastPtsUs_.load();
    if (sample.ptsUs > last) {
        lastPtsUs_.store(sample.ptsUs);
    }
    return true;
}

void Mp4Recorder::WriteThreadMain() {
    // 排空队列（Close 后 Pop 在排空后返回 false）
    Sample s;
    while (queue_ && queue_->Pop(s)) {
        if (!WriteOneSample(s)) {
            break; // 存储错误：停止写入，走异常收尾
        }
    }

    // 安全收尾：Stop 写 moov → Destroy → close(fd)
    if (muxer_ != nullptr) {
        OH_AVMuxer_Stop(muxer_);
        OH_AVMuxer_Destroy(muxer_);
        muxer_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    if (finished_.exchange(true)) {
        return;
    }
    Result result;
    result.fileName = fileName_;
    result.filePath = filePath_;
    result.durationMs = DurationMs();
    // 实际文件大小以磁盘为准
    struct stat st = {};
    if (stat(filePath_.c_str(), &st) == 0) {
        result.sizeBytes = st.st_size;
    } else {
        result.sizeBytes = writtenBytes_.load();
    }
    result.normal = !storageError_.load();
    MS_LOG_INFO("Mp4Recorder finished: %{public}s %{public}lldms %{public}lldB normal=%{public}d",
                filePath_.c_str(), result.durationMs, result.sizeBytes, result.normal ? 1 : 0);
    if (storageError_.load() && callbacks_.onError) {
        callbacks_.onError(kErrStorageFull, "存储空间不足，录制已停止");
    }
    if (callbacks_.onFinished) {
        callbacks_.onFinished(result);
    }
}

} // namespace media_stream
