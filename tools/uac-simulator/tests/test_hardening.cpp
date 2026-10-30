// 本地服务加固的回归测试：跨域/DNS 重绑定、请求体上限、空闲连接不得卡死 API 与 USB/IP 握手。
// 每条拒绝规则都配一条"同样的请求去掉违规点 → 必须通过"的对照，避免测试恒真。
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <sim_host.h>
#include <uac/control_api.h>
#include <uac/endian.h>
#include <uac/usbip_server.h>
#include <winsock2.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

struct Wsa {
    Wsa() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~Wsa() { WSACleanup(); }
};

SOCKET connect_local(std::uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(s != INVALID_SOCKET);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    return s;
}

void send_str(SOCKET s, const std::string& b) {
    std::size_t off = 0;
    while (off < b.size()) {
        const int n = ::send(s, b.data() + off, static_cast<int>(b.size() - off), 0);
        if (n <= 0) return; // 对端已关闭（如已回 4xx）：不是测试失败
        off += static_cast<std::size_t>(n);
    }
}

// 读到对端关闭、出错或超时，返回已收到的字节
std::string read_all(SOCKET s, int timeoutMs) {
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    std::string out;
    char buf[4096];
    for (;;) {
        const int r = ::recv(s, buf, sizeof(buf), 0);
        if (r <= 0) break;
        out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
}

// true = 对端在 timeoutMs 内关闭了连接（recv 返回 0）；false = 超时仍开着
bool closed_by_peer(SOCKET s, int timeoutMs) {
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    char c;
    const int r = ::recv(s, &c, 1, 0);
    if (r == 0) return true;
    return r < 0 && WSAGetLastError() != WSAETIMEDOUT; // RST 也算被关闭
}

int status_of(const std::string& resp) {
    return resp.size() >= 12 ? std::atoi(resp.substr(9, 3).c_str()) : 0;
}

std::string roundtrip(std::uint16_t port, const std::string& req, int timeoutMs = 3000) {
    SOCKET s = connect_local(port);
    send_str(s, req);
    auto r = read_all(s, timeoutMs);
    closesocket(s);
    return r;
}

std::string post(const std::string& path, const std::string& headers, const std::string& body) {
    return "POST " + path + " HTTP/1.1\r\n" + headers + "Content-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}
std::string get(const std::string& path, const std::string& headers = "Host: 127.0.0.1\r\n") {
    return "GET " + path + " HTTP/1.1\r\n" + headers + "Connection: close\r\n\r\n";
}

struct Counting {
    std::atomic<int> injects{0}, statuses{0}, shutdowns{0};
    std::atomic<std::size_t> lastBodySize{0};
    uac::ControlCallbacks callbacks() {
        uac::ControlCallbacks cb;
        cb.status = [this] { ++statuses; return std::string("{\"running\":true}"); };
        cb.shutdown = [this] { ++shutdowns; return std::string("{\"ok\":true,\"shutdown\":true}"); };
        cb.inject = [this](const std::string& b) {
            ++injects;
            lastBodySize = b.size();
            return std::string("{\"ok\":true}");
        };
        return cb;
    }
};

} // namespace

// ---------------------------------------------------------------- 跨域 / Host

TEST_CASE("control api: cross-origin requests never reach a handler") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    REQUIRE(api.start(0, c.callbacks()));
    const auto port = api.port();
    const std::string body = "{\"kind\":\"clear\"}";

    // 浏览器跨域 POST 的形态：text/plain（免预检）+ Origin
    auto r = roundtrip(port, post("/inject", "Host: 127.0.0.1\r\nOrigin: http://evil.example\r\n"
                                              "Content-Type: text/plain\r\n", body));
    CHECK(status_of(r) == 403);
    CHECK(r.find("cross-origin") != std::string::npos);
    CHECK(c.injects == 0);

    // 头名大小写不敏感；Origin: null（沙箱 iframe / file://）同样拒绝
    r = roundtrip(port, post("/inject", "host: 127.0.0.1\r\norigin: null\r\n", body));
    CHECK(status_of(r) == 403);
    CHECK(c.injects == 0);

    // 对照：去掉 Origin，同一请求必须成功——否则上面的 403 可能只是别的原因
    r = roundtrip(port, post("/inject", "Host: 127.0.0.1\r\nContent-Type: text/plain\r\n", body));
    CHECK(status_of(r) == 200);
    CHECK(c.injects == 1);
}

// POST /shutdown stops the simulator process, so it is the one endpoint where "a web page could reach it" matters most.
TEST_CASE("control api: /shutdown works for local clients and never for a web page") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    REQUIRE(api.start(0, c.callbacks()));
    const auto port = api.port();

    // A browser form POST (Origin set) and a DNS-rebinding request (foreign Host): refused, handler not called
    auto r = roundtrip(port, post("/shutdown", "Host: 127.0.0.1\r\nOrigin: http://evil.example\r\n"
                                                "Content-Type: text/plain\r\n", ""));
    CHECK(status_of(r) == 403);
    r = roundtrip(port, post("/shutdown", "Host: evil.example\r\n", ""));
    CHECK(status_of(r) == 403);
    r = roundtrip(port, get("/shutdown")); // wrong method
    CHECK(status_of(r) == 404);
    CHECK(c.shutdowns == 0);

    // Control: the same request without the offending header goes through
    r = roundtrip(port, post("/shutdown", "Host: 127.0.0.1\r\n", ""));
    CHECK(status_of(r) == 200);
    CHECK(r.find("\"shutdown\":true") != std::string::npos);
    CHECK(c.shutdowns == 1);
}

TEST_CASE("control api: /shutdown without a handler is 'not supported', not a crash") {
    Wsa w;
    uac::ControlCallbacks cb;
    cb.status = [] { return std::string("{}"); };
    uac::ControlApi api;
    REQUIRE(api.start(0, std::move(cb)));
    const auto r = roundtrip(api.port(), post("/shutdown", "Host: 127.0.0.1\r\n", ""));
    CHECK(status_of(r) == 501);
}

TEST_CASE("control api: Host must be loopback (DNS rebinding)") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    REQUIRE(api.start(0, c.callbacks()));
    const auto port = api.port();

    for (const char* host : {"evil.example", "evil.example:3241", "127.0.0.1.evil.example",
                             "127.0.0.1:", "127.0.0.1:abc", "192.168.1.5:3241", "[::1]:3241", ""}) {
        INFO("Host=[" << host << "]");
        const auto r = roundtrip(port, get("/status", std::string("Host: ") + host + "\r\n"));
        CHECK(status_of(r) == 403);
    }
    CHECK(c.statuses == 0);

    for (const char* host : {"127.0.0.1", "127.0.0.1:3241", "localhost", "LocalHost:8080"}) {
        INFO("Host=[" << host << "]");
        const auto r = roundtrip(port, get("/status", std::string("Host: ") + host + "\r\n"));
        CHECK(status_of(r) == 200);
    }
    CHECK(c.statuses == 4);

    // 没有 Host 头（原始 socket 客户端）放行：浏览器不可能省略 Host
    CHECK(status_of(roundtrip(port, get("/status", ""))) == 200);
}

// ---------------------------------------------------------------- 请求体

TEST_CASE("control api: Content-Length header is case-insensitive") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    REQUIRE(api.start(0, c.callbacks()));
    const std::string body = "{\"kind\":\"stall\"}";
    // 小写 content-length：此前按大小写敏感查找，体会被读成 0 字节
    const std::string req = "POST /inject HTTP/1.1\r\nHost: 127.0.0.1\r\ncontent-length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    CHECK(status_of(roundtrip(api.port(), req)) == 200);
    CHECK(c.lastBodySize == body.size());
}

TEST_CASE("control api: oversized body and chunked encoding are refused") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    REQUIRE(api.start(0, c.callbacks()));
    const auto port = api.port();

    // 只发头，声明 2 MiB：服务端不应去读，直接 413
    auto r = roundtrip(port, "POST /inject HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2097152"
                             "\r\nConnection: close\r\n\r\n");
    CHECK(status_of(r) == 413);
    CHECK(c.injects == 0);

    r = roundtrip(port, "POST /inject HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n"
                        "Connection: close\r\n\r\n0\r\n\r\n");
    CHECK(status_of(r) == 501);
    CHECK(c.injects == 0);

    // 对照：正常大小的体不被拒绝，且确实到达处理器
    CHECK(status_of(roundtrip(port, post("/inject", "Host: 127.0.0.1\r\n", "{}"))) == 200);
    CHECK(c.injects == 1);
}

TEST_CASE("control api: oversized head does not wedge the server") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    REQUIRE(api.start(0, c.callbacks()));
    const auto port = api.port();
    SOCKET s = connect_local(port);
    send_str(s, std::string(70000, 'A')); // 没有 \r\n\r\n 的超长头
    const auto r = read_all(s, 2000);
    closesocket(s);
    // 服务端可能回 431，也可能因未读完的数据 RST（响应丢失）——两者都可接受；不可接受的是之后不再服务
    if (!r.empty()) CHECK(status_of(r) == 431);
    CHECK(status_of(roundtrip(port, get("/status"))) == 200);
}

// ---------------------------------------------------------------- 空闲连接

TEST_CASE("control api: an idle connection times out and does not block other clients") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    api.set_request_timeout_ms(300);
    REQUIRE(api.start(0, c.callbacks()));
    const auto port = api.port();

    SOCKET idle = connect_local(port); // 连上不发数据（浏览器预连接 / 端口扫描 / 半开连接）
    const auto t0 = Clock::now();
    const auto r = roundtrip(port, get("/status")); // 排在 idle 之后，须在时限后得到服务
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    CHECK(status_of(r) == 200);
    CHECK(waited.count() < 3000); // 修复前：永远等不到（实测 4 秒超时）

    const auto idleResp = read_all(idle, 2000);
    closesocket(idle);
    CHECK(status_of(idleResp) == 408);
}

TEST_CASE("control api: slow-loris head and stalled body both time out with 408") {
    Wsa w;
    Counting c;
    uac::ControlApi api;
    api.set_request_timeout_ms(300);
    REQUIRE(api.start(0, c.callbacks()));
    const auto port = api.port();

    SOCKET a = connect_local(port);
    send_str(a, "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n"); // 头没结束
    CHECK(status_of(read_all(a, 2000)) == 408);
    closesocket(a);

    SOCKET b = connect_local(port);
    send_str(b, "POST /inject HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100\r\n\r\n12345");
    CHECK(status_of(read_all(b, 2000)) == 408);
    closesocket(b);
    CHECK(c.injects == 0); // 体不完整的请求不得触发处理器

    CHECK(status_of(roundtrip(port, get("/status"))) == 200); // 之后服务正常
}

// ---------------------------------------------------------------- SimHost 端到端（HTTP 接线）

TEST_CASE("sim host over HTTP: attach name filter and capture path policy are wired in") {
    Wsa w;
    uac::SimHost host(std::string(UAC_SOURCE_DIR) + "/profiles");
    REQUIRE(host.start(0, 0));
    const auto port = host.http_port();
    REQUIRE(port != 0);
    const auto h = std::string("Host: 127.0.0.1\r\n");

    // 主机名刻意不带 '.'：带点号的名字会被旧黑名单的 '.' 误拦，测不到反斜杠这条漏洞路径。
    // 用 localhost 而不是随意的主机名：过滤一旦回退，fs::exists 会去访问这个 UNC 路径，
    // 回环地址立即失败，不会向局域网发解析/连接请求。
    const std::string bs2(2, static_cast<char>(0x5C));
    const auto unc = json{{"profile", bs2 + "localhost" + char(0x5C) + "share" + char(0x5C) + "x"}};
    auto r = roundtrip(port, post("/attach", h, unc.dump()));
    CHECK(status_of(r) == 200);
    CHECK(r.find("invalid profile name") != std::string::npos);

    r = roundtrip(port, post("/attach", h, json{{"profile", "uac2-48k"}}.dump()));
    CHECK(r.find("\"attached\"") != std::string::npos);

    r = roundtrip(port, post("/capture", h, json{{"path", "notes.txt"}}.dump()));
    CHECK(r.find("invalid capture path") != std::string::npos);

    // 同一请求带 Origin：到不了 SimHost
    r = roundtrip(port, post("/attach", h + "Origin: http://evil.example\r\n",
                             json{{"profile", "uac2-48k"}}.dump()));
    CHECK(status_of(r) == 403);
    host.stop();
}

// ---------------------------------------------------------------- USB/IP 握手

namespace {

struct NullDispatcher final : uac::UrbDispatcher {
    std::atomic<int> imports{0}, detaches{0};
    void on_import(const uac::usbip::UsbIpDevice&) override { ++imports; }
    void on_urb(const uac::usbip::UrbCommand&, std::span<const unsigned char>,
                std::span<const uac::usbip::IsoPacket>, uac::CompletionFn) override {}
    void on_unlink(std::uint32_t, std::uint32_t, uac::UnlinkResultFn done) override { done(0); }
    void on_detach() override { ++detaches; }
};

uac::usbip::UsbIpDevice null_device() {
    uac::usbip::UsbIpDevice d;
    d.path = "/uacsim/0";
    d.busid = "1-1";
    d.busnum = 1;
    d.devnum = 1;
    d.speed = 3;
    d.idVendor = 0x1D50;
    d.idProduct = 0x6222;
    d.bNumInterfaces = 0;
    return d;
}

std::string usbip_op(std::uint16_t code, const std::string& body = "") {
    std::vector<unsigned char> pkt;
    uac::push_be<std::uint16_t>(pkt, uac::usbip::kVersion);
    uac::push_be<std::uint16_t>(pkt, code);
    uac::push_be<std::uint32_t>(pkt, 0);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return std::string(pkt.begin(), pkt.end());
}

std::string import_body() { return std::string("1-1") + std::string(29, '\0'); } // 32 字节 busid

} // namespace

TEST_CASE("usbip server: an idle control connection is dropped and does not block DEVLIST") {
    Wsa w;
    NullDispatcher disp;
    uac::UsbIpServer server;
    server.set_control_idle_timeout_ms(300);
    REQUIRE(server.start(0, disp, {null_device()}));

    SOCKET idle = connect_local(server.port()); // 握手阶段不发数据
    const auto t0 = Clock::now();
    SOCKET other = connect_local(server.port());
    send_str(other, usbip_op(uac::usbip::OP_REQ_DEVLIST));
    const auto reply = read_all(other, 3000);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    closesocket(other);
    REQUIRE(reply.size() >= 8);
    CHECK(uac::rd_be<std::uint16_t>(reinterpret_cast<const unsigned char*>(&reply[2])) == 0x0005);
    CHECK(waited.count() < 3000); // 修复前：被 idle 连接串行阻塞，等不到应答

    CHECK(closed_by_peer(idle, 2000));
    closesocket(idle);
    server.stop();
}

TEST_CASE("usbip server: a half-sent header is dropped after the idle limit") {
    Wsa w;
    NullDispatcher disp;
    uac::UsbIpServer server;
    server.set_control_idle_timeout_ms(300);
    REQUIRE(server.start(0, disp, {null_device()}));

    SOCKET s = connect_local(server.port());
    send_str(s, usbip_op(uac::usbip::OP_REQ_DEVLIST).substr(0, 4)); // 8 字节头只发 4 字节
    CHECK(closed_by_peer(s, 2000));
    closesocket(s);

    // 对照：同一连接上正常发完整头则不会被关
    SOCKET ok = connect_local(server.port());
    send_str(ok, usbip_op(uac::usbip::OP_REQ_DEVLIST));
    CHECK(read_all(ok, 500).size() >= 8);
    closesocket(ok);
    server.stop();
}

TEST_CASE("usbip server: repeated import/disconnect cycles keep working and stop is prompt") {
    Wsa w;
    NullDispatcher disp;
    uac::UsbIpServer server;
    REQUIRE(server.start(0, disp, {null_device()}));
    constexpr int kCycles = 15;

    for (int i = 0; i < kCycles; ++i) {
        INFO("cycle " << i);
        SOCKET s = connect_local(server.port());
        send_str(s, usbip_op(uac::usbip::OP_REQ_IMPORT, import_body()));
        char hdr[8];
        DWORD tv = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        int got = 0;
        while (got < 8) {
            const int r = ::recv(s, hdr + got, 8 - got, 0);
            if (r <= 0) break;
            got += r;
        }
        REQUIRE(got == 8);
        CHECK(uac::rd_be<std::uint32_t>(reinterpret_cast<const unsigned char*>(hdr) + 4) == 0); // status OK
        closesocket(s); // 设备拔出
        for (int w2 = 0; w2 < 200 && disp.detaches.load() < i + 1; ++w2)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(disp.detaches.load() == i + 1); // 通道线程确实走完了收尾
    }
    CHECK(disp.imports.load() == kCycles);

    const auto t0 = Clock::now();
    server.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(ms < 2000);
}
