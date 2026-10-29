// uacstudio 共用：极简 HTTP 客户端（127.0.0.1 控制 API）
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
#include <string>

namespace uacstudio {

// Winsock self-initialization: safe for every consumer (GUI/runner/CLI),
// so callers never need their own WSAStartup just to reach the local API.
inline void wsa_ensure_init() {
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}

inline std::string http_req(std::uint16_t port, const char* method, const std::string& path,
                            const std::string& body) {
    wsa_ensure_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return "";
    }
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: t\r\n";
    if (!body.empty()) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n" + body;
    send(s, req.c_str(), static_cast<int>(req.size()), 0);
    std::string resp;
    char buf[8192];
    int r;
    while ((r = recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, static_cast<std::size_t>(r));
    closesocket(s);
    const auto sep = resp.find("\r\n\r\n");
    return sep == std::string::npos ? "" : resp.substr(sep + 4);
}

inline bool http_post(std::uint16_t port, const std::string& path, const std::string& body,
                      std::string* out = nullptr) {
    const auto r = http_req(port, "POST", path, body);
    if (out) *out = r;
    return !r.empty();
}

} // namespace uacstudio
