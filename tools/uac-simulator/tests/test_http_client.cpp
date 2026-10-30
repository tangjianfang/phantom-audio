// GUI 用的 HTTP 客户端：任何一次调用的最坏耗时都必须有上界。
// 起因：Windows 上阻塞式 connect() 连没人监听的回环端口要等约 2.0 秒；GUI 在 UI 线程上按定时器同步发请求，
// 没有 uacsimd 时界面就被钉死（Sandbox 里实际发生过）。
#include <doctest/doctest.h>
#include <http_client.h>
#include <uac/control_api.h>
#include <chrono>

using namespace uacstudio;
using Clock = std::chrono::steady_clock;

namespace {

long long ms_since(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// 取一个当前没人监听的端口：绑定到端口 0，读出系统分配的端口，再关掉
std::uint16_t closed_port() {
    wsa_ensure_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    a.sin_port = 0;
    REQUIRE(bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    int len = sizeof(a);
    REQUIRE(getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    closesocket(s);
    return ntohs(a.sin_port);
}

} // namespace

TEST_CASE("http client: a closed port fails fast instead of blocking ~2 s") {
    const auto port = closed_port();
    for (int i = 0; i < 3; ++i) {
        const auto t0 = Clock::now();
        const auto r = http_req(port, "GET", "/status", "");
        const auto ms = ms_since(t0);
        INFO("attempt " << i << " took " << ms << " ms");
        CHECK(r.empty());
        CHECK(ms < 1200); // 实测：旧的阻塞式 connect 在这台机器上每次 2020-2120 ms；现在上限 300 ms 加开销
    }
}

TEST_CASE("http client: the connect deadline is honoured when asked for a shorter one") {
    const auto port = closed_port();
    const auto t0 = Clock::now();
    CHECK(http_req(port, "GET", "/status", "", "", /*connectTimeoutMs=*/100).empty());
    CHECK(ms_since(t0) < 900);
}

TEST_CASE("http client: a live server is still reached through the non-blocking connect") {
    uac::ControlCallbacks cb;
    cb.status = [] { return std::string(R"({"running":true})"); };
    cb.detach = [] { return std::string(R"({"ok":true})"); };
    uac::ControlApi api;
    REQUIRE(api.start(0, cb));
    const auto t0 = Clock::now();
    CHECK(http_req(api.port(), "GET", "/status", "") == R"({"running":true})");
    std::string out;
    CHECK(http_post(api.port(), "/detach", "", &out));
    CHECK(out == R"({"ok":true})");
    CHECK(ms_since(t0) < 1500); // 本机有服务时应当几乎瞬时，不能因为新的超时机制变慢
}

TEST_CASE("http client: a server that accepts but never answers is bounded by the I/O timeout") {
    wsa_ensure_init();
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    a.sin_port = 0;
    REQUIRE(bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    REQUIRE(listen(ls, 4) == 0); // 不 accept：握手由内核完成，请求发出去后永远没有应答
    int len = sizeof(a);
    REQUIRE(getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    const auto t0 = Clock::now();
    const auto r = http_req(ntohs(a.sin_port), "GET", "/status", "", "", kConnectTimeoutMs, /*ioTimeoutMs=*/300);
    CHECK(r.empty());
    CHECK(ms_since(t0) < 1500);
    closesocket(ls);
}
