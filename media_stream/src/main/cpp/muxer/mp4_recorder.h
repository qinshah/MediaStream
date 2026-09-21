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

    // 启动录制；avcC 就绪即可（asc 可选：无系统音频时仅视频轨，静音画面也能落盘）
    bool Start(const std::string &dirPath, int width, int height, const std::vector<uint8_t> &avcC,
               const std::vector<uint8_t> &asc, Callbacks callbacks);

    void WriteVideo(const uint8_t *data, int32_t size, int64_t ptsUs, bool isKeyframe);
    void WriteAudio(const uint8_t *data, int32_t size, int64_t ptsUs);

    // 停止：（幂等）仅打停止标记并唤醒写线程，绝不在此同步 join 写线程。
    // 写线程可能在 OH_AVMuxer_* 内部挂死，同步 join 会把调用线程（主/JS）一起拖死，
    // 被看门狗 THREAD_BLOCK 判冻结杀进程。因此改为 detach，让写线程在后台自行
    // 完成 muxer 收尾（写 moov→Destroy→close→onFinished）。调用方需通过
    // WaitWriteThreadDone() 在销毁本对象前确认写线程已退出，避免悬垂 this(UAF)。
    void Stop();

    // 有界等待写线程结束；返回 true 表示已退出。timeoutMs 上限，避免调用方无限阻塞。
    bool WaitWriteThreadDone(int64_t timeoutMs);

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
    // 归零点未定时的入口：缓存首样本，待两轨齐（或超时）后封零点；已定则直接落盘
    bool WriteOneSample(const Sample &sample);
    // 归零点已定：真正调用 OH_AVMuxer_WriteSampleBuffer
    bool WriteSampleNow(const Sample &sample);
    // 由待定首样本求 min（两轨中较早者）作为归零点，保证没有一轨被钳位
    void SealFirstPtsLocked();

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
    // 首/末采样 pts（μs）仅用于 mp4 采样缓冲的相对 pts 归零
    std::atomic<int64_t> firstPtsUs_{-1};
    // 归零点延迟确定（见 .cpp 注释）：录制起点的补帧是同步投递，会比异步的音频编码回调
    // 更早到达写线程。若沿用「第一个写入的样本定零点」，内容更早的音频就成了负值被钳到 0，
    // 多帧挤在同一时刻 → 爆音。这里改为等两轨首样本到齐，取较早者作零点。
    std::vector<Sample> pendingFirst_;
    bool firstSealed_ = false;
    bool firstVideoIn_ = false;
    bool firstAudioIn_ = false;
    int64_t pendingFirstStartMs_ = 0;
    bool hasVideoTrack_ = false;
    bool hasAudioTrack_ = false;
    std::atomic<int64_t> droppedEarly_{0};
    std::atomic<int64_t> lastPtsUs_{0};
    // 录制时长按真实墙钟计算（设备编码器 pts 绝对时钟不可靠，不能用 (last-first)/1000）
    std::atomic<int64_t> startSteadyMs_{0};
    // 写线程收尾时刻（0=尚未收尾）。收尾后 DurationMs() 固定返回 stop-start，
    // 避免「文件已写完 moov、但另一个输出还在跑」时界面上的录制时长继续上涨。
    std::atomic<int64_t> stopSteadyMs_{0};
    std::atomic<int64_t> writtenBytes_{0};
    std::atomic<int64_t> writtenSamples_{0};
    std::atomic<bool> finished_{false};
    // 写线程是否已完全退出（含 muxer 收尾）。Stop 改为 detach 后用此标志让调用方
    // 在销毁本对象前做有界等待，避免悬垂 this(UAF)。
    std::atomic<bool> writeThreadDone_{false};

    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> avcC_;
    std::vector<uint8_t> asc_;
};

} // namespace media_stream

#endif // MEDIA_STREAM_MP4_RECORDER_H
