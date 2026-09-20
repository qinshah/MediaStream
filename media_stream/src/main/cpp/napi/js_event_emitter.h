#ifndef MEDIA_STREAM_JS_EVENT_EMITTER_H
#define MEDIA_STREAM_JS_EVENT_EMITTER_H

// TSFN 封装：将原生线程产生的事件线程安全地投递到 ArkTS 线程
// stats 事件合并投递：队列中已有未投递 stats 时以新值替换（节流语义，节流周期由引擎定时器保证）

#include <napi/native_api.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace media_stream {

// 事件类型（与 Index.d.ts 的 MediaStreamEvent 判别联合一一对应）
enum class EventType {
    kCaptureState,
    kStreamState,
    kRecordState,
    kStats,
    kRecordFinished,
    kError,
    kMicDegraded,
};

// 跨线程传递的事件负载（POD + std::string，值拷贝入队）
struct EventData {
    EventType type = EventType::kCaptureState;

    // 状态类事件负载
    std::string state;

    // stats 负载（可选字段以 has* 标记区分）
    bool hasStreamDuration = false;
    bool hasRecordDuration = false;
    bool hasSentBytes = false;
    bool hasVideoFps = false;
    bool hasVideoBitrate = false;
    bool hasReconnectAttempt = false;
    bool hasDroppedFrames = false;
    int64_t streamDurationMs = 0;
    int64_t recordDurationMs = 0;
    int64_t sentBytes = 0;
    double videoFps = 0;
    double videoBitrateKbps = 0;
    int32_t reconnectAttempt = 0;
    int64_t droppedVideoFrames = 0;

    // recordFinished 负载
    std::string fileName;
    std::string filePath;
    int64_t durationMs = 0;
    int64_t sizeBytes = 0;
    int64_t createdAtMs = 0;
    std::string reason;

    // error / micDegraded 负载
    int32_t errorCode = 0;
    std::string message;
    std::string source;
    std::string degradedTo;
};

class JsEventEmitter {
public:
    JsEventEmitter() = default;
    ~JsEventEmitter();

    // 由 NAPI 线程调用：创建 TSFN（env 须为 ArkTS env）
    bool Attach(napi_env env, napi_value callback);

    // 任意线程调用：事件入队并触发 TSFN 异步调用
    void Emit(EventData event);

    // 释放 TSFN（幂等）；之后 Emit 为空操作
    void Release();

    bool IsAttached() const;

private:
    // TSFN 静态桥接：native 线程触发 → JS 线程执行
    static void CallJs(napi_env env, napi_value jsCallback, void *context, void *data);

    // 在 JS 线程构建事件对象并调用回调
    void DispatchToJs(napi_env env, napi_value jsCallback, EventData *event);

    napi_env env_ = nullptr;
    napi_threadsafe_function tsfn_ = nullptr;
    mutable std::mutex mutex_;
    bool released_ = false;
};

} // namespace media_stream

#endif // MEDIA_STREAM_JS_EVENT_EMITTER_H
