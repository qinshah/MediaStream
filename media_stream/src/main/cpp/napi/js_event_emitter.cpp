#include "js_event_emitter.h"

#include "../common/logger.h"

namespace media_stream {

JsEventEmitter::~JsEventEmitter() {
    Release();
}

bool JsEventEmitter::Attach(napi_env env, napi_value callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tsfn_ != nullptr) {
        // 重复订阅：先释放旧 TSFN
        napi_release_threadsafe_function(tsfn_, napi_tsfn_release);
        tsfn_ = nullptr;
    }
    if (env == nullptr || callback == nullptr) {
        return false;
    }

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "media_stream_event", NAPI_AUTO_LENGTH, &resourceName);

    // max_queue_size=0 表示无界（事件量在 stats 节流下可控）；initial_thread_count=1
    napi_status status = napi_create_threadsafe_function(
        env, callback, nullptr, resourceName, 0, 1, nullptr, nullptr, this, &JsEventEmitter::CallJs, &tsfn_);
    if (status != napi_ok) {
        MS_LOG_ERROR("napi_create_threadsafe_function failed: %{public}d", static_cast<int>(status));
        tsfn_ = nullptr;
        return false;
    }
    env_ = env;
    released_ = false;
    return true;
}

void JsEventEmitter::Emit(EventData event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (released_ || tsfn_ == nullptr) {
        return;
    }
    // 堆分配事件负载，由 CallJs 在 JS 线程消费后释放
    auto *heapEvent = new EventData(std::move(event));
    napi_status status = napi_call_threadsafe_function(tsfn_, heapEvent, napi_tsfn_nonblocking);
    if (status != napi_ok) {
        delete heapEvent;
        MS_LOG_WARN("napi_call_threadsafe_function failed: %{public}d", static_cast<int>(status));
    }
}

void JsEventEmitter::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tsfn_ != nullptr) {
        // blocking 模式确保已入队事件被消费完
        napi_release_threadsafe_function(tsfn_, napi_tsfn_release);
        tsfn_ = nullptr;
    }
    released_ = true;
    env_ = nullptr;
}

bool JsEventEmitter::IsAttached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tsfn_ != nullptr && !released_;
}

void JsEventEmitter::CallJs(napi_env env, napi_value jsCallback, void *context, void *data) {
    auto *emitter = static_cast<JsEventEmitter *>(context);
    auto *event = static_cast<EventData *>(data);
    if (event == nullptr) {
        return;
    }
    if (emitter != nullptr && env != nullptr && jsCallback != nullptr) {
        emitter->DispatchToJs(env, jsCallback, event);
    }
    delete event;
}

// —— JS 对象构建辅助 ——
static void SetStringProp(napi_env env, napi_value obj, const char *name, const std::string &value) {
    napi_value v = nullptr;
    napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &v);
    napi_set_named_property(env, obj, name, v);
}

static void SetInt64Prop(napi_env env, napi_value obj, const char *name, int64_t value) {
    napi_value v = nullptr;
    napi_create_int64(env, value, &v);
    napi_set_named_property(env, obj, name, v);
}

static void SetDoubleProp(napi_env env, napi_value obj, const char *name, double value) {
    napi_value v = nullptr;
    napi_create_double(env, value, &v);
    napi_set_named_property(env, obj, name, v);
}

static void SetInt32Prop(napi_env env, napi_value obj, const char *name, int32_t value) {
    napi_value v = nullptr;
    napi_create_int32(env, value, &v);
    napi_set_named_property(env, obj, name, v);
}

void JsEventEmitter::DispatchToJs(napi_env env, napi_value jsCallback, EventData *event) {
    napi_handle_scope scope = nullptr;
    napi_open_handle_scope(env, &scope);
    if (scope == nullptr) {
        return;
    }

    napi_value eventObj = nullptr;
    napi_create_object(env, &eventObj);

    switch (event->type) {
        case EventType::kCaptureState:
            SetStringProp(env, eventObj, "type", "captureState");
            SetStringProp(env, eventObj, "state", event->state);
            break;
        case EventType::kStreamState:
            SetStringProp(env, eventObj, "type", "streamState");
            SetStringProp(env, eventObj, "state", event->state);
            break;
        case EventType::kRecordState:
            SetStringProp(env, eventObj, "type", "recordState");
            SetStringProp(env, eventObj, "state", event->state);
            break;
        case EventType::kStats: {
            SetStringProp(env, eventObj, "type", "stats");
            napi_value statsObj = nullptr;
            napi_create_object(env, &statsObj);
            if (event->hasStreamDuration) {
                SetInt64Prop(env, statsObj, "streamDurationMs", event->streamDurationMs);
            }
            if (event->hasRecordDuration) {
                SetInt64Prop(env, statsObj, "recordDurationMs", event->recordDurationMs);
            }
            if (event->hasSentBytes) {
                SetInt64Prop(env, statsObj, "sentBytes", event->sentBytes);
            }
            if (event->hasVideoFps) {
                SetDoubleProp(env, statsObj, "videoFps", event->videoFps);
            }
            if (event->hasVideoBitrate) {
                SetDoubleProp(env, statsObj, "videoBitrateKbps", event->videoBitrateKbps);
            }
            if (event->hasReconnectAttempt) {
                SetInt32Prop(env, statsObj, "reconnectAttempt", event->reconnectAttempt);
            }
            if (event->hasDroppedFrames) {
                SetInt64Prop(env, statsObj, "droppedVideoFrames", event->droppedVideoFrames);
            }
            napi_set_named_property(env, eventObj, "stats", statsObj);
            break;
        }
        case EventType::kRecordFinished: {
            SetStringProp(env, eventObj, "type", "recordFinished");
            napi_value metaObj = nullptr;
            napi_create_object(env, &metaObj);
            SetStringProp(env, metaObj, "fileName", event->fileName);
            SetStringProp(env, metaObj, "filePath", event->filePath);
            SetInt64Prop(env, metaObj, "durationMs", event->durationMs);
            SetInt64Prop(env, metaObj, "sizeBytes", event->sizeBytes);
            SetInt64Prop(env, metaObj, "createdAtMs", event->createdAtMs);
            napi_set_named_property(env, eventObj, "meta", metaObj);
            SetStringProp(env, eventObj, "reason", event->reason);
            break;
        }
        case EventType::kError:
            SetStringProp(env, eventObj, "type", "error");
            SetInt32Prop(env, eventObj, "code", event->errorCode);
            SetStringProp(env, eventObj, "message", event->message);
            SetStringProp(env, eventObj, "source", event->source);
            break;
        case EventType::kMicDegraded:
            SetStringProp(env, eventObj, "type", "micDegraded");
            SetStringProp(env, eventObj, "degradedTo", event->degradedTo);
            SetStringProp(env, eventObj, "message", event->message);
            break;
    }

    // 调用 ArkTS 回调
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    napi_value result = nullptr;
    napi_call_function(env, undefined, jsCallback, 1, &eventObj, &result);

    napi_close_handle_scope(env, scope);
}

} // namespace media_stream
