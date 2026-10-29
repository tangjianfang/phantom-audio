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
    // 每包样本数 = rate/1000（packetIntervalUs=1ms）× channels；±slotDrift
    const std::uint32_t perMs = cfg_.rate / 1000; // 48k → 48 样本/ms（全声道一组 slot）
    std::vector<unsigned char> out;
    isoOut.clear();
    isoOut.reserve(nPackets);
    std::uint32_t offset = 0;
    for (std::uint16_t p = 0; p < nPackets; ++p) {
        std::int64_t slots = perMs;
        if (p & 1) slots += cfg_.slotDrift; // 奇数包注入漂移（|drift|<=1 合规）
        if (slots < 0) slots = 0;
        const std::size_t nSamples = static_cast<std::size_t>(slots) * cfg_.channels;
        const std::size_t bytes = nSamples * 2;
        for (std::size_t s = 0; s < nSamples; ++s) {
            double v = 0.0;
            if (src_ == Src::Sine) {
                v = std::sin(phase_) * sineAmp_;
                phase_ += 2.0 * 3.14159265358979323846 * sineHz_ / cfg_.rate;
                if (phase_ > 2.0 * 3.14159265358979323846)
                    phase_ -= 2.0 * 3.14159265358979323846;
            } else if (src_ == Src::File) {
                if (filePos_ < fileBuf_.size()) v = fileBuf_[filePos_++] / 32768.0;
                else v = 0.0;
            }
            const auto q = static_cast<std::int32_t>(std::lround(v * 2147483647.0)) >>
                           (8 * (4 - cfg_.bytesPerSample)); // 低 (bytesPerSample) 字节 LE
            for (std::uint8_t b = 0; b < cfg_.bytesPerSample; ++b)
                out.push_back(static_cast<unsigned char>((q >> (8 * b)) & 0xff));
        }
        usbip::IsoPacket ip;
        ip.offset = offset;
        ip.length = static_cast<std::uint32_t>(bytes);
        ip.actual_length = static_cast<std::uint32_t>(bytes);
        ip.status = 0;
        isoOut.push_back(ip);
        offset += static_cast<std::uint32_t>(bytes);
    }
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
    if (!iso.empty()) {
        for (auto&& p : iso) {
            if (p.offset + p.actual_length > payload.size()) break;
            const auto* s = reinterpret_cast<const std::int16_t*>(payload.data() + p.offset);
            capture_.write(s, p.actual_length / 2);
            stats_.outBytes += p.actual_length;
        }
    } else {
        const auto* s = reinterpret_cast<const std::int16_t*>(payload.data());
        capture_.write(s, payload.size() / 2);
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

std::uint32_t AsEngine::fb_value_12_13(std::uint32_t rateHz, int ppmDrift) {
    // Q12.13：每微帧采样数 × 2^13；ppmDrift 注入频偏
    const double perUframe = rateHz * (1.0 + ppmDrift * 1e-6) / 8000.0;
    return static_cast<std::uint32_t>(perUframe * 8192.0 + 0.5);
}

} // namespace uac
