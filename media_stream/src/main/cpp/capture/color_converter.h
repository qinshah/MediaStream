#ifndef MEDIA_STREAM_COLOR_CONVERTER_H
#define MEDIA_STREAM_COLOR_CONVERTER_H

// RGBA → NV12 软件转换（兜底路径：AVScreenCapture 拒绝 NV12 采集时使用）
// 标量实现，处理行距；BT.601 limited range

#include <cstdint>
#include <cstddef>

namespace media_stream {

// src: RGBA 起始地址，srcStride: 行距（字节）
// dstY/dstUV: 输出平面（调用方保证容量 w*h / w*h/2）
void RgbaToNv12(const uint8_t *src, size_t srcStride, int width, int height,
                uint8_t *dstY, uint8_t *dstUV);

} // namespace media_stream

#endif // MEDIA_STREAM_COLOR_CONVERTER_H
