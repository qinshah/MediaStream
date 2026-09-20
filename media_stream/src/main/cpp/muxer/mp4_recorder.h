#ifndef MEDIA_STREAM_MP4_RECORDER_H
#define MEDIA_STREAM_MP4_RECORDER_H

// OH_AVMuxer MP4 封装（H.264 AVCC + AAC 双轨）
//  - 文件名 screen_yyyyMMdd_HHmmss.mp4 于 {filesDir}/videos（fd 经原生 open 传入）
//  - AddTrack 于 Start 前完成：视频 AVC+width/height+CODEC_CONFIG(avcC)、音频 AAC+48000/2+CODEC_CONFIG(ASC)
//  - WriteSampleBuffer pts 单位 μs；关键帧置 AVCODEC_BUFFER_FLAGS_SYNC_FRAME
//  - 独立写线程 + 有界阻塞队列（本地封装正确性优先，不丢帧）
//  - Stop 安全收尾：Stop 写 moov → Destroy → close(fd)；存储不足检测并安全停止

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/bounded_queue.h"

struct OH_AVMuxer;

namespace media_stream {

template <typename T>
using MediaQueue = BoundedQueue<T>;

class Mp4Recorder {
public:
    struct Result {
        std::string fileName;
        std::string filePath;
        int64_t durationMs = 0;
        int64_t sizeBytes = 0;
        bool normal = true; // false=异常收尾（存储不足等）
    };

    struct Callbacks {
        std::function<void(const Result &result)> onFinished;
        std::function<void(int errorCode, const std::string &message)> onError;
    };

    Mp4Recorder() = default;
    ~Mp4Recorder();

    // 启动录制；avcC/asc 必须已就绪（引擎在两者齐备后才调用 Start）
    bool Start(const std::string &dirPath, int width, int height, const std::vector<uint8_t> &avcC,
               const std::vector<uint8_t> &asc, Callbacks callbacks);

    void WriteVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe);
    void WriteAudio(const uint8_t *data, int32_t size, int64_t ptsUs);

    // 停止并安全收尾（幂等；回调 onFinished 在写线程退出前触发）
    void Stop();

    bool IsRecording() const { return recording_.load(); }
    int64_t DurationMs() const;
    int64_t SizeBytes() const { return writtenBytes_.load(); }

private:
    struct Sample {
        std::vector<uint8_t> data;
        int64_t ptsUs = 0;
        uint32_t flags = 0;
        bool isVideo = false;
    };

    void WriteThreadMain();
    bool WriteOneSample(const Sample &sample);

    OH_AVMuxer *muxer_ = nullptr;
    int fd_ = -1;
    int videoTrack_ = -1;
    int audioTrack_ = -1;
    Callbacks callbacks_;

    std::unique_ptr<BoundedQueue<Sample>> queue_;
    std::thread writeThread_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> storageError_{false};

    std::string fileName_;
    std::string filePath_;
    std::atomic<int64_t> firstPtsUs_{-1};
    std::atomic<int64_t> lastPtsUs_{0};
    std::atomic<int64_t> writtenBytes_{0};
    std::atomic<bool> finished_{false};

    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> avcC_;
    std::vector<uint8_t> asc_;
};

} // namespace media_stream

#endif // MEDIA_STREAM_MP4_RECORDER_H
