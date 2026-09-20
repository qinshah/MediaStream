#include "color_converter.h"

namespace media_stream {

// BT.601 limited range：Y = 16 + (65.738R + 129.057G + 25.064B)/256
// U = 128 + (-37.945R - 74.494G + 112.439B)/256
// V = 128 + (112.439R - 94.154G - 18.285B)/256
// 定点系数放大 256 倍取整

static inline uint8_t ClampToU8(int32_t v) {
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}

void RgbaToNv12(const uint8_t *src, size_t srcStride, int width, int height,
                uint8_t *dstY, uint8_t *dstUV) {
    // Y 平面：逐像素
    for (int y = 0; y < height; y++) {
        const uint8_t *row = src + static_cast<size_t>(y) * srcStride;
        uint8_t *yRow = dstY + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; x++) {
            const uint8_t *p = row + static_cast<size_t>(x) * 4;
            int32_t r = p[0], g = p[1], b = p[2];
            yRow[x] = ClampToU8(((66 * r + 129 * g + 25 * b) >> 8) + 16);
        }
    }
    // UV 平面：2x2 子采样（取每 2x2 块左上角像素，避免额外平均开销）
    for (int y = 0; y < height / 2; y++) {
        const uint8_t *row = src + static_cast<size_t>(y * 2) * srcStride;
        uint8_t *uvRow = dstUV + static_cast<size_t>(y) * width;
        for (int x = 0; x < width / 2; x++) {
            const uint8_t *p = row + static_cast<size_t>(x * 2) * 4;
            int32_t r = p[0], g = p[1], b = p[2];
            uvRow[x * 2] = ClampToU8(((-38 * r - 74 * g + 112 * b) >> 8) + 128);     // U
            uvRow[x * 2 + 1] = ClampToU8(((112 * r - 94 * g - 18 * b) >> 8) + 128);  // V
        }
    }
}

} // namespace media_stream
