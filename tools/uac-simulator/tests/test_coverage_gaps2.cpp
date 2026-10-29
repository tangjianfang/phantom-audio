// 覆盖率补齐第二轮（对应 /Od 精确归因后的缺口）
#include <doctest/doctest.h>
#include <uac/control_api.h>
#include <uac/device_model.h>
#include <uac/desc_uac2.h>
#include <uac/as_engine.h>
#include <uac/profile.h>
#include <uac/ude_adapter.h>
#include <uac/usbip_server.h>
#include <uac/endian.h>
#include <winsock2.h>
#include <algorithm>

using namespace uac;
using u8 = std::uint8_t;

TEST_CASE("gap2: hid report descriptor truncated to wLength") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_hid_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 1;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 0;
    const u8 setup[8] = {0x81, 0x06, 0x00, 0x22, 0x03, 0x00, 10, 0}; // wLength=10
    std::copy(setup, setup + 8, c.submit.setup);
    std::vector<u8> rep;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { rep = std::move(cm.payload); });
    REQUIRE(rep.size() == 10);
    CHECK(rep[1] == 0x0C);
}

TEST_CASE("gap2: ep0 class stall propagates through device model") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 2;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 0;
    const u8 setup[8] = {0xA1, 0x77, 0, 0, 0, 0, 4, 0};
    std::copy(setup, setup + 8, c.submit.setup);
    std::int32_t st = 0;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { st = cm.ret.status; });
    CHECK(st == -32);
}

TEST_CASE("gap2: unlink cancels held interrupt submit") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac2_default_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 31;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 1;
    int completed = 0;
    m.on_urb(c, {}, {}, [&](Completion&&) { ++completed; });
    CHECK(completed == 0);
    std::int32_t unlinkStatus = 0;
    m.on_unlink(32, 31, [&](std::int32_t s) { unlinkStatus = s; });
    CHECK(unlinkStatus == -104);
    m.inject_mute_toggle();
    CHECK(completed == 0);
}

TEST_CASE("gap2: profile volumeDb missing entries use defaults; name without dot") {
    auto p = parse_profile_json(R"({"protocol":"uac1","audio":{"volumeDb":{}}})", "v");
    CHECK(p.volMin == -6144);
    CHECK(p.volMax == 0);
    CHECK(p.volRes == 256);
    auto p2 = parse_profile_json(R"({"protocol":"uac1"})", "noext");
    CHECK(p2.name == "noext");
}

TEST_CASE("gap2: ude lint catches AS alt with non-iso endpoint only") {
    desc::Builder b;
    b.config(2, 1, 0x80, 0x32);
    b.interface_(0, 0, 0, 0x01, 0x01, 0x00);
    constexpr u8 ac_hdr[9] = {0x09,0x24,0x01,0x00,0x01,0x2B,0x00,0x01,0x01};
    b.raw(ac_hdr);
    b.interface_(1, 0, 0, 0x01, 0x02, 0x00);
    b.interface_(1, 1, 1, 0x01, 0x02, 0x00);
    b.endpoint(0x07, 0x02, 64, 0); // bulk 数据端点 → AS 非 0 alt 无 iso
    auto cfg = b.build();
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    bool caught = false;
    for (auto&& e : ude::lint_common(*v))
        if (e.code == "as-alt-no-iso-endpoint") caught = true;
    CHECK(caught);
}

TEST_CASE("gap2: uac2 lint catches multiple clock sources") {
    uac2::Uac2Profile p;
    auto cfg = uac2::build_config_desc(p);
    std::vector<u8> extra(cfg.begin() + 35, cfg.begin() + 35 + 8); // 复制 Clock Source 段（config9+IAD8+IF9+AC头9=35）
    cfg.insert(cfg.end(), extra.begin(), extra.end());
    cfg[2] = static_cast<u8>(cfg.size() & 0xff);
    cfg[3] = static_cast<u8>(cfg.size() >> 8);
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    bool caught = false;
    for (auto&& e : uac2::lint_uac2(*v, cfg))
        if (e.code == "uac2-single-clock-source") caught = true;
    CHECK(caught);
}

TEST_CASE("gap2: engine set_rate and non-iso capture path") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{48000, 2, 2, 1000, 0});
    eng.set_rate(44100);
    eng.set_capture_sink("cov_cap2.wav");
    eng.on_iso_out(std::vector<u8>{0x11, 0x22, 0x33, 0x44}, {});
    CHECK(eng.stats().outBytes == 4);
    eng.set_capture_sink("");
    std::remove("cov_cap2.wav");
}

namespace {
struct NullDisp final : UrbDispatcher {
    void on_import(const usbip::UsbIpDevice&) override {}
    void on_urb(const usbip::UrbCommand&, std::span<const u8>, std::span<const usbip::IsoPacket>,
                CompletionFn) override {}
    void on_unlink(std::uint32_t, std::uint32_t, UnlinkResultFn done) override { done(0); }
    void on_detach() override {}
};
} // namespace

TEST_CASE("gap2: control api rejects second bind and malformed request line") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    ControlApi a, b;
    ControlCallbacks cb;
    cb.status = [] { return "{}"; };
    REQUIRE(a.start(0, cb));
    CHECK_FALSE(b.start(a.port(), cb));
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in ad{};
    ad.sin_family = AF_INET;
    ad.sin_port = htons(a.port());
    ad.sin_addr.s_addr = htonl(0x7f000001);
    REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&ad), sizeof(ad)) == 0);
    const char* garbage = "GARBAGE\r\n\r\n";
    send(s, garbage, 11, 0);
    char buf[256]{};
    int r = recv(s, buf, sizeof(buf), 0);
    if (r > 9)
        CHECK(std::string(buf, static_cast<std::size_t>(r)).find("400") != std::string::npos);
    closesocket(s);
    a.stop();
    b.stop();
    WSACleanup();
}

TEST_CASE("gap2: server rejects import while attached") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    NullDisp disp;
    usbip::UsbIpDevice d;
    d.busid = "1-1";
    d.speed = 3;
    UsbIpServer srv;
    REQUIRE(srv.start(0, disp, {d}));
    auto connect_and_import = [&] {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in ad{};
        ad.sin_family = AF_INET;
        ad.sin_port = htons(srv.port());
        ad.sin_addr.s_addr = htonl(0x7f000001);
        if (connect(s, reinterpret_cast<sockaddr*>(&ad), sizeof(ad)) != 0) return INVALID_SOCKET;
        std::vector<u8> pkt;
        uac::push_be<std::uint16_t>(pkt, usbip::kVersion);
        uac::push_be<std::uint16_t>(pkt, usbip::OP_REQ_IMPORT);
        uac::push_be<std::uint32_t>(pkt, 0);
        pkt.insert(pkt.end(), {'1', '-', '1', 0});
        pkt.resize(40, 0);
        send(s, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
        return s;
    };
    char buf[512];
    SOCKET c1 = connect_and_import();
    REQUIRE(recv(c1, buf, sizeof(buf), 0) > 8);
    for (int i = 0; i != 50; ++i)
        if (srv.attached()) break;
        else Sleep(20);
    REQUIRE(srv.attached());
    SOCKET c2 = connect_and_import();
    int r2 = recv(c2, buf, sizeof(buf), 0);
    REQUIRE(r2 >= 8);
    CHECK(uac::rd_be<std::uint32_t>(&buf[4]) == 1);
    CHECK(recv(c2, buf, sizeof(buf), 0) <= 0);
    closesocket(c2);
    closesocket(c1);
    srv.stop();
    WSACleanup();
}
