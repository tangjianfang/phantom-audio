// AsEngine 实现：pacing 队列 + 打包器 + 源生成 + 录音
#include <uac/as_engine.h>
#include <algorithm>

namespace uac {

AsEngine::AsEngine(MicroframeClock& clock, LogStore& log, StreamConfig cfg)
    : clock_(clock), log_(log), cfg_(cfg) {}

AsEngine::~AsEngine() {
    set_auto_pump(false);
    if (capture_.is_open()) capture_.close();
}

void AsEngine::set_auto_pump(bool on) {
    autoPump_.store(on);
    if (on && !pumpThreadStarted_) {
        pumpThreadStarted_ = true;
        pumpThread_ = std::thread([this] { // uac-cov:thread
            while (autoPump_.load()) {
                pump();
                std::mutex dummyMtx; // 仅用于 cv 超时等待载体
                std::unique_lock<std::mutex> lk(dummyMtx);
                cv_.wait_for(lk, std::chrono::microseconds(500), // uac-cov:thread
                             [this] { return !autoPump_.load(); });
            }
        });
    } else if (!on && pumpThreadStarted_) {
        cv_.notify_all();
        if (pumpThread_.joinable()) pumpThread_.join();
        pumpThreadStarted_ = false;
    }
}

void AsEngine::set_rate(std::uint32_t rateHz) {
    std::lock_guard lk(mtx_);
    cfg_.rate = rateHz;
    phase_ = 0.0;
    // A mid-capture rate change must be reflected in the WAV header, or the
    // file plays at the wrong pitch with no error anywhere.
    if (capture_.is_open()) capture_.update_rate(rateHz);
}

void AsEngine::set_stream_config(StreamConfig cfg) {
    std::lock_guard lk(mtx_);
    cfg_ = cfg;
}

void AsEngine::set_source_sine(double hz, double amp) {
    std::lock_guard lk(mtx_);
    src_ = Src::Sine;
    sineHz_ = hz;
    sineAmp_ = amp;
    phase_ = 0.0;
}
void AsEngine::set_source_file(const std::string& wavPath) {
    // 读取 16bit PCM WAV 数据段到内存（简单解析：找 "data" 块）
    std::lock_guard lk(mtx_);
    std::ifstream f(wavPath, std::ios::binary);
    fileBuf_.clear();
    filePos_ = 0;
    if (!f) {
        src_ = Src::Silence;
        return;
    }
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    for (std::size_t i = 12; i + 8 <= raw.size();) {
        const std::uint32_t sz = raw[i + 4] | (raw[i + 5] << 8) | (raw[i + 6] << 16) |
                                 (static_cast<std::uint32_t>(raw[i + 7]) << 24);
        if (raw[i] == 'd' && raw[i + 1] == 'a' && raw[i + 2] == 't' && raw[i + 3] == 'a') {
            const std::size_t n = std::min<std::size_t>(sz, raw.size() - i - 8);
            const auto* p = reinterpret_cast<const std::int16_t*>(raw.data() + i + 8);
            const std::size_t odd = n % 2;
            fileBuf_.assign(p, p + (n - odd) / 2);
            src_ = Src::File;
            return;
        }
        i += 8 + sz + (sz % 2);
    }
    src_ = Src::Silence;
}
void AsEngine::set_source_silence() {
    std::lock_guard lk(mtx_);
    src_ = Src::Silence;
}
void AsEngine::set_capture_sink(const std::string& wavPath) {
    std::lock_guard lk(mtx_);
    if (capture_.is_open()) capture_.close();
    capture_.open(wavPath, cfg_.rate, cfg_.channels);
}

AsEngine::Stats AsEngine::stats() const {
    std::lock_guard lk(mtx_);
    return stats_;
}

std::vector<unsigned char> AsEngine::fill_packets(std::uint16_t nPackets,
                                                  std::vector<usbip::IsoPacket>& isoOut) {
    // Fractional-sample pacing: 44100 Hz is 44.1 samples/ms; integer division
    // alone would stream at 44000 and drift against the host. A micro-
    // accumulator carries the remainder across packets.
    const std::uint64_t intervalUs = cfg_.packetIntervalUs;
    std::uint64_t carry = fracCarry_;
    const std::uint32_t bytesPerSample = std::max<std::uint32_t>(cfg_.bytesPerSample, 1);
    std::vector<unsigned char> out;
    isoOut.clear();
    isoOut.reserve(nPackets);
    std::uint32_t offset = 0;
    for (std::uint16_t p = 0; p < nPackets; ++p) {
        // exact samples = rate * intervalUs / 1e6 (+ carry), floor.
        // rate(<=768k) * intervalUs(<=1000) fits u64 with room to spare.
        const std::uint64_t num = static_cast<std::uint64_t>(cfg_.rate) * intervalUs + carry;
        std::uint64_t slots = num / 1000000;
        carry = num % 1000000;
        if (p & 1) slots += static_cast<std::uint64_t>(cfg_.slotDrift); // fault inj.
        const std::size_t nSamples = slots * cfg_.channels;
        const std::size_t bytes = nSamples * bytesPerSample;
        for (std::size_t sm = 0; sm < nSamples; ++sm) {
            double v = 0.0;
            if (src_ == Src::Sine) {
                v = std::sin(phase_) * sineAmp_;
                phase_ += 2.0 * 3.14159265358979323846 * sineHz_ / cfg_.rate;
                if (phase_ > 2.0 * 3.14159265358979323846)
                    phase_ -= 2.0 * 3.14159265358979323846;
            } else if (src_ == Src::File) {
                if (filePos_ < fileBuf_.size()) v = fileBuf_[filePos_++] / 32768.0;
            }
            const auto q = static_cast<std::int32_t>(std::lround(v * 2147483647.0)) >>
                           (8 * (4 - cfg_.bytesPerSample));
            for (std::uint8_t by = 0; by < cfg_.bytesPerSample; ++by)
                out.push_back(static_cast<unsigned char>((q >> (8 * by)) & 0xff));
        }
        usbip::IsoPacket ip;
        ip.offset = offset;
        ip.length = static_cast<std::uint32_t>(bytes);        // matches payload
        ip.actual_length = static_cast<std::uint32_t>(bytes);
        ip.status = 0;
        isoOut.push_back(ip);
        offset += static_cast<std::uint32_t>(bytes);
    }
    fracCarry_ = carry;
    return out;
}

void AsEngine::on_iso_in(const usbip::UrbCommand& cmd, CompletionFn complete) {
    std::lock_guard lk(mtx_);
    PendingIn pi;
    pi.seqnum = cmd.basic.seqnum;
    pi.nPackets = static_cast<std::uint16_t>(
        std::min<std::uint64_t>(cmd.submit.number_of_packets, 0xffff));
    pi.deadlineUs = clock_.now_us() +
                    static_cast<std::uint64_t>(pi.nPackets) * cfg_.packetIntervalUs;
    pi.complete = std::move(complete);
    ++stats_.inRequests;
    pending_.push_back(std::move(pi));
    std::sort(pending_.begin(), pending_.end(),
              [](auto&& a, auto&& b) { return a.deadlineUs < b.deadlineUs; });
}

void AsEngine::on_iso_out(std::span<const unsigned char> payload,
                          std::span<const usbip::IsoPacket> iso) {
    std::lock_guard lk(mtx_);
    ++stats_.outRequests;
    if (!capture_.is_open()) return;
    const std::size_t slot = std::max<std::size_t>(cfg_.bytesPerSample, 1);
    auto write_packet = [&](const unsigned char* data, std::size_t bytes) {
        // The sink is 16-bit PCM: for wider slots keep the low 16 bits of
        // each slot (little-endian), preserving sample count and channels.
        std::vector<std::int16_t> cvt;
        if (slot > 2) {
            cvt.reserve(bytes / slot);
            for (std::size_t off = 0; off + slot <= bytes; off += slot) {
                // S32/S24 slots are little-endian: the significant 16 bits sit
                // at the TOP of the slot (bytes slot-2 / slot-1), not the bottom.
                cvt.push_back(static_cast<std::int16_t>(data[off + slot - 2] |
                                                        (data[off + slot - 1] << 8)));
            }
            capture_.write(cvt.data(), cvt.size());
        } else {
            capture_.write(reinterpret_cast<const std::int16_t*>(data), bytes / 2);
        }
    };
    if (!iso.empty()) {
        for (auto&& p : iso) {
            if (p.offset + p.actual_length > payload.size()) break;
            write_packet(payload.data() + p.offset, p.actual_length);
            stats_.outBytes += p.actual_length;
        }
    } else {
        write_packet(payload.data(), payload.size());
        stats_.outBytes += payload.size();
    }
}

void AsEngine::pump() {
    std::vector<PendingIn> due;
    {
        std::lock_guard lk(mtx_);
        const std::uint64_t now = clock_.now_us();
        while (!pending_.empty() && pending_.front().deadlineUs <= now) {
            due.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
    }
    for (auto& p : due) {
        std::vector<usbip::IsoPacket> iso;
        std::vector<unsigned char> data;
        {
            std::lock_guard lk(mtx_);
            data = fill_packets(p.nPackets, iso);
            stats_.inBytes += data.size();
        }
        Completion cm;
        cm.ret.status = 0;
        cm.ret.actual_length = static_cast<std::uint32_t>(data.size());
        cm.ret.number_of_packets = p.nPackets;
        cm.ret.start_frame = 0; // #200 纪元问题：先归零，M0 实测后调整
        cm.ret.error_count = 0;
        cm.payload = std::move(data);
        cm.iso = std::move(iso);
        p.complete(std::move(cm));
    }
}

bool AsEngine::cancel(std::uint32_t seqnum) {
    std::lock_guard lk(mtx_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
        if (it->seqnum == seqnum) {
            pending_.erase(it);
            return true;
        }
    return false;
}

std::uint32_t AsEngine::fb_value_16_16(std::uint32_t rateHz, int ppmDrift) {
    // High-Speed explicit feedback: Q16.16 samples per microframe.
    // (Q12.13 would read 8x low to the host and starve adaptive playback.)
    const double perUframe = rateHz * (1.0 + ppmDrift * 1e-6) / 8000.0;
    return static_cast<std::uint32_t>(perUframe * 65536.0 + 0.5);
}

} // namespace uac
