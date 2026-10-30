// uacstudio 共用：极简 HTTP 客户端（127.0.0.1 控制 API）
//
// 所有网络调用都有截止时间——这是 GUI 不卡死的前提：
//   Windows 上阻塞式 connect() 连一个没人监听的回环端口要等约 2.0 秒才返回（RST 之后协议栈还会重发 SYN，
//   实测 2.02-2.12 s）。GUI 的定时器如果在 UI 线程上同步发请求，Sandbox 里没有 uacsimd 时 UI 线程几乎一直在阻塞，
//   窗口拖不动、点不了。所以连接用非阻塞 + select 的短截止时间（本机有服务时握手远小于 1 ms，300 ms 不会误伤），
//   读写再加超时，保证任何一次调用的最坏耗时都有上界。
// 即便如此，GUI 也不应在 UI 线程上调用本文件的函数——见 main.cpp 的后台轮询线程与请求线程。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN // uac_core 以编译定义 =1 导出它；无条件重定义会触发 C4005（/WX 下为错误）
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
#include <uac/json_util.h>
#include <string>

namespace uacstudio {

inline constexpr int kConnectTimeoutMs = 300; // 本机握手 < 1 ms；超过 300 ms 就当不可达
inline constexpr int kIoTimeoutMs = 2000;     // 单次 send / recv 的上限（控制 API 串行处理，偶尔要排队）

// Winsock self-initialization: safe for every consumer (GUI/runner/CLI),
// so callers never need their own WSAStartup just to reach the local API.
inline void wsa_ensure_init() {
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}

// 带截止时间的回环连接；失败返回 INVALID_SOCKET（调用方不用关）。成功时套接字已恢复为阻塞模式并设好读写超时。
inline SOCKET connect_loopback(std::uint16_t port, int connectTimeoutMs, int ioTimeoutMs) {
    wsa_ensure_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001);

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { // 立即失败
            closesocket(s);
            return INVALID_SOCKET;
        }
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval tv{connectTimeoutMs / 1000, (connectTimeoutMs % 1000) * 1000};
        // 可写 = 连上了；在 exceptfds 里 = 被拒；超时 = 当作不可达（协议栈的 SYN 重发留给它自己，我们不等）
        if (select(0, nullptr, &writable, &failed, &tv) <= 0 || FD_ISSET(s, &failed) || !FD_ISSET(s, &writable)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    const DWORD tmo = static_cast<DWORD>(ioTimeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    return s;
}

// extraHeaders：每行以 "\r\n" 结尾的完整头（如 "Origin: http://x\r\n"），仅供测试
// 构造跨域请求以验证服务端拒绝；正常调用留空。
// 返回响应体；连接失败 / 超时 / 没有完整响应头 -> 空串。
inline std::string http_req(std::uint16_t port, const char* method, const std::string& path,
                            const std::string& body, const std::string& extraHeaders = "",
                            int connectTimeoutMs = kConnectTimeoutMs, int ioTimeoutMs = kIoTimeoutMs) {
    SOCKET s = connect_loopback(port, connectTimeoutMs, ioTimeoutMs);
    if (s == INVALID_SOCKET) return "";
    // Host 必须是回环地址：服务端用它防 DNS 重绑定
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                      std::to_string(port) + "\r\n" + extraHeaders;
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
