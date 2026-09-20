#ifndef MEDIA_STREAM_FLV_PACKAGER_H
#define MEDIA_STREAM_FLV_PACKAGER_H

// FLV tag 负载组包（RTMP message body，不含 FLV 文件头/PrevTagSize）
//  - 视频：AVCC sequence header（0x17 0x00 + avcC）/ NALU tag（0x17|0x27 + 0x01 + cts(0) + AVCC 数据）
//  - 音频：AAC sequence header（0xAF 0x00 + ASC）/ 原始帧 tag（0xAF 0x01 + AAC）
//  - 元数据：@setDataFrame onMetaData（ECMA array：宽高/帧率/码率/采样率）

#include <cstdint>
#include <vector>

namespace media_stream {

class FlvPackager {
public:
    // 视频 sequence header：AVCDecoderConfigurationRecord
    static std::vector<uint8_t> BuildVideoSequenceHeader(const std::vector<uint8_t> &avcC);

    // 视频帧 tag：data 为 AVCC 格式（4 字节长度前缀），isKeyframe 区分 FrameType
    static std::vector<uint8_t> BuildVideoTag(const uint8_t *data, int32_t size, bool isKeyframe);

    // 音频 sequence header：AudioSpecificConfig
    static std::vector<uint8_t> BuildAudioSequenceHeader(const std::vector<uint8_t> &asc);

    // 音频帧 tag：原始 AAC（无 ADTS）
    static std::vector<uint8_t> BuildAudioTag(const uint8_t *data, int32_t size);

    // @setDataFrame onMetaData
    static std::vector<uint8_t> BuildMetaData(int width, int height, int fps, int videoBitrateKbps);
};

} // namespace media_stream

#endif // MEDIA_STREAM_FLV_PACKAGER_H
