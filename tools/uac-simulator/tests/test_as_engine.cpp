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

// iso IN 完成语义（2026-11-01 真机定案）：RET_SUBMIT 必须**立即**完成——usbip-win2 客户端
// 对延迟的 iso IN 完成会弃管管道（固定 +1ms 即死；扬声器 0x06 的立即完成路径从未出问题）。
// 速率控制改为**内容记账**（见下个用例）。本用例锁定"立即完成 + 幂等"。
TEST_CASE("iso IN: completion is immediate (never deferred) and idempotent") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.on_iso_in(make_iso_in(7, 10), capture_completion());
    g_completed = 0;
    eng.pump(); // 不推时间：立即完成
    CHECK(g_completed == 1);
    eng.pump();
    CHECK(g_completed == 1); // 幂等：不重复
    CHECK(eng.stats().inRequests == 1);
    CHECK(eng.stats().inBytes <= 10 * 192); // 受墙钟额度限制（新流 +2ms 预支 ≈ 2 包）
    CHECK(g_last.iso.size() == 10);
}

// 内容记账：K 个在飞 URB 全部立即完成，但**载荷字节**遵守墙钟额度——额度外的包为零长度帧。
// （此前按"到达+span"延迟排程：usbaudio.sys(UAC1) 压 ~6 个在飞 URB 时吞吐 K 倍，真机 6.09×
// 超采；而延迟完成又会被 usbip-win2 客户端弃管。零长度帧是真设备"本帧无数据"的合法形态。）
TEST_CASE("iso IN: K in-flight URBs all complete now; DATA obeys the wall-clock budget") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    constexpr int kInFlight = 6;
    for (uint32_t i = 1; i <= kInFlight; ++i)
        eng.on_iso_in(make_iso_in(i, 1), capture_completion());
    g_completed = 0;
    eng.pump(); // t=0：全部完成
    REQUIRE(g_completed == kInFlight);
    // t=0 的额度 = 2ms 预支 = 96 帧 = 2 个满包（48 帧/包 @48k/1ms）；其余为零长度
    int full = 0, empty = 0;
    // g_last 只留最后一个：改用统计断言——首包数据有、且 inBytes 不超过额度
    CHECK(eng.stats().inBytes == 2 * 192);
    CHECK(g_last.iso[0].actual_length == 0); // 最后一个 URB 在额度之外 → 零长度
    clock.advance_to(5000);
    eng.on_iso_in(make_iso_in(99, 1), capture_completion());
    g_completed = 0;
    eng.pump();
    REQUIRE(g_completed == 1);
    CHECK(g_last.iso[0].actual_length == 192); // 墙钟已过去 5ms，额度充足 → 满包
    CHECK(eng.stats().inBytes == 3 * 192);
    (void)full;
    (void)empty;
}

TEST_CASE("completion carries the requested packet count") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.on_iso_in(make_iso_in(1, 4), capture_completion());
    g_completed = 0;
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

TEST_CASE("feedback value Q16.16 (High-Speed) golden") {
    // USB 2.0 HS explicit feedback: samples/uframe * 2^16 (kernel endpoint.c)
    // 48k: 6.0 * 65536 = 393216 = 0x00060000
    CHECK(AsEngine::fb_value_16_16(48000) == 0x00060000);
    // 44.1k: 5.5125 * 65536 = 361267.2 -> 361267
    CHECK(AsEngine::fb_value_16_16(44100) == 361267);
    // +100ppm drift: 393216 * 1.0001 = 393255.3 -> 393255
    CHECK(AsEngine::fb_value_16_16(48000, 100) == 393255);
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
