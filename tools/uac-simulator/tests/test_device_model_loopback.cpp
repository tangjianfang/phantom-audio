// M1 端到端回环验收：fake-vhci 客户端 ←→ UsbIpServer ←→ DeviceModel
// 剧本：DEVLIST→IMPORT→枚举描述符→SET_CONFIG→SET_INTERFACE→iso OUT×3→音量 GET/SET→iso IN(pacing)
#include <doctest/doctest.h>
#include <uac/device_model.h>
#include <uac/usbip_server.h>
#include <uac/endian.h>
#include <winsock2.h>
#include <chrono>
#include <array>
#include <algorithm>
#include <cstdint>

using namespace uac;
using namespace uac::usbip;

namespace {
class Client {
public:
    explicit Client(std::uint16_t port) {
        s_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(s_ != INVALID_SOCKET);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(0x7f000001);
        REQUIRE(connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    }
    ~Client() { closesocket(s_); }
    void send(std::span<const unsigned char> b) {
        size_t off = 0;
        while (off < b.size()) {
            int n = ::send(s_, reinterpret_cast<const char*>(b.data() + off),
                           static_cast<int>(b.size() - off), 0);
            REQUIRE(n > 0);
            off += static_cast<size_t>(n);
        }
    }
    SOCKET raw() { return s_; }
    std::vector<unsigned char> recv(std::size_t n) {
        std::vector<unsigned char> buf(n);
        size_t off = 0;
        while (off < n) {
            int r = ::recv(s_, reinterpret_cast<char*>(buf.data() + off),
                           static_cast<int>(n - off), 0);
            if (r <= 0) { buf.resize(off); closesocket(s_); s_ = INVALID_SOCKET; break; }
            off += static_cast<size_t>(r);
        }
        return buf;
    }
    // 便捷：发送 CMD_SUBMIT 并等待对应 RET_SUBMIT（跳过期间到达的其他 seqnum）
    struct Ret { BasicHeader h; RetSubmit r; std::vector<unsigned char> payload; std::vector<IsoPacket> iso; };
    Ret submit_wait(const BasicHeader& b, const CmdSubmit& c,
                    std::span<const unsigned char> out = {}) {
        send(encode_cmd_submit(b, c));
        if (!out.empty()) send(out);
        if (is_iso(c.number_of_packets)) {
            // iso OUT：附加 iso 描述符
            std::vector<unsigned char> raw;
            for (std::uint32_t i = 0; i < c.number_of_packets; ++i) {
                uac::push_be<std::uint32_t>(raw, i * 192);
                uac::push_be<std::uint32_t>(raw, 192);
                uac::push_be<std::uint32_t>(raw, 192);
                uac::push_be<std::int32_t>(raw, 0);
            }
            send(raw);
        }
        // 读 48B 头
        for (;;) {
            auto h = recv(kUrbHeaderSize);
            REQUIRE(h.size() == kUrbHeaderSize);
            BasicHeader bh{uac::rd_be<std::uint32_t>(&h[0]), uac::rd_be<std::uint32_t>(&h[4]),
                           uac::rd_be<std::uint32_t>(&h[8]), uac::rd_be<std::uint32_t>(&h[12]),
                           uac::rd_be<std::uint32_t>(&h[16])};
            REQUIRE(bh.command == RET_SUBMIT);
            RetSubmit rs;
            rs.status = uac::rd_be<std::int32_t>(&h[20]);
            rs.actual_length = uac::rd_be<std::uint32_t>(&h[24]);
            rs.number_of_packets = uac::rd_be<std::uint32_t>(&h[32]);
            Ret r{bh, rs, {}, {}};
            if (r.h.seqnum != b.seqnum) continue; // 乱序跳过
            if (rs.actual_length && b.direction == DIR_IN) r.payload = recv(rs.actual_length);
            if (is_iso(rs.number_of_packets)) {
                auto raw = recv(rs.number_of_packets * kIsoDescSize);
                r.iso = decode_iso_descs(raw);
            }
            return r;
        }
    }

private:
    SOCKET s_ = INVALID_SOCKET;
};

BasicHeader hdr(std::uint32_t seq, std::uint32_t dir, std::uint32_t ep) {
    return BasicHeader{CMD_SUBMIT, seq, 0x00010001, dir, ep};
}
CmdSubmit ctrl(std::array<std::uint8_t, 8> setup, std::uint16_t len) {
    CmdSubmit c{0, len, -1, 0, 0, {}};
    std::copy(setup.begin(), setup.end(), c.setup);
    return c;
}
template <class F>
bool wait_for(F&& f, int ms = 3000) {
    for (int i = 0; i != ms / 10; ++i) {
        if (f()) return true;
        Sleep(10);
    }
    return f();
}
} // namespace

TEST_CASE("M1 loopback: full enumeration + control + iso streams") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    ManualClock clock;
    DeviceModel model(make_uac1_default_snapshot(), std::make_unique<ManualClock>());
    // 用同一个 ManualClock 注入：DeviceModel 构造已接收；再取引用推进时间
    auto* mc = dynamic_cast<ManualClock*>(&model.clock());
    REQUIRE(mc != nullptr);

    UsbIpServer srv;
    REQUIRE(srv.start(0, model, {model.usbip_device()}));
    const auto port = srv.port();

    Client c(port);
    // Client-side receive timeout: the strict "exactly 2 RETs" contract
    // otherwise blocks forever if a wrap raced (recv blocks on an open socket
    // that will never deliver more data).
    DWORD tmo = 2000;
    setsockopt(c.raw(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo),
               sizeof(tmo));
    // IMPORT
    {
        std::vector<unsigned char> pkt;
        uac::push_be<std::uint16_t>(pkt, kVersion);
        uac::push_be<std::uint16_t>(pkt, OP_REQ_IMPORT);
        uac::push_be<std::uint32_t>(pkt, 0);
        pkt.insert(pkt.end(), {'1', '-', '1', 0});
        pkt.resize(40, 0);
        c.send(pkt);
        auto h = c.recv(8);
        REQUIRE(h.size() == 8);
        CHECK(uac::rd_be<std::uint16_t>(&h[2]) == OP_REP_IMPORT);
        CHECK(uac::rd_be<std::uint32_t>(&h[4]) == 0);
        c.recv(kUsbIpDeviceSize);
    }
    CHECK(wait_for([&] { return model.attached(); }));

    // 枚举：device → config → string
    auto d = c.submit_wait(hdr(1, DIR_IN, 0), ctrl({0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0}, 18));
    REQUIRE(d.r.status == 0);
    REQUIRE(d.payload.size() == 18);
    CHECK(d.payload[0] == 0x12);
    CHECK(d.payload[8] == 0x8C); // C-Media VID low byte? —— 用默认 profile: 0D8C → bytes 8C 0D
    auto cfg = c.submit_wait(hdr(2, DIR_IN, 0), ctrl({0x80, 0x06, 0x00, 0x02, 0, 0, 0xFF, 0}, 255));
    REQUIRE(cfg.r.status == 0);
    CHECK(cfg.payload.size() > 150);
    auto pv = desc::parse_config(cfg.payload);
    REQUIRE(pv.has_value());
    CHECK(pv->bNumInterfaces == 3);
    // UDE 适配后的端点 interval >= 4
    for (auto&& e : pv->endpoints) CHECK(e.interval >= 4);

    // SET_CONFIGURATION(1)
    auto sc = c.submit_wait(hdr(3, DIR_OUT, 0), ctrl({0x00, 0x09, 0x01, 0x00, 0, 0, 0, 0}, 0));
    CHECK(sc.r.status == 0);

    // SET_INTERFACE(播放 IF1 alt1)
    auto si = c.submit_wait(hdr(4, DIR_OUT, 0), ctrl({0x01, 0x0B, 0x01, 0x00, 0x01, 0x00, 0, 0}, 0));
    CHECK(si.r.status == 0);
    CHECK(model.interface_alt(1) == 1);

    // iso OUT ×3（10 包 × 192B）—— 立即 RET（OUT 无 pacing）
    for (int k = 0; k < 3; ++k) {
        CmdSubmit co{0, 1920, -1, 10, 1, {}};
        auto r = c.submit_wait(hdr(10 + k, DIR_OUT, 6), co,
                               std::vector<unsigned char>(1920, 0x55));
        CHECK(r.r.status == 0);
        CHECK(r.r.actual_length == 1920);
    }

    // 音量 SET_CUR ch1 = -10dB → GET_CUR 一致
    auto sv = c.submit_wait(hdr(20, DIR_OUT, 0),
                            ctrl({0x21, 0x01, 0x01, 0x02, 0x00, 0x02, 2, 0}, 2) /*CN=1 CS=VOL; IF=0 FU=2*/,
                            std::vector<unsigned char>{0x00, 0xF6}); // -2560 = 0xF600
    CHECK(sv.r.status == 0);
    auto gv = c.submit_wait(hdr(21, DIR_IN, 0), ctrl({0xA1, 0x81, 0x01, 0x02, 0x00, 0x02, 2, 0}, 2));
    REQUIRE(gv.r.status == 0);
    CHECK(gv.payload.size() == 2);
    CHECK(gv.payload[0] == 0x00);
    CHECK(gv.payload[1] == 0xF6);

    // iso IN ×2（麦克风，10ms pacing）—— ManualClock 推进后 RET
    CmdSubmit ci{0, 1920, -1, 10, 1, {}};
    std::vector<unsigned char> isoDescs;
    for (int i = 0; i < 10; ++i) {
        uac::push_be<std::uint32_t>(isoDescs, i * 192);
        uac::push_be<std::uint32_t>(isoDescs, 192);
        uac::push_be<std::uint32_t>(isoDescs, 192);
        uac::push_be<std::int32_t>(isoDescs, 0);
    }
    c.send(encode_cmd_submit(hdr(30, DIR_IN, 6), ci));
    c.send(isoDescs);
    c.send(encode_cmd_submit(hdr(31, DIR_IN, 6), ci));
    c.send(isoDescs);
    mc->advance_to(9999);
    model.pump(); // 未到期：无 RET（pacing 语义由单测覆盖）
    // 通道线程读 submit 与测试推进时钟存在竞争：submit31 被读到时时钟可能
    // 已在 20000+，其 deadline=30000。按视野推进并逐轮泵送，直到凑齐 2 RET。
    for (std::uint64_t t = 10000; t <= 60000; t += 5000) {
        mc->advance_to(t);
        model.pump();
        // 若两个 RET 已到，读循环会立刻完成；否则继续推进
        // （无阻塞探测：c.recv 在有数据时立即返回）
        // noop probe below
        if (t >= 20000) Sleep(5); // give the wrap-sends a moment
    }
    // 读取两个 RET（RET 到达时刻与发送方线程竞争：c.recv 可能读到 0 字节
    // —— 挂起等待是 TcpClient::recv 的阻塞语义，但 Windows 下对端尚未
    // send 时 recv 可能立即返回空；短轮询直到凑齐 48B 头）
    int got = 0;
    int emptyReads = 0;
    while (got < 2) {
        auto h = c.recv(kUrbHeaderSize);
        if (h.empty()) {
            if (++emptyReads > 3) { FAIL("no more RET data (timeout/close)"); }
            Sleep(10);
            continue;
        }
        REQUIRE(h.size() == kUrbHeaderSize);
        REQUIRE(uac::rd_be<std::uint32_t>(&h[0]) == RET_SUBMIT);
        auto alen = uac::rd_be<std::uint32_t>(&h[24]);
        auto npk = uac::rd_be<std::uint32_t>(&h[32]);
        if (alen) c.recv(alen);
        if (is_iso(npk)) c.recv(npk * kIsoDescSize);
        CHECK(npk == 10);
        ++got;
    }

    // 日志断言：枚举+控制全量记录
    CHECK(model.log().count() >= 6); // 2 描述符 + SET_CFG + SET_IF + 2 音量
    CHECK(model.processor().request_count() >= 5); // AcProcessor 收到的 EP0 类/标准请求

    srv.stop();
    WSACleanup();
}
