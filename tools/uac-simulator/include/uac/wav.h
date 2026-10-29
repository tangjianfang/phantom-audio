#pragma once
// 极简 16bit PCM WAV 写入器（头部占位，close 时回填大小）
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace uac {

class WavWriter {
public:
    bool open(const std::string& path, std::uint32_t rate, std::uint16_t channels,
              std::uint16_t bitsPerSample = 16);
    void write(const std::int16_t* samples, std::size_t count); // 交织声道
    void close(); // 回填 RIFF/data 长度
    bool is_open() const { return f_.is_open(); }
    std::uint64_t sample_count() const { return samples_; }

private:
    std::ofstream f_;
    std::uint32_t rate_ = 48000;
    std::uint16_t ch_ = 2, bits_ = 16;
    std::uint64_t samples_ = 0;
};

} // namespace uac
