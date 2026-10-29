// 微帧时钟 + AS 引擎 + WAV 测试
#include <doctest/doctest.h>
#include <uac/as_engine.h>
#include <cstdio>
#include <fstream>

using namespace uac;
using usbip::IsoPacket;

namespace {
Completion g_last;
int g_completed = 0;

CompletionFn capture_completion() {
    return [](Completion&& cm) {
        g_last = std::move(cm);
        ++g_completed;
    };
}
usbip::UrbCommand make_iso_in(uint32_t seqnum, uint32_t nPackets) {
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = seqnum;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 2; // IN 端点 0x86 → ep 2? (vhci ep 字段为端点号)
    c.submit.number_of_packets = nPackets;
    c.submit.transfer_buffer_length = nPackets * 192;
    return c;
}
} // namespace

TEST_CASE("sine packer: 10 packets of 192 bytes, waveform approximates 1kHz") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.set_source_sine(1000.0, 0.5);
    eng.on_iso_in(make_iso_in(1, 10), capture_completion());
    g_completed = 0;
    clock.advance_to(10000);
    eng.pump();
    REQUIRE(g_completed == 1);
    REQUIRE(g_last.iso.size() == 10);
    REQUIRE(g_last.payload.size() == 10 * 192);
    CHECK(g_last.ret.number_of_packets == 10);
    CHECK(g_last.ret.actual_length == 10 * 192);
    std::uint32_t off = 0;
    for (auto&& p : g_last.iso) {
        CHECK(p.offset == off);
        CHECK(p.length == 192);
        CHECK(p.status == 0);
        off += 192;
    }
    // 波形：首样本 sin(0)=0；样本 12 处 ≈ sin(2π*1000*12/48000)=sin(π/2)=1 → ≈ 0.5*32767
    auto s0 = static_cast<std::int16_t>(g_last.payload[0] | (g_last.payload[1] << 8));
    auto s12 = static_cast<std::int16_t>(g_last.payload[24] | (g_last.payload[25] << 8));
    CHECK(std::abs(s0) <= 40);
    CHECK(std::abs(s12 - 16384) <= 200); // 0.5 满幅
    // 相位连续：第二包首样本 = 样本 48 → sin(2π*1000*48/48000) = sin(π)=0
    auto s48 = static_cast<std::int16_t>(g_last.payload[192] | (g_last.payload[193] << 8));
    CHECK(std::abs(s48) <= 40);
}

TEST_CASE("pacing: completion strictly at deadline, never early, exactly once") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.on_iso_in(make_iso_in(7, 10), capture_completion());
    g_completed = 0;
    clock.advance_to(9999);
    eng.pump();
    CHECK(g_completed == 0); // 未到期
    clock.advance_to(10000);
    eng.pump();
    CHECK(g_completed == 1); // 恰好到期
    eng.pump();
    CHECK(g_completed == 1); // 幂等：不重复
    CHECK(eng.stats().inRequests == 1);
    CHECK(eng.stats().inBytes == 10 * 192);
}

TEST_CASE("deadline scales with packet count") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.on_iso_in(make_iso_in(1, 4), capture_completion()); // 4ms
    g_completed = 0;
    clock.advance_to(3999);
    eng.pump();
    CHECK(g_completed == 0);
    clock.advance_to(4000);
    eng.pump();
    CHECK(g_completed == 1);
    CHECK(g_last.iso.size() == 4);
}

TEST_CASE("cancel pending submit prevents completion") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.on_iso_in(make_iso_in(42, 10), capture_completion());
    g_completed = 0;
    CHECK(eng.cancel(42) == true);
    clock.advance_to(20000);
    eng.pump();
    CHECK(g_completed == 0);
    CHECK(eng.cancel(42) == false); // 已无此项
}

TEST_CASE("iso out captured to wav, sizes patched on close") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    const std::string path = "as_test_capture.wav";
    eng.set_capture_sink(path);
    // 3 包 × 8 样本（16bit 立体声 → 每包 32B）
    std::vector<unsigned char> payload(3 * 32, 0x11);
    std::vector<IsoPacket> iso(3);
    for (int i = 0; i < 3; ++i) {
        iso[i].offset = i * 32;
        iso[i].length = 32;
        iso[i].actual_length = 32;
    }
    eng.on_iso_out(payload, iso);
    CHECK(eng.stats().outBytes == 96);
    eng.set_capture_sink(""); // 关闭并落盘
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.is_open());
    char hdr[44];
    f.read(hdr, 44);
    CHECK(std::equal(hdr, hdr + 4, "RIFF"));
    CHECK(std::equal(hdr + 8, hdr + 12, "WAVE"));
    std::uint32_t riffSize;
    std::memcpy(&riffSize, hdr + 4, 4);
    CHECK(riffSize == 36 + 96);
    std::uint32_t dataSize;
    std::memcpy(&dataSize, hdr + 40, 4);
    CHECK(dataSize == 96);
    f.close();
    std::remove(path.c_str());
}

TEST_CASE("slot drift stays within +/-1 slot") {
    ManualClock clock;
    LogStore log;
    StreamConfig cfg;
    cfg.slotDrift = 1;
    AsEngine eng(clock, log, cfg);
    eng.on_iso_in(make_iso_in(1, 2), capture_completion());
    g_completed = 0;
    clock.advance_to(2000);
    eng.pump();
    REQUIRE(g_completed == 1);
    CHECK(g_last.iso[0].length == 192); // 48 slots 基准
    CHECK(g_last.iso[1].length == 196); // 奇数包 +1 slot（4B）
    CHECK(g_last.payload.size() == 388);
}

TEST_CASE("feedback value Q12.13 golden") {
    // 48k：6.0 samples/µframe × 2^13 = 49152 = 0x0000C000
    CHECK(AsEngine::fb_value_12_13(48000) == 0x0000C000);
    // 44.1k：5.5125 × 8192 = 45158.4 → 45158
    CHECK(AsEngine::fb_value_12_13(44100) == 45158);
    // +100ppm 漂移
    CHECK(AsEngine::fb_value_12_13(48000, 100) == 49157); // 49152×1.0001 = 49156.99 → 49157
}

TEST_CASE("auto pump thread completes within wall-clock deadline") {
    WallClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.set_auto_pump(true);
    eng.on_iso_in(make_iso_in(9, 2), capture_completion()); // 2ms
    g_completed = 0;
    Sleep(150); // 500µs 泵周期，150ms 足够
    eng.set_auto_pump(false);
    CHECK(g_completed == 1);
}
