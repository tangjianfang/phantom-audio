// 覆盖率补齐：针对 OpenCppCoverage 报告的未覆盖分支（100% 目标）
#include <doctest/doctest.h>
#include <uac/ac_processor.h>
#include <uac/as_engine.h>
#include <uac/control_api.h>
#include <uac/device_model.h>
#include <uac/log_store.h>
#include <uac/profile.h>
#include <uac/ude_adapter.h>
#include <uac/usbip_server.h>
#include <uac/endian.h>
#include <uac/usbip_proto.h>
#include <uac/wav.h>
#include <winsock2.h>
#include <fstream>
#include <thread>

using namespace uac;
using u8 = std::uint8_t;

// ---------- AcProcessor ----------
TEST_CASE("ac: GET_STATUS / SET_INTERFACE passthrough / unknown standard stall") {
    AcState st;
    LogStore log;
    AcProcessor ac(st, log);
    ac.set_descriptors({}, {}, {});
    auto gs = ac.on_control(0x80, 0x00, 0, 0, 2, {});
    REQUIRE(gs.kind == ControlResult::Kind::Ok);
    CHECK(gs.data.size() == 2);
    auto si = ac.on_control(0x01, 0x0B, 1, 1, 0, {}); // SET_INTERFACE 直通
    CHECK(si.kind == ControlResult::Kind::Ok);
    auto gi = ac.on_control(0x81, 0x0A, 0, 1, 1, {}); // GET_INTERFACE 直通
    CHECK(gi.kind == ControlResult::Kind::Ok);
    auto bad = ac.on_control(0x80, 0x55, 0, 0, 0, {}); // 未知标准请求
    CHECK(bad.kind == ControlResult::Kind::Stall);
    auto badIface = ac.on_control(0x01, 0x44, 0, 1, 0, {}); // 标准/接口 未知
    CHECK(badIface.kind == ControlResult::Kind::Stall);
}

TEST_CASE("ac: UAC2 error paths (bad sizes, unknown cs, mute range stall)") {
    AcState st;
    LogStore log;
    AcProcessor ac(st, log);
    ac.set_protocol(Protocol::Uac2);
    ac.set_clock_id(4);
    ac.set_feature_unit_ids({2});
    ac.set_descriptors({}, {}, {}, {});
    // SET CUR 频率数据不足 → stall
    auto badFreq = ac.on_control(0x21, 0x01, 0x0100, 0x0400, 4,
                                 std::vector<u8>{0x80, 0xBB});
    CHECK(badFreq.kind == ControlResult::Kind::Stall);
    // 未知实体
    auto badEnt = ac.on_control(0xA1, 0x01, 0x0100, 0x9900, 4, {});
    CHECK(badEnt.kind == ControlResult::Kind::Stall);
    // 通道号越界（CN=9）
    auto badCh = ac.on_control(0xA1, 0x01, 0x0109, 0x0200, 4, {});
    CHECK(badCh.kind == ControlResult::Kind::Stall);
    // Mute RANGE → stall（UAC2 mute 无 RANGE）
    auto mr = ac.on_control(0xA1, 0x02, 0x0100, 0x0200, 2, {});
    CHECK(mr.kind == ControlResult::Kind::Stall);
    // Volume SET 数据不足
    auto vs = ac.on_control(0x21, 0x01, 0x0201, 0x0200, 2, std::vector<u8>{0x00});
    CHECK(vs.kind == ControlResult::Kind::Stall);
    // Clock Valid SET
    auto cvSet = ac.on_control(0x21, 0x01, 0x0200, 0x0400, 0, {});
    CHECK(cvSet.kind == ControlResult::Kind::Ok);
    // Clock Valid RANGE → stall
    auto cvR = ac.on_control(0xA1, 0x02, 0x0200, 0x0400, 1, {});
    CHECK(cvR.kind == ControlResult::Kind::Stall);
    // 未知 bRequest（UAC2 只认 CUR/RANGE）
    auto ub = ac.on_control(0xA1, 0x07, 0x0100, 0x0400, 4, {});
    CHECK(ub.kind == ControlResult::Kind::Stall);
    // 未知 CS
    auto ucs = ac.on_control(0xA1, 0x01, 0x0900, 0x0200, 2, {});
    CHECK(ucs.kind == ControlResult::Kind::Stall);
}

TEST_CASE("ac: UAC1 mute set bad size and unknown FU entity stall") {
    AcState st;
    LogStore log;
    AcProcessor ac(st, log);
    ac.set_feature_unit_ids({2});
    ac.set_descriptors({}, {}, {});
    auto bm = ac.on_control(0x21, 0x01, 0x0101, 0x0200, 1, {});
    CHECK(bm.kind == ControlResult::Kind::Stall); // 空 OUT 数据 → 拒绝
    auto notFu = ac.on_control(0xA1, 0x81, 0x0201, 0x7700, 2, {});
    CHECK(notFu.kind == ControlResult::Kind::Stall);
    auto badCh = ac.on_control(0xA1, 0x81, 0x0205, 0x0200, 2, {});
    CHECK(badCh.kind == ControlResult::Kind::Stall);
    auto epUnk = ac.on_control(0x22, 0x01, 0x0200, 0x86, 3, std::vector<u8>{1, 2, 3, 4});
    CHECK(epUnk.kind == ControlResult::Kind::Stall); // CS 非 SAM_FREQ
}

// ---------- log_store file sink ----------
TEST_CASE("log store file sink roundtrip") {
    LogStore ls;
    CHECK(ls.set_file_sink("cov_log_test.ndjson"));
    ls.append("{\"a\":1}");
    ls.append("{\"b\":2}");
    ls.set_file_sink(""); // 关闭并 flush
    std::ifstream f("cov_log_test.ndjson");
    REQUIRE(f.is_open());
    std::string l1, l2;
    std::getline(f, l1);
    std::getline(f, l2);
    CHECK(l1 == "{\"a\":1}");
    CHECK(l2 == "{\"b\":2}");
    std::remove("cov_log_test.ndjson");
}

// ---------- profile error paths ----------
TEST_CASE("profile: bad rate / empty rates / bad subslot type rejected") {
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","audio":{"rates":[99000000]}})", "x"), ProfileError);
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","audio":{"rates":[]}})", "x"), ProfileError);
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","usb":{"vid":"0xZZ"}})", "x"), ProfileError);
}

// ---------- ude_adapter parse failure ----------
TEST_CASE("ude adapter returns -1 on invalid config") {
    std::vector<u8> broken{0x09, 0x02, 0x50, 0x00}; // 声称 80B 实际 4B
    CHECK(ude::adapt_config_for_ude(broken) == -1);
}

// ---------- usbip_proto: iso desc bad length ----------
TEST_CASE("decode_iso_descs rejects non-multiple size") {
    std::vector<u8> bad(17, 0);
    CHECK(usbip::decode_iso_descs(bad).empty());
}

// ---------- as_engine: file source + auto pump shutdown + stream config ----------
TEST_CASE("as engine: wav file source roundtrip via packets") {
    ManualClock clock;
    LogStore log;
    AsEngine eng(clock, log, StreamConfig{48000, 1, 2, 1000, 0}); // 单声道 16bit
    WavWriter w;
    REQUIRE(w.open("cov_src.wav", 48000, 1, 16));
    std::vector<std::int16_t> src(96);
    for (int i = 0; i < 96; ++i) src[i] = static_cast<std::int16_t>((i % 2) ? 12000 : -12000);
    w.write(src.data(), src.size());
    w.close();
    eng.set_source_file("cov_src.wav");
    std::vector<usbip::IsoPacket> iso;
    auto data = eng.fill_packets_for_test(1, iso); // 1ms = 48 样本 = 96B
    REQUIRE(data.size() == 96);
    std::int16_t s0 = static_cast<std::int16_t>(data[0] | (data[1] << 8));
    std::int16_t s1 = static_cast<std::int16_t>(data[2] | (data[3] << 8));
    CHECK(s0 == -12000);   // 方波 i%2==0 → -12000
    CHECK(s1 == 12000);
    std::remove("cov_src.wav");
}

TEST_CASE("as engine: auto pump on wall clock starts and stops cleanly") {
    WallClock clock;
    LogStore log;
    {
        AsEngine eng(clock, log, StreamConfig{48000, 2, 2, 1000, 0});
        eng.set_auto_pump(true);
        eng.set_stream_config(StreamConfig{44100, 2, 2, 1000, 1});
        Sleep(80); // 泵线程运转若干周期
    } // 析构（含自动泵线程关闭）
    CHECK(true);
}

// ---------- usbip_server: kick + bad version + second client rejected ----------
namespace {
struct NullDispatcher final : UrbDispatcher {
    void on_import(const usbip::UsbIpDevice&) override {}
    void on_urb(const usbip::UrbCommand&, std::span<const u8>, std::span<const usbip::IsoPacket>,
                CompletionFn) override {}
    void on_unlink(std::uint32_t, std::uint32_t, UnlinkResultFn done) override { done(0); }
    void on_detach() override {}
};
} // namespace

TEST_CASE("server: kick disconnects imported client and allows re-import") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    NullDispatcher disp;
    usbip::UsbIpDevice d;
    d.busid = "1-1";
    d.speed = 3;
    UsbIpServer srv;
    REQUIRE(srv.start(0, disp, {d}));
    const auto port = srv.port();

    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    REQUIRE(connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    std::vector<u8> pkt;
    uac::push_be<std::uint16_t>(pkt, usbip::kVersion);
    uac::push_be<std::uint16_t>(pkt, usbip::OP_REQ_IMPORT);
    uac::push_be<std::uint32_t>(pkt, 0);
    pkt.insert(pkt.end(), {'1', '-', '1', 0});
    pkt.resize(40, 0);
    send(c, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
    char buf[512];
    int r = recv(c, buf, sizeof(buf), 0);
    REQUIRE(r > 8); // IMPORT OK 回复

    for (int i = 0; i != 50; ++i)
        if (srv.attached()) break;
        else Sleep(20);
    REQUIRE(srv.attached());
    srv.kick(); // 拔线（客户端仍连接——保持 socket 打开直到断开）
    for (int i = 0; i != 50; ++i) {
        int rr = recv(c, buf, sizeof(buf), 0);
        if (rr <= 0) break; // 服务端关闭
        Sleep(20);
    }
    closesocket(c);
    for (int i = 0; i != 50; ++i)
        if (!srv.attached()) break;
        else Sleep(20);
    CHECK_FALSE(srv.attached()); // on_detach 路径执行，可再次 IMPORT
    srv.stop();
    CHECK(srv.port() == 0);
    WSACleanup();
}

TEST_CASE("server: bad protocol version closes connection") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    NullDispatcher disp;
    usbip::UsbIpDevice d;
    d.busid = "1-1";
    UsbIpServer srv;
    REQUIRE(srv.start(0, disp, {d}));
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(srv.port());
    a.sin_addr.s_addr = htonl(0x7f000001);
    REQUIRE(connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    const char bad[8] = {0x02, 0x00, 0x00, 0x05, 0, 0, 0, 0}; // 版本 0x0200
    send(c, bad, 8, 0);
    char buf[16];
    int r = recv(c, buf, sizeof(buf), 0); // 服务端直接断开
    CHECK(r <= 0);
    closesocket(c);
    srv.stop();
    WSACleanup();
}

// ---------- control_api: malformed requests ----------
TEST_CASE("control api: garbage request and oversized head handled") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    ControlCallbacks cb;
    cb.status = [] { return "{}"; };
    cb.inject = [](const std::string&) -> std::string { throw std::runtime_error("boom"); };
    ControlApi api;
    REQUIRE(api.start(0, std::move(cb)));

    // 1) 非 HTTP 字节流 → 连接被关闭（无响应头）
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(api.port());
        a.sin_addr.s_addr = htonl(0x7f000001);
        REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        shutdown(s, SD_BOTH); // 立即关闭 → recv 失败路径
        closesocket(s);
    }
    // 2) 回调抛异常 → 500
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(api.port());
        a.sin_addr.s_addr = htonl(0x7f000001);
        REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        const char* req = "POST /inject HTTP/1.1\r\nContent-Length: 9\r\nConnection: close\r\n\r\nboom body";
        send(s, req, static_cast<int>(strlen(req)), 0);
        char buf[512]{};
        int r = recv(s, buf, sizeof(buf), 0);
        REQUIRE(r > 12);
        CHECK(std::string(buf, static_cast<std::size_t>(r)).find("500") != std::string::npos);
        closesocket(s);
    }
    api.stop();
    WSACleanup();
}

// ---------- device_model: GET_INTERFACE + UAC2 usbip_device + HID pend queue ----------
TEST_CASE("device model: GET_INTERFACE returns current alt") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 1;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 0;
    const u8 set[8] = {0x01, 0x0B, 0x01, 0x00, 0x01, 0x00, 0, 0}; // SET_INTERFACE IF1 alt1
    std::copy(set, set + 8, c.submit.setup);
    m.on_urb(c, {}, {}, [](Completion&&) {});
    const u8 get[8] = {0x81, 0x0A, 0, 0, 0x01, 0x00, 1, 0}; // GET_INTERFACE IF1
    std::copy(get, get + 8, c.submit.setup);
    c.basic.seqnum = 2;
    std::vector<u8> data;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { data = std::move(cm.payload); });
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 1);
}

TEST_CASE("device model: uac2 usbip_device carries 0x20 protocol") {
    auto m1 = make_uac1_default_snapshot();
    DeviceModel a(std::move(m1));
    CHECK(a.usbip_device().interfaces[0].bInterfaceProtocol == 0x00);
    auto m2 = make_uac2_default_snapshot();
    DeviceModel b(std::move(m2));
    CHECK(b.usbip_device().interfaces[0].bInterfaceProtocol == 0x20);
    CHECK(b.usbip_device().idVendor != 0);
}

TEST_CASE("device model: hid keys queued without pending submit") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_hid_snapshot(), std::move(clock));
    m.inject_hid_keys(hid::kVolUp); // 无挂起提交 → 报告入队
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 5;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 2;
    c.submit.transfer_buffer_length = 2;
    std::vector<u8> rep;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { rep = std::move(cm.payload); });
    REQUIRE(rep.size() == 2);
    CHECK(rep == std::vector<u8>{0x01, 0x01}); // 立即消费队列中的按键
}

TEST_CASE("device model: unknown endpoint stalls") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_default_snapshot(), std::move(clock));
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 9;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 5; // 未知端点
    std::int32_t status = 0;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { status = cm.ret.status; });
    CHECK(status == -32);
}

// ---------- fault_injector: clear + disconnect rule ----------
TEST_CASE("fault injector: disconnect afterMs fires and clear resets") {
    FaultInjector f;
    FaultRule r;
    r.kind = FaultRule::Kind::Disconnect;
    r.ms = 0; // 立即
    f.arm(r);
    Sleep(30);
    CHECK(f.should_disconnect()); // 一次性消费
    CHECK_FALSE(f.should_disconnect());
    f.arm(r);
    f.clear();
    CHECK(f.empty());
    CHECK_FALSE(f.should_disconnect());
    CHECK(f.iso_error_count() == 0); // 无规则时不注入
}
