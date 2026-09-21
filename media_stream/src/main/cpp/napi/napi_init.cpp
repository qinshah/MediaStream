// media_stream NAPI 模块入口：注册 native addon，桥接 ArkTS 调用到 C++ 引擎
// 模块名 media_stream（产物 libmedia_stream.so），与 CMake add_library 名一致
#include <napi/native_api.h>
#include <string>

#include "../common/logger.h"
#include "../core/media_stream_engine.h"
#include "js_event_emitter.h"

using namespace media_stream;

namespace {

// 全局事件通道：TEFN 与引擎共享（引擎不持有所有权）
JsEventEmitter gEventEmitter;

MediaStreamEngine &Engine() {
    return MediaStreamEngine::Instance();
}

// NAPI 回调中抛错：丢出带 code 的 Error
void ThrowNapiError(napi_env env, int code, const char *msg) {
    napi_value err = nullptr;
    napi_create_error(env, nullptr, nullptr, &err);
    napi_value codeVal = nullptr;
    napi_create_int32(env, code, &codeVal);
    napi_set_named_property(env, err, "code", codeVal);
    napi_value msgVal = nullptr;
    napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &msgVal);
    napi_set_named_property(env, err, "message", msgVal);
    napi_throw(env, err);
}

std::string GetStringProp(napi_env env, napi_value obj, const char *name) {
    napi_value v = nullptr;
    if (napi_get_named_property(env, obj, name, &v) != napi_ok || v == nullptr) {
        return "";
    }
    size_t len = 0;
    napi_get_value_string_utf8(env, v, nullptr, 0, &len);
    if (len == 0) {
        return "";
    }
    std::string s(len, '\0');
    size_t written = 0;
    napi_get_value_string_utf8(env, v, &s[0], len + 1, &written);
    return s;
}

// 把 napi_value 直接转 string（用于 init 的 filesDir 参数）
std::string GetStringPropUnsafe(napi_env env, napi_value v) {
    if (v == nullptr) {
        return "";
    }
    napi_valuetype type;
    napi_typeof(env, v, &type);
    if (type != napi_string) {
        return "";
    }
    size_t len = 0;
    napi_get_value_string_utf8(env, v, nullptr, 0, &len);
    if (len == 0) {
        return "";
    }
    std::string s(len, '\0');
    size_t written = 0;
    napi_get_value_string_utf8(env, v, &s[0], len + 1, &written);
    return s;
}

double GetNumProp(napi_env env, napi_value obj, const char *name, double def) {
    napi_value v = nullptr;
    if (napi_get_named_property(env, obj, name, &v) != napi_ok || v == nullptr) {
        return def;
    }
    double d = def;
    napi_get_value_double(env, v, &d);
    return d;
}

// —— JsEventEmitter 回调：解析 MediaStreamEvent，交给 engine 上报路径（此处直接透传）——
// 实际上 engine 直接调用 gEventEmitter；此处仅为模块初始化时把 emitter 传给 engine
void EnsureEmitter() {
    Engine().SetEventEmitter(&gEventEmitter);
}

// init: (filesDir: string) => void
napi_value NapiInit(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string filesDir = argc >= 1 ? GetStringPropUnsafe(env, args[0]) : "";
    EnsureEmitter();
    Engine().Init(filesDir);
    return nullptr;
}

// startStreaming: (config: OutputConfig) => void
napi_value NapiStartStreaming(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        ThrowNapiError(env, 0, "缺少配置参数");
        return nullptr;
    }
    MediaStreamEngine::Config cfg;
    cfg.rtmpUrl = GetStringProp(env, args[0], "rtmpUrl");
    cfg.preset = GetStringProp(env, args[0], "preset");
    if (cfg.preset.empty()) {
        cfg.preset = "720p";
    }
    cfg.fps = static_cast<int>(GetNumProp(env, args[0], "fps", 30));
    cfg.videoBitrateKbps = static_cast<int>(GetNumProp(env, args[0], "videoBitrateKbps", 4000));
    cfg.audioMode = GetStringProp(env, args[0], "audioMode");
    if (cfg.audioMode.empty()) {
        cfg.audioMode = "inner";
    }

    int errCode = 0;
    std::string errMsg;
    EnsureEmitter();
    if (!Engine().StartStreaming(cfg, errCode, errMsg)) {
        ThrowNapiError(env, errCode, errMsg.c_str());
    }
    return nullptr;
}

// stopStreaming: () => void
napi_value NapiStopStreaming(napi_env env, napi_callback_info info) {
    Engine().StopStreaming();
    return nullptr;
}

// startRecording: (config?: OutputConfig) => void
// 录制需携带会话配置（尤其 audioMode）；缺省时回退为与默认一致的 "inner"
napi_value NapiStartRecording(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    MediaStreamEngine::Config cfg;
    if (argc >= 1 && args[0] != nullptr) {
        cfg.rtmpUrl = GetStringProp(env, args[0], "rtmpUrl");
        cfg.preset = GetStringProp(env, args[0], "preset");
        if (cfg.preset.empty()) {
            cfg.preset = "720p";
        }
        cfg.fps = static_cast<int>(GetNumProp(env, args[0], "fps", 30));
        cfg.videoBitrateKbps = static_cast<int>(GetNumProp(env, args[0], "videoBitrateKbps", 4000));
        cfg.audioMode = GetStringProp(env, args[0], "audioMode");
        if (cfg.audioMode.empty()) {
            cfg.audioMode = "inner";
        }
    }
    int errCode = 0;
    std::string errMsg;
    EnsureEmitter();
    if (!Engine().StartRecording(cfg, errCode, errMsg)) {
        ThrowNapiError(env, errCode, errMsg.c_str());
    }
    return nullptr;
}

// stopRecording: () => void
napi_value NapiStopRecording(napi_env env, napi_callback_info info) {
    Engine().StopRecording();
    return nullptr;
}

// onEvent: (callback: (event) => void) => void
napi_value NapiOnEvent(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        ThrowNapiError(env, 0, "缺少事件回调");
        return nullptr;
    }
    napi_valuetype type;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        ThrowNapiError(env, 0, "回调必须是函数");
        return nullptr;
    }
    EnsureEmitter();
    gEventEmitter.Attach(env, args[0]);
    return nullptr;
}

// destroy: () => void
napi_value NapiDestroy(napi_env env, napi_callback_info info) {
    Engine().Destroy();
    gEventEmitter.Release();
    return nullptr;
}

// 把 ErrorCode 枚举注册为模块导出对象（与 Index.d.ts 的 `export enum ErrorCode` 对齐）
// 关键:ArkTS `import { ErrorCode } from 'media_stream'` 解析的是名为 "ErrorCode" 的导出符号，
// 因此必须新建一个枚举对象赋给 exports.ErrorCode，而不是把成员散落成顶层导出（否则运行时
// 仍报 "does not provide an export name 'ErrorCode'"）。
void RegisterErrorCodeExport(napi_env env, napi_value exports) {
    napi_value ec = nullptr;
    napi_create_object(env, &ec);
    auto set = [&](const char *name, int32_t val) {
        napi_value v = nullptr;
        napi_create_int32(env, val, &v);
        napi_set_named_property(env, ec, name, v);
    };
    set("INVALID_ARG", static_cast<int32_t>(EngineError::kInvalidArg));
    set("BUSY", static_cast<int32_t>(EngineError::kBusy));
    set("CAPTURE_INIT_FAIL", static_cast<int32_t>(EngineError::kCaptureInitFail));
    set("CAPTURE_PERMISSION_DENIED", static_cast<int32_t>(EngineError::kCapturePermissionDenied));
    set("MIC_PERMISSION_DENIED", static_cast<int32_t>(EngineError::kMicPermissionDenied));
    set("ENCODER_INIT_FAIL", static_cast<int32_t>(EngineError::kEncoderInitFail));
    set("ENCODER_ERROR", static_cast<int32_t>(EngineError::kEncoderError));
    set("RTMP_URL_INVALID", static_cast<int32_t>(EngineError::kRtmpUrlInvalid));
    set("RTMP_CONNECT_FAIL", static_cast<int32_t>(EngineError::kRtmpConnectFail));
    set("RTMP_TIMEOUT", static_cast<int32_t>(EngineError::kRtmpTimeout));
    set("RTMP_AUTH_FAIL", static_cast<int32_t>(EngineError::kRtmpAuthFail));
    set("STORAGE_FULL", static_cast<int32_t>(EngineError::kStorageFull));
    set("INTERNAL_ERROR", static_cast<int32_t>(EngineError::kInternalError));
    napi_set_named_property(env, exports, "ErrorCode", ec);
}

napi_value InitModule(napi_env env, napi_value exports) {
    napi_property_descriptor methods[] = {
        {"init", nullptr, NapiInit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startStreaming", nullptr, NapiStartStreaming, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopStreaming", nullptr, NapiStopStreaming, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startRecording", nullptr, NapiStartRecording, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopRecording", nullptr, NapiStopRecording, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"onEvent", nullptr, NapiOnEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroy", nullptr, NapiDestroy, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods);
    // 额外导出 ErrorCode 枚举成员（无副作用：仅写入 exports 对象属性）
    RegisterErrorCodeExport(env, exports);
    return exports;
}

} // namespace

static napi_module mediaStreamModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = InitModule,
    .nm_modname = "media_stream",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterMediaStreamModule() {
    napi_module_register(&mediaStreamModule);
}