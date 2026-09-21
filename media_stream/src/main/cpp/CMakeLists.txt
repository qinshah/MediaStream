# the minimum version of CMake.
cmake_minimum_required(VERSION 3.5.0)
project(media_stream)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${NATIVERENDER_ROOT_PATH}
                    ${NATIVERENDER_ROOT_PATH}/include)

# 原生实现按域组织：napi 桥接 / core 控制面 / capture 采集 / codec 编码 / audio 混音 / muxer 封装 / rtmp 推流 / common 公共设施
add_library(media_stream SHARED
    napi/napi_init.cpp
    napi/js_event_emitter.cpp
    core/media_stream_engine.cpp
    capture/screen_capture.cpp
    capture/color_converter.cpp
    codec/video_encoder.cpp
    codec/audio_encoder.cpp
    audio/audio_mixer.cpp
    muxer/mp4_recorder.cpp
    rtmp/rtmp_client.cpp
    rtmp/amf.cpp
    rtmp/flv_packager.cpp
)

# Debug 构建强制开优化。
# 起因（真机实测）：默认 Debug 配置为 `-O0 -g`，而本项目最重的热路径是每帧必跑的
# capture/screen_capture.cpp 的 RGBA 行距紧凑化 + core/media_stream_engine.cpp 的
# RgbaToNv12Scaled（720x1632 约 117 万像素，内层是查表 + 整数矩阵运算）。-O0 下该
# 路径实测约 90~100ms/帧，直接把有效帧率锁死在 ~10fps：配置 60fps 却只出 4~5 帧/秒，
# 画面表现为「几乎不动 / 卡在某一帧」。同为 Debug 的联调构建必须开优化才有参考价值。
# 保留 -g（调试信息不丢），只把优化级别抬到 -O2。
target_compile_options(media_stream PRIVATE $<$<CONFIG:Debug>:-O2>)

# 链接 OHOS 系统能力库（全部来自 SDK sysroot，零三方依赖）
target_link_libraries(media_stream PUBLIC
    libace_napi.z.so
    libnative_avscreen_capture.so
    libnative_buffer.so
    libnative_media_venc.so
    libnative_media_aenc.so
    libnative_media_codecbase.so
    libnative_media_acodec.so
    libnative_media_avmuxer.so
    libnative_media_core.so
    libnative_display_manager.so
    libhilog_ndk.z.so
)
