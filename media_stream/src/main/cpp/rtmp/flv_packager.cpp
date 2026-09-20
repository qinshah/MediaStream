#include "flv_packager.h"

#include "amf.h"

namespace media_stream {

// FLV 视频 tag 首字节：FrameType(4bit) | CodecID(4bit)；AVC CodecID=7
static constexpr uint8_t kFlvVideoKeyframeAvc = 0x17; // keyframe + AVC
static constexpr uint8_t kFlvVideoInterAvc = 0x27;    // inter frame + AVC
// FLV 音频 tag 首字节：SoundFormat(4bit)=10(AAC) | 44kHz(2bit)=3 | 16bit(1bit)=1 | stereo(1bit)=1
static constexpr uint8_t kFlvAudioAac = 0xAF;

std::vector<uint8_t> FlvPackager::BuildVideoSequenceHeader(const std::vector<uint8_t> &avcC) {
    std::vector<uint8_t> out;
    out.reserve(avcC.size() + 5);
    out.push_back(kFlvVideoKeyframeAvc);
    out.push_back(0x00); // AVCPacketType = sequence header
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00); // composition time = 0
    out.insert(out.end(), avcC.begin(), avcC.end());
    return out;
}

std::vector<uint8_t> FlvPackager::BuildVideoTag(const uint8_t *data, int32_t size, bool isKeyframe) {
    std::vector<uint8_t> out;
    out.reserve(size + 5);
    out.push_back(isKeyframe ? kFlvVideoKeyframeAvc : kFlvVideoInterAvc);
    out.push_back(0x01); // AVCPacketType = NALU
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00); // composition time = 0（无 B 帧）
    out.insert(out.end(), data, data + size);
    return out;
}

std::vector<uint8_t> FlvPackager::BuildAudioSequenceHeader(const std::vector<uint8_t> &asc) {
    std::vector<uint8_t> out;
    out.reserve(asc.size() + 2);
    out.push_back(kFlvAudioAac);
    out.push_back(0x00); // AACPacketType = sequence header
    out.insert(out.end(), asc.begin(), asc.end());
    return out;
}

std::vector<uint8_t> FlvPackager::BuildAudioTag(const uint8_t *data, int32_t size) {
    std::vector<uint8_t> out;
    out.reserve(size + 2);
    out.push_back(kFlvAudioAac);
    out.push_back(0x01); // AACPacketType = raw
    out.insert(out.end(), data, data + size);
    return out;
}

std::vector<uint8_t> FlvPackager::BuildMetaData(int width, int height, int fps, int videoBitrateKbps) {
    AmfWriter w;
    w.WriteString("@setDataFrame");
    w.WriteString("onMetaData");
    // ECMA array（type 0x08）：数组长度(4) + 属性列表 + object end
    // AmfWriter 未提供数组起始标记，此处直接按字节拼装
    std::vector<uint8_t> out;
    const std::vector<uint8_t> &head = w.Data();
    out.insert(out.end(), head.begin(), head.end());
    out.push_back(0x08); // ECMA array
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x07); // 7 个属性
    AmfWriter props;
    props.WriteObjectPropertyNumber("width", width);
    props.WriteObjectPropertyNumber("height", height);
    props.WriteObjectPropertyNumber("framerate", fps);
    props.WriteObjectPropertyNumber("videodatarate", videoBitrateKbps);
    props.WriteObjectPropertyNumber("audiodatarate", 128);
    props.WriteObjectPropertyNumber("audiosamplerate", 48000);
    props.WriteObjectPropertyNumber("audiochannels", 2);
    props.WriteObjectEnd();
    const std::vector<uint8_t> &tail = props.Data();
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

} // namespace media_stream
