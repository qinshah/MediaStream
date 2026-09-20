# Implementation Plan: 手机录屏与 RTMP 直播推流（media_stream 模块化解耦）

**Input**: Feature specification from `spec/media-stream/spec.md`

## Summary

为 MediaStream 手机应用实现**屏幕采集 → 硬件编码 → 双输出分发**管线：本地 MP4 录制（OH_AVMuxer）与自定义 RTMP 推流（自研最小 RTMP 客户端）。全部原生能力收敛在 `media_stream` HAR 模块（NAPI 暴露 ArkTS API + TSFN 事件回调）；`entry` 仅含 ArkUI 界面（控制台 + 录制列表，MVVM + 状态管理 V2）、权限申请与长时任务管理。采集层借鉴 `Harmony-OBS-Studio/plugins/ohos-capture` 的 AVScreenCapture 原始流适配经验（NV12 优先 + RGBA 兜底、内录/麦克风双路音频、显示尺寸查询），RTMP 层参考 `Harmony-OBS-Studio/plugins/obs-outputs/librtmp` 的协议行为（simple handshake + AMF0 + FLV tag），但不引入任何 OBS/libobs/FFmpeg 运行时依赖。

## Technical Context

**Language/Version**: ArkTS（ArkUI，状态管理 V2）；Native C++17（OHOS NDK，BiSheng 编译器）  
**Primary Dependencies**（全部来自 OHOS SDK sysroot，零三方源码依赖）:
- `libace_napi.z.so`（NAPI + TSFN）
- `libnative_avscreen_capture.so` + `libnative_buffer.so`（录屏原始流）
- `libnative_media_venc.so`（OH_VideoEncoder，H.264 硬件编码）
- `libnative_media_acodec.so`（OH_AACEncoder，AAC 硬件编码）
- `libnative_media_avmuxer.so` + `libnative_media_core.so`（MP4 封装）
- `libnative_display_manager.so`（默认显示屏尺寸查询）
- `libhilog_ndk.z.so`（原生日志）
- ArkTS 侧：`@kit.BackgroundTasksKit`（长时任务）、`@ohos.data.preferences`（配置持久化）、`@kit.CoreFileKit`（文件管理）、权限 API（`abilityAccessCtrl`）

**State Management**: 状态管理 V2（greenfield 0-1 项目；@ComponentV2 / @ObservedV2 / @Local / @Trace）  
**Storage**: 应用沙箱 `context.filesDir/videos/*.mp4`（录制产物）+ `recordings.json`（录制索引 sidecar）+ `@ohos.data.preferences`（用户配置持久化）  
**Testing**: 构建验证（build_project debug）+ 模拟器/真机 UI 验证；推流功能验证依赖用户自备局域网 RTMP 服务器（SRS/nginx-rtmp）  
**Target Platform**: HarmonyOS 手机（phone），`targetSdkVersion 26.0.0`，`compatibleSdkVersion 6.1.0(23)`；ABI：arm64-v8a（真机）+ x86_64（模拟器）  
**Project Type**: HarmonyOS HAP 应用（entry）+ 静态共享包 HAR（media_stream，含 Native）  
**Performance Goals**: 1080p/30fps 硬件编码推流；端到端延迟（采集→编码→RTMP 发出）≤ 800ms；RTMP 发送队列不持续积压  
**Constraints**: 录屏授权由系统弹窗承载（无需 restricted 权限声明，用户拒绝即终止会话）；麦克风采集需 `ohos.permission.MICROPHONE` 运行时授权；后台/锁屏持续需长时任务 `MODE_AV_PLAYBACK_AND_RECORD + SUBMODE_SCREEN_RECORD_NORMAL_NOTIFICATION`（API 22+，项目兼容基线 23 满足）+ `ohos.permission.KEEP_BACKGROUND_RUNNING`；网络访问需 `ohos.permission.INTERNET`  
**Scale/Scope**: 2 个模块（entry + media_stream）；entry 约 11 个 ArkTS 文件（MVVM）；media_stream 原生约 15 个 C++ 文件；2 个页面（控制台、录制列表）；无服务端

## Project Structure

### Documentation (this feature)

```text
/Users/qshh/Desktop/Code/MediaStream/spec/media-stream/
├── spec.md              # 需求规格（已评审通过）
└── plan.md              # 本文件
```

### Source Code (repository root)

```text
/Users/qshh/Desktop/Code/MediaStream/
├── build-profile.json5                    # 已注册 entry + media_stream（无需改动；signingConfigs 为空属环境问题，不在特性范围内）
├── entry/
│   ├── oh-package.json5                   # 改：新增依赖 "media_stream": "file:../media_stream"；移除 libentry.so 依赖
│   ├── build-profile.json5                # 改：移除 externalNativeOptions（entry 不再自建 native）
│   ├── src/main/module.json5              # 改：requestPermissions: INTERNET/MICROPHONE/KEEP_BACKGROUND_RUNNING；ability 增加 backgroundModes: ["audioRecording"]
│   ├── src/main/cpp/                      # 删除：模板 napi_init.cpp/CMakeLists/types（native 收敛到 media_stream）
│   └── src/main/ets/
│       ├── entryability/EntryAbility.ets      # 改：loadContent pages/Index（保持模板结构）
│       ├── pages/Index.ets                    # Navigation 宿主页（NavPathStack 路由）
│       ├── views/ConsoleView.ets              # 控制台业务视图：配置区（RTMP 地址/画质预设/音频源）+ 操作区（开始推流/停止、开始录制/停止）+ 状态统计区
│       ├── views/RecordingsView.ets           # 录制列表视图：列表 + 播放弹层（ArkUI Video 组件）+ 删除确认 + 分享
│       ├── viewmodel/ConsoleViewModel.ets     # 会话状态机镜像、统计刷新、操作编排（权限检查→服务调用→状态推进）
│       ├── viewmodel/RecordingsViewModel.ets  # 列表数据加载/删除/分享/孤儿文件清理
│       ├── service/MediaStreamService.ets     # media_stream HAR 的进程级单例封装：init(filesDir)、事件分发、指令转发
│       ├── service/PermissionService.ets      # 麦克风运行时授权检查与申请（拒绝时降级决策）
│       ├── service/BackgroundTaskService.ets  # 长时任务申请/取消（MODE_AV_PLAYBACK_AND_RECORD + SUBMODE_SCREEN_RECORD_NORMAL_NOTIFICATION）
│       ├── data/PreferencesRepository.ets     # 用户配置持久化（rtmpUrl、preset、audioMode）
│       ├── data/RecordingsRepository.ets      # 录制索引 recordings.json 维护 + videos 目录扫描 + 孤儿/损坏文件检测
│       └── model/Types.ets                    # UI 域类型：画质预设常量表、音频源枚举、路由名、事件负载映射
└── media_stream/
    ├── oh-package.json5                   # 保留：name=media_stream，依赖 libmedia_stream.so 类型声明
    ├── build-profile.json5                # 保留：externalNativeOptions 指向 cpp/CMakeLists.txt，abiFilters [arm64-v8a, x86_64]
    ├── Index.ets                          # 改：导出 native 模块 API 与类型（移除模板 MainPage）
    ├── src/main/ets/components/MainPage.ets   # 删除：模板文件
    └── src/main/cpp/
        ├── CMakeLists.txt                 # 改：project(media_stream)；链接全部系统能力库；C++17
        ├── types/libmedia_stream/
        │   ├── Index.d.ts                 # TS API 契约（见 Contracts & Interfaces）
        │   └── oh-package.json5           # 保留（libmedia_stream.so 声明）
        ├── napi/napi_init.cpp                 # NAPI 模块注册、方法实现、参数解析、TSFN 创建
        ├── napi/js_event_emitter.h/.cpp       # TSFN 封装：线程安全事件投递到 ArkTS（合并投递/节流）
        ├── core/media_stream_engine.h/.cpp    # 控制面：会话状态机（capture/stream/record 三平面）、输出编排、配置校验、资源生命周期
        ├── capture/screen_capture.h/.cpp      # AVScreenCapture 封装：原始流配置（NV12 优先/RGBA 兜底）、内录+麦克风双路音频、显示尺寸查询、系统授权回调处理
        ├── capture/color_converter.h/.cpp     # RGBA→NV12 软件转换（兜底路径，标量实现）
        ├── codec/video_encoder.h/.cpp         # OH_VideoEncoder H.264 buffer 模式：参数配置、输入投递、输出回调（含 SPS/PPS 提取）
        ├── codec/audio_encoder.h/.cpp         # OH_AACEncoder AAC-LC 48k 双声道 buffer 模式（含 ASC 提取）
        ├── audio/audio_mixer.h/.cpp           # 内录+麦克风 s16 饱和混音（按采样数对齐，单输入直通）
        ├── muxer/mp4_recorder.h/.cpp          # OH_AVMuxer MP4 封装：写线程 + 采样队列（pts 单位 μs）、avcC/ASC codec config、安全收尾
        ├── rtmp/rtmp_client.h/.cpp            # RTMP 客户端：socket 非阻塞连接、simple handshake、connect/createStream/publish 命令、发送线程、指数退避重连状态机（≤3 次）
        ├── rtmp/amf.h/.cpp                    # AMF0 编解码（number/string/bool/object/null/eoh，仅命令所需子集）
        ├── rtmp/flv_packager.h/.cpp           # FLV tag 组包：H.264 AVCC（sequence header + NALU 分装）、AAC（ASC + 原始帧）、@setDataFrame onMetaData
        └── common/{logger.h, bounded_queue.h} # hilog 封装；有界阻塞队列模板（MP4 阻塞 / RTMP 丢视频帧两种策略）
```

**Structure Decision**: 本特性对 entry 属于 **MVVM 层级**：存在多页面（控制台/录制列表）、持久化（preferences + 录制索引）、跨页面业务服务（会话编排、权限、长时任务）与可测试域逻辑（状态机镜像），故采用 `pages/views/viewmodel/service/data/model` 责任边界；文件数控制在 11 个（模板要求 6-12），未按域概念过度拆分（播放弹层并入 RecordingsView，常量并入 model/Types.ets）。media_stream 为纯 API 型 HAR（无 UI），原生实现按"采集/编码/封装/协议/控制面/桥接"六域拆分为 15 个文件。工程为 greenfield（entry 现仅 Hello World 模板），故采用状态管理 V2；删除 entry 自带 cpp 脚手架，原生能力唯一收敛点为 media_stream（对应 FR-013 模块解耦红线）。

## Complexity Tracking

> 无 Constitution Check 违规需豁免。MVVM 层级由"多页面 + 持久化 + 业务服务编排"触发；文件数与结构均符合所选层级约束。

## Research & Decisions

### R1: 原生集成路径 —— 轻量自研管线（用户已确认）
- **Decision**: `media_stream` 内直连 OHOS NDK（AVScreenCapture → OH_VideoEncoder/OH_AACEncoder → OH_AVMuxer + 自研 RTMP），零三方源码依赖。
- **Rationale**: Harmony-OBS-Studio 树无预编译产物，全量集成需交叉编译 FFmpeg/x264/jansson/MbedTLS（构建链路极重、失败风险高）；Qt 前端面向鸿蒙 PC 与手机无关；手机端硬件编码器功耗/性能远优于 x264 软编；系统能力库（采集/编码/封装）均在 sysroot 就绪。
- **Alternatives considered**: ①全量 OBS 集成（被否：依赖编译链过重）；②轻量管线+libobs 风格抽象层（被否：v1 无第二实现，YAGNI，后续演进不受影响）。

### R2: RTMP 协议实现 —— 自研最小客户端（simple handshake），参考 OBS 树 librtmp 行为
- **Decision**: 自研 RTMP：TCP 非阻塞连接 → RTMP simple handshake（C0/C1/C2 随机数）→ AMF0 `connect`/`createStream`/`publish`（live）→ 接收 `_result`/`onStatus` → FLV tag 消息流发送；重连时缓存并重发 codec sequence header（AVCC/ASC）。
- **Rationale**: 零外部依赖（librtmp 的 digest handshake 需 mbedtls、SWF 校验需 zlib，均未交叉编译）；simple handshake 为 RTMP 规范标准路径，SRS/nginx-rtmp 等主流服务端默认兼容；OBS 树 `plugins/obs-outputs/librtmp`（rtmp.c/amf.c/handshake.c）作为行为参照可直接查阅；AMF0 所需子集小（connect/publish/onStatus 命令）。
- **Alternatives considered**: ①移植 librtmp + NO_CRYPTO（被否：LGPL 源码搬运 + 剥离 crypto/zlib 的适配成本高于自研，且简单握手指令行为与 simple handshake 等价）；②引入 FFmpeg libavformat RTMP（被否：需整体交叉编译 FFmpeg）。

### R3: 视频编码 —— OH_VideoEncoder 硬件 H.264，buffer 模式，NV12 直采优先
- **Decision**: 采集源优先配置 `OH_VIDEO_SOURCE_SURFACE_YUV`（NV12），编码器输入直通（零拷贝路径最近）；若 Init 拒绝 NV12 则回退 RGBA 采集 + 软件转 NV12（标量实现，兜底路径）。关键帧间隔 = 2s（GOP 60 @30fps）；码率控制 CBR 按预设。
- **Rationale**: 硬件编码器输入以 NV12 为通用基线（RGBA 大多不被硬件编码器接受）；NV12 采集由 AVScreenCapture 原生支持（ohos-capture 插件已验证该优先级策略）；手机端硬件编码是功耗与性能的正解（对应 SC-007）。
- **Alternatives considered**: ①x264 软编（被否：无预编译产物、手机功耗不可接受）；②固定 RGBA + 全量软件转换（被否：1080p30 常态多耗 CPU）。

### R4: 后台/锁屏持续 —— 长时任务（API 22+ 录屏子类型）
- **Decision**: 会话活跃期间申请 `backgroundTaskManager` 长时任务：`ContinuousTaskRequest` 主类型 `MODE_AV_PLAYBACK_AND_RECORD` + 子类型 `SUBMODE_SCREEN_RECORD_NORMAL_NOTIFICATION`（录屏场景官方子类型，通知栏常驻通知由系统生成）；全部输出停止后立即取消。声明 `ohos.permission.KEEP_BACKGROUND_RUNNING`（system_grant）+ ability `backgroundModes: ["audioRecording"]`。
- **Rationale**: 官方文档明确"录屏退后台"属长时任务场景；API 22+ 提供录屏专用子类型，项目兼容基线 6.1.0(23) 满足；系统在通知栏展示常驻通知天然满足"用户可感知"约束（对应 FR-012）。
- **Alternatives considered**: 经典 `startBackgroundRunning(AUDIO_RECORDING)`（被否：子类型语义不如录屏专用档精确，且新旧接口混用增加分支）。

### R5: 采集分辨率策略 —— 按屏幕宽高比缩放至预设短边、偶数对齐
- **Decision**: 会话启动时经 `OH_NativeDisplayManager` 查询默认显示屏物理分辨率，将预设（1080p/720p/480p）解释为**短边目标值**，按屏幕宽高比等比缩放长边，宽高对齐偶数（H.264 编码要求），作为 AVScreenCapture `videoWidth/videoHeight` 一次性配置；会话期间不随旋转重建（对应规格假设）。
- **Rationale**: 手机屏幕为竖屏非标准分辨率（如 1260×2720），预设若按 16:9 硬编码将引入黑边或拉伸；短边映射 + 等比缩放保证画面无损语义；AVScreenCapture 原始流由系统完成缩放，无额外 CPU 开销。
- **Alternatives considered**: ①预设硬编码固定分辨率（被否：黑边/拉伸）；②采集原生分辨率 + 编码前自缩放（被否：软件缩放开销大）。

### R6: 音频链路 —— 双路 PCM 饱和混音 → 单 AAC 轨
- **Decision**: 内录与麦克风为两路独立 PCM 回调（48kHz/双声道/s16le）；`AudioMixer` 按采样数对齐做 s16 饱和相加（单输入时直通），输出送 OH_AACEncoder（AAC-LC 48k stereo，码率 128kbps）形成**单 AAC 轨**，MP4 与 RTMP 共用。
- **Rationale**: MP4 与 FLV/RTMP 单音轨实现最简且兼容性最好（双音轨需 TS/FLV 特殊处理，RTMP 常规单轨）；混音为录屏直播的通用预期（OBS 同为混音后单轨）。
- **Alternatives considered**: 双 AAC 轨（被否：封装/推流兼容性差、UI 复杂度翻倍）。

### R7: 录制产物管理 —— 沙箱 MP4 + recordings.json 索引 + 孤儿检测
- **Decision**: MP4 写入 `{filesDir}/videos/screen_{yyyyMMdd_HHmmss}.mp4`（fd 经原生 open）；每条录制完成时原生上报（路径/时长/字节），由 entry 侧 `RecordingsRepository` 维护 `recordings.json` 索引；应用启动时扫描目录，存在但不在索引中的 mp4 判定为"中断残留"，列表标记为无效可清理。
- **Rationale**: 沙箱无需媒体库权限（用户已选）；时长/大小元数据由会话权威产出，避免解析 mp4 box 的额外实现；崩溃残留文件可被识别（对应 Edge Case "应用被杀进程"）。
- **Alternatives considered**: ①写入媒体库（被否：用户已选沙箱）；②解析 mp4 moov 获取时长（被否：增加解析器实现面）。

### R8: 时间戳与码流格式约定 —— 三层单位/格式显式映射
- **Decision**: 采集回调时间戳（ns）→ 编码器（μs）→ MP4 `WriteSampleBuffer`（μs）/ RTMP FLV tag（ms）显式换算；H.264 走 AVCC（4 字节长度前缀）路径：编码输出若为 Annex-B 则转 AVCC（SPS/PPS 提取 → avcC codec config 同时供 MP4 `OH_MD_KEY_CODEC_CONFIG` 与 RTMP sequence header）；AAC ASC 同理双供。首帧 pts 归零、单调递增。
- **Rationale**: OH_AVMuxer 要求 pts 单位 μs（官方样例明确），FLV/RTMP 为 ms；avcC 与 Annex-B 的双格式是 MP4/RTMP 复用同一编码流的核心接缝，需在架构层一次性约定避免两处实现漂移。
- **Alternatives considered**: 各输出层自行解析（被否：重复实现、SPS/PPS 提取逻辑两份易漂移）。

### R9: 背压与容错策略
- **Decision**: ①RTMP 发送侧有界队列（容量 ~300 帧），满时丢弃**新入队视频帧**（音频不丢，记入 droppedFrames 统计）；②MP4 写线程独立 + 有界阻塞队列（磁盘写快，常规不满，阻塞超时则报错停止录制）；③socket 发送带超时；④重连期间编码持续、RTMP 队列照常入队（恢复后追帧）。
- **Rationale**: 直播链路宁可瞬时限帧不可背压阻塞采集/编码（会拖垮整机管线）；本地封装正确性优先（不丢帧）。
- **Alternatives considered**: 全链路无界队列（被否：断网时内存膨胀直至 OOM）。

### R10: entry 原生脚手架清理
- **Decision**: 删除 `entry/src/main/cpp/`（模板 napi_init.cpp 等）及其 `externalNativeOptions`、`libentry.so` 依赖；`Index.ets` 重写为 Navigation 宿主。
- **Rationale**: 原生能力唯一收敛点为 media_stream（FR-013）；entry 保留无用 native 增加构建面与混淆点。
- **Alternatives considered**: 保留（被否：死代码、双 native 模块误导后续维护）。

### R11: 验证环境注意 —— 模拟器采集能力受限
- **Decision**: abiFilters 保留 x86_64 以支持模拟器构建/部署；AVScreenCapture 在模拟器上可能返回初始化失败或无帧回调，UI 与状态机仍可在模拟器验证（错误提示、按钮态机、配置持久化、列表交互）；采集/推流全链路验证以真机为准。
- **Rationale**: 规格已假定全链路验证需真机；模拟器覆盖 UI 交互与错误路径即可满足 UI 验证目标。

## Data Model

### 领域实体（ArkTS 侧，d.ts 契约的主体）

- **OutputConfig**（会话输出配置）
  - `rtmpUrl: string`（推流地址，`rtmp://` 前缀校验）
  - `preset: ResolutionPreset`（`'1080p' | '720p' | '480p'`，短边目标值）
  - `fps: number`（默认 30）
  - `videoBitrateKbps: number`（预设映射：1080p→8000、720p→4000、480p→2000）
  - `audioMode: AudioSourceMode`（`'mic' | 'inner' | 'micInner'`）
- **会话状态三平面**（引擎内部状态机，事件上报给 UI）
  - `captureState: 'idle' | 'starting' | 'active' | 'stopping' | 'error'`
  - `streamState: 'idle' | 'connecting' | 'streaming' | 'reconnecting' | 'stopped' | 'error'`
  - `recordState: 'idle' | 'recording' | 'stopped' | 'error'`
  - 不变式：任一输出活跃 ⇒ captureState = active；两输出均停止 ⇒ 采集随停止而释放（状态机驱动资源生命周期，对应 FR-014）
- **SessionStats**（节流 500ms 上报）：`streamDurationMs / recordDurationMs / sentBytes / videoFps / videoBitrateKbps / reconnectAttempt / droppedVideoFrames`（各字段按输出活跃与否可选）
- **RecordingMeta**：`fileName / filePath / durationMs / sizeBytes / createdAtMs`
- **RecordingIndexEntry**（entry 侧索引）：`RecordingMeta + id + valid: boolean`
- **MediaStreamError**：`code: ErrorCode + message: string`；`ErrorCode` 枚举：`INVALID_ARG / BUSY / CAPTURE_INIT_FAIL / CAPTURE_PERMISSION_DENIED / MIC_PERMISSION_DENIED / ENCODER_INIT_FAIL / ENCODER_ERROR / RTMP_URL_INVALID / RTMP_CONNECT_FAIL / RTMP_TIMEOUT / RTMP_AUTH_FAIL / STORAGE_FULL / INTERNAL_ERROR`
- **UserConfig**（preferences 持久化）：`rtmpUrl / preset / audioMode / micPermissionHintShown`

### 原生内部数据流（无持久化）

- `VideoSample`：NV12 平面 buffer（宽/高/stride/ptsNs/isKeyframe 归属编码后抽象为 `EncodedSample{data, size, ptsUs, isKeyframe, track}`）
- `AudioFrame`：交错 s16 PCM（采样数/ptsNs）→ `EncodedSample`（track=audio）
- `CodecConfigCache`：`avcC(bytes) / asc(bytes) / width / height / sampleRate / channels`——编码器输出回调首次解析后缓存，供 MP4 AddTrack、RTMP sequence header、重连重发三处共享
- `FanOutDispatcher`：编码输出 →（RTMP 队列， MP4 队列）双路分发；单输出活跃时另一路直通丢弃

## Contracts & Interfaces

### 1. media_stream 对外 ArkTS API（`types/libmedia_stream/Index.d.ts`，NAPI 模块名 `media_stream`）

```ts
// —— 全部方法由 libmedia_stream.so 原生实现，Index.ets re-export ——
init(filesDir: string): void                                  // 注入沙箱根目录（录制目录 = filesDir/videos）
startStreaming(config: OutputConfig): void                    // 校验 rtmpUrl；采集未运行则启动采集+编码；连接 RTMP
stopStreaming(): void                                         // 关闭 RTMP 输出；无其他输出时停止采集编码
startRecording(): void                                        // 复用当前会话配置（采集未运行则按最近一次 config 启动）；MP4 输出开始
stopRecording(): void                                         // MP4 安全收尾（moov 写全）；无其他输出时停止采集编码
onEvent(callback: (event: MediaStreamEvent) => void): void    // 唯一事件通道（TSFN 投递），见事件契约
destroy(): void                                               // 停止全部输出并释放原生资源（幂等）
```

约束：所有 start/stop 幂等防重入（FR-015）；`startStreaming` 时若 streamState 非 `idle/stopped/error` 直接忽略；配置对象含非法值时抛出带 `ErrorCode` 的异常。

### 2. 事件契约（`MediaStreamEvent` 判别联合）

```ts
{ type: 'captureState', state: CaptureState }
{ type: 'streamState',  state: StreamState }
{ type: 'recordState',  state: RecordState }
{ type: 'stats',        stats: SessionStats }
{ type: 'recordFinished', meta: RecordingMeta, reason: 'normal' | 'error' }
{ type: 'error',        code: ErrorCode, message: string, source: 'capture' | 'videoEncoder' | 'audioEncoder' | 'rtmp' | 'mp4' | 'storage' }
{ type: 'micDegraded',  degradedTo: AudioSourceMode, message: string }   // 麦克风不可用降级通知
```

### 3. entry ↔ media_stream 使用契约（entry 侧不触媒体细节）

- entry 在 EntryAbility/首页 `aboutToAppear` 调 `init(context.filesDir)`；进程内单例（`MediaStreamService` 持有）
- 麦克风授权由 **entry** 在 `startStreaming/startRecording` 前完成（`PermissionService`）；未授权且配置含 mic → 弹确认降级（继续=改 inner）或取消
- 长时任务由 **entry** 在首输出启动成功后申请、末输出停止后取消（`BackgroundTaskService`）
- 录制列表由 **entry** 经 `RecordingsRepository` 维护（media_stream 不暴露列表 API，仅 `recordFinished` 事件）

### 4. 权限与模块配置（`entry/src/main/module.json5`）

- `requestPermissions`：`ohos.permission.INTERNET`（system_grant）、`ohos.permission.MICROPHONE`（user_grant，运行时申请）、`ohos.permission.KEEP_BACKGROUND_RUNNING`（system_grant）
- EntryAbility `backgroundModes: ["audioRecording"]`
- 录屏本体：无需应用权限声明，`OH_AVScreenCapture_StartScreenCapture` 触发系统授权弹窗；拒绝 → `CAPTURE_PERMISSION_DENIED` 事件

### 5. 原生构建契约（`media_stream/src/main/cpp/CMakeLists.txt`）

- C++17；产物 `libmedia_stream.so`（保持现有模块名与 NAPI 注册名一致）
- 链接：`libace_napi.z.so libnative_avscreen_capture.so libnative_buffer.so libnative_media_venc.so libnative_media_acodec.so libnative_media_avmuxer.so libnative_media_core.so libnative_display_manager.so libhilog_ndk.z.so`
- abiFilters：`arm64-v8a, x86_64`（随模块 build-profile，无需改）

### 6. MP4 封装契约（依据官方 AVMuxer 指南验证）

- `OH_AVMuxer_Create(fd, AV_OUTPUT_FORMAT_MPEG_4)`；音视频轨 AddTrack 于 Start 前完成：视频轨 `OH_AVCODEC_MIMETYPE_VIDEO_AVC + width/height + OH_MD_KEY_CODEC_CONFIG(avcC)`；音频轨 `OH_AVCODEC_MIMETYPE_AUDIO_AAC + 48000/2 + OH_MD_KEY_CODEC_CONFIG(ASC)`
- `WriteSampleBuffer` pts 单位 **μs**；关键帧置 `AVCODEC_BUFFER_FLAGS_SYNC_FRAME`；Stop 写 moov 尾后 Destroy + close(fd)，全程写线程串行化

### 7. RTMP 输出契约

- URL 解析：`rtmp://host[:port]/app/streamName`（默认端口 1935）；非 rtmp 协议 → `RTMP_URL_INVALID`
- 命令序列：handshake(simple) → `connect(app)` → 等 `_result` → `createStream` → `publish(streamName,'live')` → 等 `onStatus(NetStream.Publish.Start)` → 进入推流态
- 消息流：`@setDataFrame onMetaData` → video sequence header（AVCC）→ audio sequence header（ASC）→ 按时间戳交织音视频 FLV tag；chunk size 协商 4096
- 断线：指数退避 1s/2s/4s 重连并重发 sequence header；3 次失败 → `streamState=error` + `RTMP_CONNECT_FAIL/RTMP_TIMEOUT`（采集若因录制仍活跃则不中断）

### 8. UI 导航契约（entry）

- `pages/Index` 为 Navigation 宿主：首页目标 = 控制台（ConsoleView）；`NavPathStack.pushPath('recordings')` 进入录制列表
- 控制台四个操作按钮态机：推流（开始/停止互斥切换）、录制（开始/停止互斥切换），会话进行中禁用预设/音频源切换（下次会话生效提示）；RTMP 地址输入实时校验前缀
- 录制列表：List 渲染倒序索引；播放 = Sheet 内 ArkUI `Video` 组件加载沙箱路径；删除 = AlertDialog 确认后删文件+索引；分享 = `startAbility(ACTION_SEND_DATA, uri=沙箱文件 URI, FLAG_GRANT_READ_URI_PERMISSION)`，失败降级 toast

### 9. 环境前置（非特性范围，验证阶段前提）

- 根 `build-profile.json5` 的 `signingConfigs` 为空：debug HAP 可正常构建；真机安装需本机 DevEco 自动签名配置（验证子代理按需处理，不修改产品配置）
- 全链路推流验证需真机 + 局域网标准 RTMP 服务器（SRS/nginx-rtmp，用户自备）
