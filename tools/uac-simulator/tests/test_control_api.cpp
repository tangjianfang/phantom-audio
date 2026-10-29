// 控制 API 回环测试：全端点往返 + 错误路径
#include <doctest/doctest.h>
#include <uac/control_api.h>
#include <winsock2.h>
#include <cstring>

using namespace uac;

namespace {
struct HttpResult {
    int status;
    std::string body;
};
HttpResult http(std::uint16_t port, const char* method, const char* path, const char* body = "") {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(s != INVALID_SOCKET);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (body && *body) {
        req += "Content-Length: " + std::to_string(strlen(body)) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    if (body && *body) req += body;
    send(s, req.c_str(), static_cast<int>(req.size()), 0);
    std::string resp;
    char buf[4096];
    int r;
    while ((r = recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, static_cast<std::size_t>(r));
    closesocket(s);
    HttpResult out{};
    out.status = std::atoi(resp.substr(9, 3).c_str());
    const auto sep = resp.find("\r\n\r\n");
    out.body = sep == std::string::npos ? "" : resp.substr(sep + 4);
    return out;
}
} // namespace

TEST_CASE("control api endpoints roundtrip") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    ControlCallbacks cb;
    cb.status = [] { return "{\"state\":\"idle\"}"; };
    cb.log_tail = [](std::size_t n) { return "[\"l1\",\"l2\",\"n=" + std::to_string(n) + "\"]"; };
    cb.clear_log = [] {};
    cb.attach = [](const std::string& p) { return "{\"attached\":\"" + p + "\"}"; };
    cb.detach = [] { return "{\"ok\":true}"; };
    cb.event = [](const std::string& e) { return "{\"event\":\"" + e + "\"}"; };
    cb.inject = [](const std::string& b) { return "{\"inj\":" + std::to_string(b.size()) + "}"; };
    cb.source = [](const std::string&) { return "{\"src\":\"ok\"}"; };
    cb.capture = [](const std::string&) { return "{\"cap\":\"ok\"}"; };

    ControlApi api;
    REQUIRE(api.start(0, std::move(cb)));
    const auto port = api.port();
    CHECK(port != 0);

    auto st = http(port, "GET", "/status");
    CHECK(st.status == 200);
    CHECK(st.body == "{\"state\":\"idle\"}");

    auto lg = http(port, "GET", "/log?n=7");
    CHECK(lg.status == 200);
    CHECK(lg.body.find("\"n=7\"") != std::string::npos);

    auto dl = http(port, "DELETE", "/log");
    CHECK(dl.status == 200);

    auto at = http(port, "POST", "/attach", "{\"profile\":\"uac2-48k\"}");
    CHECK(at.status == 200);
    CHECK(at.body == "{\"attached\":\"uac2-48k\"}");

    auto atBad = http(port, "POST", "/attach", "{}");
    CHECK(atBad.status == 400);

    auto dt = http(port, "POST", "/detach");
    CHECK(dt.status == 200);
    CHECK(dt.body == "{\"ok\":true}");

    auto ev = http(port, "POST", "/event", "{\"type\":\"volume_up\"}");
    CHECK(ev.status == 200);
    CHECK(ev.body == "{\"event\":\"volume_up\"}");

    auto inj = http(port, "POST", "/inject", "{\"kind\":\"stall\"}");
    CHECK(inj.status == 200);
    CHECK(inj.body == "{\"inj\":16}");

    auto nf = http(port, "GET", "/nope");
    CHECK(nf.status == 404);

    auto badJson = http(port, "POST", "/event", "not-json");
    CHECK(badJson.status == 400);

    auto missEv = http(port, "POST", "/event", "{}");
    CHECK(missEv.status == 400);

    api.stop();
    WSACleanup();
}

TEST_CASE("control api 501 when callback missing") {
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    ControlApi api;
    REQUIRE(api.start(0, ControlCallbacks{}));
    auto r = http(api.port(), "GET", "/status");
    CHECK(r.status == 501);
    api.stop();
    WSACleanup();
}
