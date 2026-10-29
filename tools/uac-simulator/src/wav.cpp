// WavWriter 实现
#include <uac/wav.h>
#include <cstring>

namespace uac {

namespace {
void wr32(std::ofstream& f, std::uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    f.write(b, 4);
}
void wr16(std::ofstream& f, std::uint16_t v) {
    char b[2];
    std::memcpy(b, &v, 2);
    f.write(b, 2);
}
} // namespace

bool WavWriter::open(const std::string& path, std::uint32_t rate, std::uint16_t channels,
                     std::uint16_t bitsPerSample) {
    f_.open(path, std::ios::binary | std::ios::trunc);
    if (!f_.is_open()) return false;
    rate_ = rate;
    ch_ = channels;
    bits_ = bitsPerSample;
    samples_ = 0;
    f_.write("RIFF", 4);
    wr32(f_, 0); // 占位
    f_.write("WAVE", 4);
    f_.write("fmt ", 4);
    wr32(f_, 16);
    wr16(f_, 1);  // PCM
    wr16(f_, ch_);
    wr32(f_, rate_);
    wr32(f_, rate_ * ch_ * bits_ / 8);
    wr16(f_, static_cast<std::uint16_t>(ch_ * bits_ / 8));
    wr16(f_, bits_);
    f_.write("data", 4);
    wr32(f_, 0); // 占位
    return true;
}

void WavWriter::write(const std::int16_t* samples, std::size_t count) {
    if (!f_.is_open()) return;
    f_.write(reinterpret_cast<const char*>(samples),
             static_cast<std::streamsize>(count * sizeof(std::int16_t)));
    samples_ += count;
}

void WavWriter::close() {
    if (!f_.is_open()) return;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples_ * 2);
    f_.seekp(4);
    wr32(f_, 36 + dataBytes);
    f_.seekp(40);
    wr32(f_, dataBytes);
    f_.flush();
    f_.close();
}

} // namespace uac
