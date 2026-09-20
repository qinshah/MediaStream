#ifndef MEDIA_STREAM_AMF_H
#define MEDIA_STREAM_AMF_H

// AMF0 编解码子集：仅实现 RTMP 命令所需类型
// 编码：number / string / bool / null / object(key-value) / eoh（object end）
// 解码：number / string / bool / null / object（扁平遍历），满足 _result/onStatus 解析

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace media_stream {

class AmfWriter {
public:
    void WriteNumber(double value);
    void WriteString(const std::string &value);   // 短字符串（len < 65536）
    void WriteBool(bool value);
    void WriteNull();
    void WriteObjectBegin();                  // 0x03 对象起始标记
    void WriteObjectEnd();                        // 00 00 09
    void WriteObjectProperty(const std::string &key, const std::string &value); // 对象属性（字符串值）
    void WriteObjectPropertyNumber(const std::string &key, double value);
    void Append(const std::vector<uint8_t> &bytes); // 追加原始字节

    const std::vector<uint8_t> &Data() const { return data_; }
    void Reset() { data_.clear(); }

private:
    void WriteU16(uint16_t v);
    void WriteU32(uint32_t v);
    void WriteDouble(double v);

    std::vector<uint8_t> data_;
};

class AmfReader {
public:
    AmfReader(const uint8_t *data, size_t size) : data_(data), size_(size), pos_(0) {}

    // 读取一个 AMF 值；返回类型标记（0x02 string / 0x00 number / 0x01 bool / 0x05 null / 0x03 object）
    // 字符串值写入 outStr，数值写入 outNum（按类型选用）
    int ReadValue(std::string &outStr, double &outNum);
    // 跳过任意类型值
    bool SkipValue();

    bool Eof() const { return pos_ >= size_; }

private:
    uint16_t ReadU16();
    uint32_t ReadU32();
    double ReadDouble();

    const uint8_t *data_;
    size_t size_;
    size_t pos_;
};

} // namespace media_stream

#endif // MEDIA_STREAM_AMF_H
