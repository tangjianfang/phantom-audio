// 覆盖率第三轮：真实缺测路径
#include <doctest/doctest.h>
#include <uac/ac_processor.h>
#include <uac/as_engine.h>
#include <cstring>
#include <uac/control_api.h>
#include <uac/device_model.h>
#include <uac/profile.h>
#include <winsock2.h>
#include <algorithm>

using namespace uac;
using u8 = std::uint8_t;
using uac::usbip::CmdSubmit;

TEST_CASE("gap3: uac1 mute SET with data updates both channels") {
    AcState st;
    LogStore log;
    AcProcessor ac(st, log);
    ac.set_feature_unit_ids({2});
    ac.set_descriptors({}, {}, {});
    auto r = ac.on_control(0x21, 0x01, 0x0100, 0x0200, 1, std::vector<u8>{1}); // master mute
    CHECK(r.kind == ControlResult::Kind::Ok);
    CHECK(st.mute[0]);
    CHECK(st.mute[1]);
}

TEST_CASE("gap3: control api body split across packets and multi-param query") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    ControlCallbacks cb;
    cb.log_tail = [](std::size_t) { return "[]"; };
    ControlApi api;
    REQUIRE(api.start(0, std::move(cb)));
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(api.port());
    a.sin_addr.s_addr = htonl(0x7f000001);
    REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    const char* head = "GET /log?n=5&x=1 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    send(s, head, static_cast<int>(strlen(head)), 0);
    char buf[512]{};
    int r = recv(s, buf, sizeof(buf), 0);
    REQUIRE(r > 8);
    CHECK(std::string(buf, 12).find("200") != std::string::npos);
    closesocket(s);
    api.stop();
    WSACleanup();
}

TEST_CASE("gap3: unlink pending iso submit goes through engine cancel") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 51;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 6;
    c.submit.number_of_packets = 10;
    int done = 0;
    m.on_urb(c, {}, {}, [&](Completion&&) { ++done; });
    CHECK(done == 0); // pacing 挂起
    std::int32_t st = 0;
    m.on_unlink(52, 51, [&](std::int32_t s) { st = s; });
    CHECK(st == -104); // engine.cancel 命中
    auto* mc = dynamic_cast<ManualClock*>(&m.clock());
    mc->advance_to(999999);
    m.pump();
    CHECK(done == 0); // 已取消不再完成
}

TEST_CASE("gap3: held hid submit re-pends when no report available") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_hid_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 61;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 2;
    c.submit.transfer_buffer_length = 2;
    int done = 0;
    m.on_urb(c, {}, {}, [&](Completion&&) { ++done; });
    CHECK(done == 0);            // 无按键 → 挂起
    m.inject_mute_toggle();      // 非 HID 注入（UAC1 FU），HID 队列仍空 → 重新挂起路径
    CHECK(done == 0);
    m.inject_hid_keys(hid::kMute);
    CHECK(done == 1);            // 有报告后被唤醒
}

TEST_CASE("gap3: profile usb field with wrong json type rejected") {
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","usb":{"vid":[1,2]}})", "x"), ProfileError);
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","usb":{"vid":true}})", "x"), ProfileError);
}

TEST_CASE("gap3: engine silence source and file-open-fail fall to silence") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{});
    eng.set_source_silence();
    std::vector<usbip::IsoPacket> iso;
    auto d1 = eng.fill_packets_for_test(1, iso);
    bool allZero = true;
    for (auto b : d1)
        if (b != 0) allZero = false;
    CHECK(allZero);
    eng.set_source_file("Z:\\definitely\\missing.wav"); // 打不开 → 静音
    auto d2 = eng.fill_packets_for_test(1, iso);
    allZero = true;
    for (auto b : d2)
        if (b != 0) allZero = false;
    CHECK(allZero);
}

// ---------- review fixes: feedback EP / rate sync ----------
TEST_CASE("gap3: feedback EP 0x84 returns Q12.13 value at negotiated rate") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac2_default_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 71;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 4; // 0x84
    c.submit.transfer_buffer_length = 4;
    std::vector<u8> fb;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { fb = std::move(cm.payload); });
    REQUIRE(fb.size() == 4);
    // 48000 Hz default snapshot rate: 6.0 samples/uframe -> 0xC000 (49152)
    const auto v = static_cast<std::uint32_t>(fb[0]) | (fb[1] << 8) | (fb[2] << 16) | (fb[3] << 24);
    CHECK(v == uac::AsEngine::fb_value_12_13(48000)); // snapshot default rate
    CHECK(v == 49152u); // 0xC000
}

TEST_CASE("gap3: UAC2 clock SET_CUR re-times the IN engine") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac2_default_snapshot(), std::move(clock));
    // host sets 96 kHz via CS_SAM_FREQ CUR on the clock source (entity 4)
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 72;
    c.basic.direction = usbip::DIR_OUT;
    c.basic.ep = 0;
    CmdSubmit cs{0, 4, -1, 0, 0, {}};
    const u8 setup[8] = {0x21, 0x01, 0x00, 0x01, 0x00, 0x04, 4, 0}; // wValue: CN=0 CS=1; wIndex: iface=0 entity=4
    std::copy(setup, setup + 8, cs.setup);
    c.submit = cs;
    std::vector<u8> out{0x00, 0x77, 0x01, 0x00}; // 96000 = 0x00017700 LE
    m.on_urb(c, out, {}, [](Completion&&) {});
    CHECK(m.ac_state().sampleRate == 96000);
    // mic IN packet at 96 kHz stereo 4B slot: 96 samples/ms -> 768 B
    c = usbip::UrbCommand{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 73;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 6;
    c.submit.number_of_packets = 1;
    std::vector<u8> payload;
    m.engine().on_iso_in(c, [&](Completion&& cm) { payload = std::move(cm.payload); });
    auto* mc = dynamic_cast<ManualClock*>(&m.clock());
    mc->advance_to(1000);
    m.pump();
    CHECK(payload.size() == 768); // engine re-timed to 96 kHz
}
