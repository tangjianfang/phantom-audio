#pragma once
// AudioStreaming 引擎：IN 打包（正弦/文件/静音）、OUT 解包录音、
// 虚拟微帧时钟 pacing（RET_SUBMIT 按包数 × 包间隔延迟，防流快进）。
#include <uac/log_store.h>
#include <uac/microframe_clock.h>
#include <uac/urb_dispatcher.h>
#include <uac/wav.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace uac {

struct StreamConfig {
    std::uint32_t rate = 48000;
    std::uint8_t channels = 2;
    std::uint8_t bytesPerSample = 2;
    std::uint32_t packetIntervalUs = 1000; // HS bInterval=4 → 8 微帧 = 1ms
    int slotDrift = 0;                     // 每包 ±slot 注入（<=1 合规）
};

class AsEngine {
public:
    AsEngine(MicroframeClock& clock, LogStore& log, StreamConfig cfg);
    ~AsEngine();

    // 自动泵（生产）：内部线程每 500µs 调 pump()；测试关闭后手动 pump()
    void set_auto_pump(bool on);
    void set_rate(std::uint32_t rateHz);
    void set_stream_config(StreamConfig cfg);

    // 设备→主机（iso IN）：挂起到 deadline，到期由 pump() 完成回调
    void on_iso_in(const usbip::UrbCommand& cmd, CompletionFn complete);
    // 主机→设备（iso OUT）：立即接收数据（RET 由 DeviceModel 生成），本引擎只消费数据
    void on_iso_out(std::span<const unsigned char> payload,
                    std::span<const usbip::IsoPacket> iso);

    void set_source_sine(double hz, double amp);
    void set_source_file(const std::string& wavPath);
    void set_source_silence();

    void set_capture_sink(const std::string& wavPath);

    struct Stats {
        std::uint64_t inRequests = 0, outRequests = 0;
        std::uint64_t inBytes = 0, outBytes = 0;
        std::uint64_t inLate = 0; // 到期时仍在排队（供诊断）
    };
    Stats stats() const;

    void pump();                 // 处理到期完成项（幂等、线程安全）
    bool cancel(std::uint32_t seqnum); // UNLINK：返回是否确有挂起项被取消

    // UAC2 feedback 端点数值（12.13 格式，Q12.13 每微帧采样数）
    static std::uint32_t fb_value_12_13(std::uint32_t rateHz, int ppmDrift = 0);

private:
    struct PendingIn {
        std::uint32_t seqnum;
        std::uint16_t nPackets;
        std::uint64_t deadlineUs;
        CompletionFn complete;
    };
    std::vector<unsigned char> fill_packets(std::uint16_t nPackets,
                                            std::vector<usbip::IsoPacket>& isoOut);

    MicroframeClock& clock_;
    LogStore& log_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    StreamConfig cfg_;
    std::deque<PendingIn> pending_;

    // 源状态
    enum class Src { Silence, Sine, File } src_ = Src::Silence;
    double sineHz_ = 1000.0, sineAmp_ = 0.5;
    double phase_ = 0.0; // 单声道相位（连续跨包）
    std::vector<std::int16_t> fileBuf_;
    std::uint64_t filePos_ = 0;

    WavWriter capture_;
    Stats stats_;
    std::atomic<bool> autoPump_{false};
    std::thread pumpThread_;
    bool pumpThreadStarted_ = false;
};

} // namespace uac
