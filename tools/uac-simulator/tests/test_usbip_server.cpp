// 传输层回环集成测试：fake-vhci 客户端 ↔ UsbIpServer ↔ FakeDispatcher
// （doctest 主实现位于 test_smoke.cpp）
#include <doctest/doctest.h>
#include <uac/usbip_server.h>
#include <uac/endian.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <mutex>

using namespace uac;
using namespace uac::usbip;
using u8 = unsigned char;

// ---------- 测试用 mini TCP 客户端 ----------
class TcpClient {
public:
    explicit TcpClient(uint16_t port) {
        s_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(s_ != INVALID_SOCKET);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(0x7f000001);
        REQUIRE(connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    }
    ~TcpClient() { closesocket(s_); }
    void send_all(std::span<const u8> b) {
        size_t off = 0;
        while (off < b.size()) {
            int n = ::send(s_, reinterpret_cast<const char*>(b.data() + off),
                           static_cast<int>(b.size() - off), 0);
            REQUIRE(n > 0);
            off += static_cast<size_t>(n);
        }
    }
    std::vector<u8> recv_exact(size_t n) {
        std::vector<u8> buf(n);
        size_t off = 0;
        while (off < n) {
            int r = ::recv(s_, reinterpret_cast<char*>(buf.data() + off),
                           static_cast<int>(n - off), 0);
            if (r <= 0) { buf.resize(off); closesocket(s_); s_ = INVALID_SOCKET; break; }
            off += static_cast<size_t>(r);
        }
        return buf; // size()==n 成功；<n 表示连接关闭（EOF）
    }
    void close() { if (s_ != INVALID_SOCKET) { closesocket(s_); s_ = INVALID_SOCKET; } }

private:
    SOCKET s_ = INVALID_SOCKET;
};

struct CommonHdr { uint16_t version, code; uint32_t status; };
CommonHdr recv_common(TcpClient& c) {
    auto b = c.recv_exact(8);
    REQUIRE(b.size() == 8);
    return {uac::rd_be<uint16_t>(&b[0]), uac::rd_be<uint16_t>(&b[2]), uac::rd_be<uint32_t>(&b[4])};
}
void send_op(TcpClient& c, uint16_t code, std::span<const u8> body = {}) {
    std::vector<u8> pkt;
    uac::push_be<uint16_t>(pkt, kVersion);
    uac::push_be<uint16_t>(pkt, code);
    uac::push_be<uint32_t>(pkt, 0);
    pkt.insert(pkt.end(), body.begin(), body.end());
    c.send_all(pkt);
}

// ---------- Fake 设备与分发器 ----------
UsbIpDevice make_sim_device() {
    UsbIpDevice d;
    d.path = "/uacsim/0"; d.busid = "1-1";
    d.busnum = 1; d.devnum = 1; d.speed = 3; // HS —— UDE 适配
    d.idVendor = 0x1D50; d.idProduct = 0x6222; d.bcdDevice = 0x0100;
    d.bNumInterfaces = 2;
    d.interfaces = {{0x01,0x01,0x00},{0x01,0x02,0x00}};
    return d;
}

struct FakeDispatcher final : UrbDispatcher {
    std::mutex mtx;
    int imports = 0, detaches = 0, urbs = 0, unlinks = 0;
    std::vector<uint32_t> unlinked_seqnums;
    std::function<void(const UrbCommand&, CompletionFn)> urb_handler; // 可注入行为

    void on_import(const UsbIpDevice&) override {
        std::lock_guard lk(mtx); ++imports;
    }
    void on_urb(const UrbCommand& cmd, std::span<const u8>, std::span<const IsoPacket>,
                CompletionFn complete) override {
        std::lock_guard lk(mtx); ++urbs;
        if (urb_handler) urb_handler(cmd, std::move(complete));
    }
    void on_unlink(uint32_t, uint32_t unlink_seqnum, UnlinkResultFn done) override {
        std::lock_guard lk(mtx);
        ++unlinks; unlinked_seqnums.push_back(unlink_seqnum);
        done(-104); // -ECONNRESET
    }
    void on_detach() override { std::lock_guard lk(mtx); ++detaches; }
};

// 等待谓词为真（带超时），避免测试挂死
template <class F>
bool wait_for(F&& f, int timeout_ms = 3000) {
    for (int i = 0; i != timeout_ms / 10; ++i) {
        if (f()) return true;
        Sleep(10);
    }
    return f();
}

TEST_SUITE_BEGIN("usbip server");

TEST_CASE("devlist -> import -> control URB roundtrip -> unlink -> detach") {
    WSADATA wsa; REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    FakeDispatcher disp;
    UsbIpServer srv;
    REQUIRE(srv.start(0, disp, {make_sim_device()}));
    const auto port = srv.port();
    CHECK(port != 0);

    // --- 1) DEVLIST 查询 ---
    {
        TcpClient c(port);
        send_op(c, OP_REQ_DEVLIST);
        auto h = recv_common(c);
        CHECK(h.code == OP_REP_DEVLIST);
        CHECK(h.status == 0);
        auto rest = c.recv_exact(4);
        CHECK(uac::rd_be<uint32_t>(rest.data()) == 1);
        auto dev = c.recv_exact(kUsbIpDeviceSize + 2 * 4);
        CHECK(dev[0] == '/'); // path
    }
    CHECK_FALSE(srv.attached());

    // --- 2) IMPORT 错误 busid 被拒 ---
    {
        TcpClient c(port);
        std::vector<u8> busid(32, 0); busid[0] = '9'; busid[1] = '-';
        send_op(c, OP_REQ_IMPORT, busid);
        auto h = recv_common(c);
        CHECK(h.code == OP_REP_IMPORT);
        CHECK(h.status == 1);
        // 错误后服务端关闭连接：继续读得到 EOF
        auto tail = c.recv_exact(1);
        CHECK(tail.empty());
    }
    CHECK_FALSE(srv.attached());

    // --- 3) IMPORT 正确 busid 进入 URB 通道 ---
    TcpClient att(port);
    {
        std::vector<u8> busid(32, 0); busid[0] = '1'; busid[1] = '-'; busid[2] = '1';
        send_op(att, OP_REQ_IMPORT, busid);
        auto h = recv_common(att);
        CHECK(h.code == OP_REP_IMPORT);
        CHECK(h.status == 0);
        auto dev = att.recv_exact(kUsbIpDeviceSize);
        CHECK(dev[256] == '1'); // busid "1-1"
    }
    CHECK(wait_for([&] { return srv.attached(); }));
    CHECK(wait_for([&] { std::lock_guard lk(disp.mtx); return disp.imports == 1; }));

    // --- 4) 控制 IN：GET_DESCRIPTOR(Device)，fake 立即完成 ---
    disp.urb_handler = [](const UrbCommand& cmd, CompletionFn complete) {
        if (cmd.basic.direction == DIR_IN && cmd.submit.setup[0] == 0x80) {
            Completion cm;
            cm.ret.actual_length = 18;
            cm.payload = std::vector<u8>(18, 0xAB);
            complete(std::move(cm));
        }
        // 其他（如挂起中的）不完成，留给 UNLINK 用
    };
    {
        BasicHeader b{CMD_SUBMIT, 0x100, 0x00010001, DIR_IN, 0};
        CmdSubmit cs{0, 18, -1, 0, 0, {0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0}};
        att.send_all(encode_cmd_submit(b, cs));
        auto rh = att.recv_exact(48);
        REQUIRE(rh.size() == 48);
        REQUIRE(uac::rd_be<uint32_t>(&rh[0]) == RET_SUBMIT);
        CHECK(uac::rd_be<uint32_t>(&rh[4]) == 0x100); // seqnum 匹配
        CHECK(uac::rd_be<int32_t>(&rh[20]) == 0);     // status
        auto body = att.recv_exact(18);
        CHECK(body.front() == 0xAB);
    }

    // --- 5) 挂起的 IN URB + UNLINK：RET_UNLINK(-ECONNRESET)，被取消者无 RET ---
    {
        BasicHeader b{CMD_SUBMIT, 0x101, 0x00010001, DIR_IN, 1};
        CmdSubmit cs{0, 64, -1, 0, 4, {}};
        att.send_all(encode_cmd_submit(b, cs)); // handler 不完成 → 挂起

        BasicHeader u{CMD_UNLINK, 0x102, 0, 0, 0};
        std::vector<u8> uh(48, 0);
        uac::wr_be<uint32_t>(uh.data() + 0, CMD_UNLINK);
        uac::wr_be<uint32_t>(uh.data() + 4, 0x102);
        uac::wr_be<uint32_t>(uh.data() + 20, 0x101); // unlink_seqnum
        att.send_all(uh);

        auto rh = att.recv_exact(48);
        REQUIRE(uac::rd_be<uint32_t>(&rh[0]) == RET_UNLINK);
        CHECK(uac::rd_be<uint32_t>(&rh[4]) == 0x102);
        CHECK(uac::rd_be<int32_t>(&rh[20]) == -104);
        CHECK(wait_for([&] { std::lock_guard lk(disp.mtx); return disp.unlinks == 1; }));
    }

    // --- 6) 客户端断开 → on_detach → 可再次 IMPORT ---
    att.close();
    CHECK(wait_for([&] { std::lock_guard lk(disp.mtx); return disp.detaches == 1; }));
    CHECK(wait_for([&] { return !srv.attached(); }));

    srv.stop();
    WSACleanup();
}

TEST_CASE("stop before import is clean") {
    WSADATA wsa; REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    FakeDispatcher disp;
    UsbIpServer srv;
    REQUIRE(srv.start(0, disp, {make_sim_device()}));
    srv.stop();
    CHECK_FALSE(srv.attached());
    WSACleanup();
}

TEST_SUITE_END();
