// uacsim —— 仿真器命令行客户端（走控制 API）
// 用法：uacsim.exe [--host 127.0.0.1:3241] <command> [args]
//   status / attach <profile> / detach / event <type> / inject <json> / source <json>
//   capture <path|-> / log [n]
#include <uac/json_util.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string http_post_get(const std::string& host, std::uint16_t port, const char* method,
                          const std::string& path, const std::string& body) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return "{\"error\":\"socket\"}";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    // 服务端只监听 127.0.0.1；"localhost" 按回环解析，其它名字不做 DNS 解析
    const char* ip = host == "localhost" ? "127.0.0.1" : host.c_str();
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        closesocket(s);
        return "{\"error\":\"bad --host (IPv4 address or localhost expected)\"}";
    }
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return "{\"error\":\"connect failed - is uacsimd running?\"}";
    }
    // Host 头如实带上目标地址：服务端会拒绝非回环 Host（防 DNS 重绑定）
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                      std::to_string(port) + "\r\n";
    if (!body.empty()) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n" + body;
    send(s, req.c_str(), static_cast<int>(req.size()), 0);
    std::string resp;
    char buf[8192];
    int r;
    while ((r = recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, static_cast<std::size_t>(r));
    closesocket(s);
    const auto sep = resp.find("\r\n\r\n");
    return sep == std::string::npos ? resp : resp.substr(sep + 4);
}
} // namespace

int main(int argc, char** argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::string host = "127.0.0.1";
    std::uint16_t port = 3241;
    int i = 1;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) {
            const std::string h = argv[++i];
            const auto c = h.rfind(':');
            if (c != std::string::npos && h.find(':') == c) {
                host = h.substr(0, c);
                port = static_cast<std::uint16_t>(atoi(h.substr(c + 1).c_str()));
            } else host = h;
        } else break;
    }
    if (i >= argc) {
        printf("usage: uacsim [--host 127.0.0.1:3241] <status|attach|detach|shutdown|event|inject|source|capture|log> [args]\n");
        return 2;
    }
    const std::string cmd = argv[i++];
    std::string out;
    if (cmd == "status") out = http_post_get(host, port, "GET", "/status", "");
    else if (cmd == "attach" && i < argc)
        out = http_post_get(host, port, "POST", "/attach",
                            std::string("{\"profile\":\"") + uac::JsonEscape(argv[i]) + "\"}");
    else if (cmd == "detach") out = http_post_get(host, port, "POST", "/detach", "");
    else if (cmd == "shutdown") out = http_post_get(host, port, "POST", "/shutdown", ""); // stops uacsimd itself
    else if (cmd == "event" && i < argc)
        out = http_post_get(host, port, "POST", "/event",
                            std::string("{\"type\":\"") + argv[i] + "\"}");
    else if (cmd == "inject" && i < argc)
        out = http_post_get(host, port, "POST", "/inject", argv[i]);
    else if (cmd == "source" && i < argc)
        out = http_post_get(host, port, "POST", "/source", argv[i]);
    else if (cmd == "capture")
        out = http_post_get(host, port, "POST", "/capture",
                            i < argc && std::string(argv[i]) != "-"
                                ? std::string("{\"path\":\"") + uac::JsonEscape(argv[i]) + "\"}"
                                : "{\"path\":\"\"}");
    else if (cmd == "log")
        out = http_post_get(host, port, "GET",
                            std::string("/log?n=") + (i < argc ? argv[i] : "50"), "");
    else {
        printf("unknown command: %s\n", cmd.c_str());
        return 2;
    }
    printf("%s\n", out.c_str());
    return 0;
}
