#include "amf.h"

#include <cstring>

namespace media_stream {

// AMF0 类型标记
static constexpr uint8_t kAmfNumber = 0x00;
static constexpr uint8_t kAmfBool = 0x01;
static constexpr uint8_t kAmfString = 0x02;
static constexpr uint8_t kAmfObject = 0x03;
static constexpr uint8_t kAmfNull = 0x05;

// —— AmfWriter ——

void AmfWriter::WriteU16(uint16_t v) {
    data_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    data_.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AmfWriter::WriteU32(uint32_t v) {
    data_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    data_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    data_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    data_.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AmfWriter::WriteDouble(double v) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "double must be 8 bytes");
    memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; i--) {
        data_.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
    }
}

void AmfWriter::WriteNumber(double value) {
    data_.push_back(kAmfNumber);
    WriteDouble(value);
}

void AmfWriter::WriteString(const std::string &value) {
    data_.push_back(kAmfString);
    WriteU16(static_cast<uint16_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
}

void AmfWriter::WriteBool(bool value) {
    data_.push_back(kAmfBool);
    data_.push_back(value ? 1 : 0);
}

void AmfWriter::WriteNull() {
    data_.push_back(kAmfNull);
}

void AmfWriter::WriteObjectBegin() {
    data_.push_back(kAmfObject);
}

void AmfWriter::Append(const std::vector<uint8_t> &bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
}

void AmfWriter::WriteObjectEnd() {
    WriteU16(0);
    data_.push_back(0x09);
}

void AmfWriter::WriteObjectProperty(const std::string &key, const std::string &value) {
    // 对象属性无类型前缀：key 为裸 UTF-8（u16 长度），value 带类型标记
    WriteU16(static_cast<uint16_t>(key.size()));
    data_.insert(data_.end(), key.begin(), key.end());
    WriteString(value);
}

void AmfWriter::WriteObjectPropertyNumber(const std::string &key, double value) {
    WriteU16(static_cast<uint16_t>(key.size()));
    data_.insert(data_.end(), key.begin(), key.end());
    WriteNumber(value);
}

// —— AmfReader ——

uint16_t AmfReader::ReadU16() {
    if (pos_ + 2 > size_) {
        return 0;
    }
    uint16_t v = (static_cast<uint16_t>(data_[pos_]) << 8) | data_[pos_ + 1];
    pos_ += 2;
    return v;
}

uint32_t AmfReader::ReadU32() {
    if (pos_ + 4 > size_) {
        return 0;
    }
    uint32_t v = (static_cast<uint32_t>(data_[pos_]) << 24) |
                 (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
                 (static_cast<uint32_t>(data_[pos_ + 2]) << 8) | data_[pos_ + 3];
    pos_ += 4;
    return v;
}

double AmfReader::ReadDouble() {
    if (pos_ + 8 > size_) {
        return 0;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) {
        bits = (bits << 8) | data_[pos_ + i];
    }
    pos_ += 8;
    double v = 0;
    memcpy(&v, &bits, 8);
    return v;
}

int AmfReader::ReadValue(std::string &outStr, double &outNum) {
    if (pos_ >= size_) {
        return -1;
    }
    uint8_t type = data_[pos_++];
    switch (type) {
        case kAmfNumber:
            outNum = ReadDouble();
            return type;
        case kAmfBool:
            outNum = pos_ < size_ ? data_[pos_++] : 0;
            return type;
        case kAmfString: {
            uint16_t len = ReadU16();
            if (pos_ + len > size_) {
                return -1;
            }
            outStr.assign(reinterpret_cast<const char *>(data_ + pos_), len);
            pos_ += len;
            return type;
        }
        case kAmfNull:
            return type;
        case kAmfObject: {
            // 扁平读取对象属性直至 object end；简单语义：level/description 等存入 outStr（最后一个字符串值）
            while (pos_ + 3 <= size_) {
                uint16_t keyLen = ReadU16();
                if (keyLen == 0 && pos_ < size_ && data_[pos_] == 0x09) {
                    pos_++;
                    break;
                }
                if (pos_ + keyLen > size_) {
                    return -1;
                }
                pos_ += keyLen; // 跳过 key
                std::string v;
                double n = 0;
                int t = ReadValue(v, n);
                if (t < 0) {
                    return -1;
                }
                if (t == kAmfString) {
                    // 累积全部字符串值：服务端 info 对象按 {level, code, description} 排列，
                    // 只保留最后一个会让 description 覆盖 code，导致 NetStream.Publish.Start 匹配不到
                    if (!outStr.empty()) {
                        outStr += ';';
                    }
                    outStr += v;
                }
            }
            return type;
        }
        default:
            return type; // 未知类型由调用方决定是否跳过
    }
}

bool AmfReader::SkipValue() {
    std::string s;
    double n = 0;
    return ReadValue(s, n) >= 0;
}

} // namespace media_stream
