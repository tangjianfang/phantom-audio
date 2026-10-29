// 故障注入测试：STALL / 延迟 / 描述符损坏 / 半枚举断连 / iso error
#include <doctest/doctest.h>
#include <uac/device_model.h>

using namespace uac;

namespace {
usbip::UrbCommand ctrl_cmd(std::uint32_t seq, std::array<std::uint8_t, 8> setup) {
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = seq;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 0;
    std::copy(setup.begin(), setup.end(), c.submit.setup);
    return c;
}
} // namespace

TEST_CASE("stall nth matching request") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    FaultRule r;
    r.kind = FaultRule::Kind::StallNth;
    r.bRequest = 0x81; // GET_CUR
    r.nth = 2;         // 第二条 GET_CUR STALL
    m.faults().arm(r);

    std::vector<std::int32_t> statuses;
    for (int i = 0; i < 2; ++i) {
        auto c = ctrl_cmd(10 + i, {0xA1, 0x81, 0x02, 0x01, 0x00, 0x02, 2, 0});
        m.on_urb(c, {}, {}, [&](Completion&& cm) { statuses.push_back(cm.ret.status); });
    }
    REQUIRE(statuses.size() == 2);
    CHECK(statuses[0] == 0);
    CHECK(statuses[1] == -32); // -EPIPE
    auto fired = m.faults().fired();
    REQUIRE(!fired.empty());
    CHECK(fired.back().find("stall") != std::string::npos);
}

TEST_CASE("delay defers completion by ms") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    FaultRule r;
    r.kind = FaultRule::Kind::DelayMs;
    r.bRequest = 0x06; // GET_DESCRIPTOR
    r.ms = 120;
    m.faults().arm(r);

    std::atomic<int> done{0};
    auto c = ctrl_cmd(1, {0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0});
    auto t0 = std::chrono::steady_clock::now();
    m.on_urb(c, {}, {}, [&](Completion&&) { ++done; });
    CHECK(done.load() == 0); // 未到时不完成
    Sleep(260);
    REQUIRE(done.load() == 1);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    CHECK(elapsed >= 100);
}

TEST_CASE("corrupt config descriptor byte") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    FaultRule r;
    r.kind = FaultRule::Kind::CorruptDescByteAt;
    r.byteOffset = 100; // iso EP interval 字节
    r.xorByte = 0x55;
    m.faults().arm(r);

    std::vector<unsigned char> cfg;
    auto c = ctrl_cmd(1, {0x80, 0x06, 0x00, 0x02, 0, 0, 0xFF, 0});
    m.on_urb(c, {}, {}, [&](Completion&& cm) { cfg = std::move(cm.payload); });
    REQUIRE(!cfg.empty());
    auto clean = make_uac1_default_snapshot();
    CHECK(cfg[100] == (clean.config[100] ^ 0x55));
    CHECK(cfg[2] == clean.config[2]); // 其余不动（长度同）
}

TEST_CASE("half enumerate kicks disconnect hook") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    std::atomic<int> kicked{0};
    m.set_disconnect_hook([&] { ++kicked; });
    FaultRule r;
    r.kind = FaultRule::Kind::HalfEnumerate;
    r.stopAtStep = 2; // 第 2 条 EP0 请求后断连
    m.faults().arm(r);

    for (int i = 0; i < 3; ++i) {
        auto c = ctrl_cmd(20 + i, {0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0});
        m.on_urb(c, {}, {}, [](Completion&&) {});
    }
    CHECK(kicked.load() == 1);
    for (auto& f : m.faults().fired()) MESSAGE(f);
}

TEST_CASE("iso error injection wraps engine completion") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    FaultRule r;
    r.kind = FaultRule::Kind::IsoError;
    r.everyN = 2; // 每 2 个 iso RET 注入
    r.errCount = 3;
    m.faults().arm(r);

    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 1;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 6;
    c.submit.number_of_packets = 2;
    std::vector<std::uint32_t> errors;
    for (int i = 0; i < 3; ++i) {
        c.basic.seqnum = 100 + i;
        m.on_urb(c, {}, {}, [&](Completion&& cm) { errors.push_back(cm.ret.error_count); });
        auto* mc = dynamic_cast<ManualClock*>(&m.clock());
        mc->advance_to(mc->now_us() + 2000);
        m.pump();
    }
    REQUIRE(errors.size() == 3);
    CHECK(errors[0] == 0);
    CHECK(errors[1] == 3); // 第 2 个注入
    CHECK(errors[2] == 0);
}
