// 控制台 API 实现：极简 HTTP/1.1 + JSON 路由（仅 127.0.0.1）
#include <uac/control_api.h>
#include <uac/net_util.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <thread>

namespace {
std::once_flag g_wsa_once;
void wsa_init_once() {
    std::call_once(g_wsa_once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}
} // namespace

namespace uac {

namespace {

constexpr std::size_t kMaxHeadBytes = 65536;
constexpr std::size_t kMaxBodyBytes = 1u << 20; // 1 MiB：控制 API 的 JSON 体远小于此

bool send_all(SOCKET s, const char* p, std::size_t n) {
    return net::send_all(s, std::span<const unsigned char>(
                                reinterpret_cast<const unsigned char*>(p), n));
}

const char* reason_phrase(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    default: return "Internal Server Error";
    }
}

std::string http_response(int code, const std::string& body) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason_phrase(code) +
                    "\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    return h + body;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

// 头名大小写不敏感（RFC 9110）。head = 请求行 + 各头行（不含结尾空行）。
std::optional<std::string> header_value(const std::string& head, std::string_view name) {
    std::size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        pos += 2; // 跳过 CRLF，到下一行行首
        const std::size_t end = head.find("\r\n", pos);
        const std::string_view line(head.data() + pos,
                                    (end == std::string::npos ? head.size() : end) - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && iequals(line.substr(0, colon), name)) {
            auto v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
            return std::string(v);
        }
        pos = end;
    }
    return std::nullopt;
}

// Host 头形如 "127.0.0.1"、"127.0.0.1:3241"、"localhost"、"localhost:3241"。
bool is_loopback_host(const std::string& host) {
    std::string_view h = host;
    if (const auto colon = h.rfind(':'); colon != std::string_view::npos) {
        const auto port = h.substr(colon + 1);
        if (port.empty()) return false;
        for (const char c : port)
            if (c < '0' || c > '9') return false;
        h = h.substr(0, colon);
    }
    return iequals(h, "127.0.0.1") || iequals(h, "localhost");
}

} // namespace

struct ControlApi::Impl {
    std::atomic<bool> stopping{false};
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<int> requestTimeoutMs{5000};
    std::thread acceptThread;
    ControlCallbacks cb;

    enum class Read { Ok, Closed, Deadline };

    // 读入一批字节追加到 acc。250ms 的 SO_RCVTIMEO 只是让循环能周期性检查
    // stopping 与总时限——空闲但不关闭的对端到时限就被放弃，不会永久占住串行的 accept 线程。
    Read read_some(SOCKET s, std::string& acc,
                   std::chrono::steady_clock::time_point deadline) {
        char buf[2048];
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) return Read::Deadline;
            const int r = ::recv(s, buf, sizeof(buf), 0);
            if (r > 0) {
                acc.append(buf, static_cast<std::size_t>(r));
                return Read::Ok;
            }
            if (r < 0 && WSAGetLastError() == WSAETIMEDOUT && !stopping.load()) continue;
            return Read::Closed;
        }
    }

    void reply_and_close(SOCKET s, int code, const std::string& body) {
        const auto resp = http_response(code, body);
        send_all(s, resp.c_str(), resp.size());
        closesocket(s);
    }

    void serve(SOCKET s) {
        net::set_recv_timeout(s, 250);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(requestTimeoutMs.load());

        // 读到 \r\n\r\n
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            if (req.size() >= kMaxHeadBytes) {
                reply_and_close(s, 431, "{\"error\":\"request head too large\"}");
                return;
            }
            const auto rd = read_some(s, req, deadline);
            if (rd == Read::Deadline) {
                reply_and_close(s, 408, "{\"error\":\"request timeout\"}");
                return;
            }
            if (rd == Read::Closed) {
                closesocket(s);
                return;
            }
        }
        const auto hdrEnd = req.find("\r\n\r\n");
        std::string head = req.substr(0, hdrEnd), body = req.substr(hdrEnd + 4);

        // 请求行：METHOD /path?query HTTP/1.1
        const auto lineEnd = head.find("\r\n");
        const auto line = head.substr(0, lineEnd);
        const auto m1 = line.find(' ');
        const auto m2 = line.find(' ', m1 + 1);
        if (m1 == std::string::npos || m2 == std::string::npos) {
            reply_and_close(s, 400, "{\"error\":\"bad request\"}");
            return;
        }
        const std::string method = line.substr(0, m1);
        std::string target = line.substr(m1 + 1, m2 - m1 - 1);
        std::string query;
        if (const auto q = target.find('?'); q != std::string::npos) {
            query = target.substr(q + 1);
            target = target.substr(0, q);
        }

        // 安全策略：浏览器跨域请求与 DNS 重绑定（见 control_api.h）
        if (header_value(head, "origin")) {
            reply_and_close(s, 403, "{\"error\":\"cross-origin request rejected\"}");
            return;
        }
        if (const auto host = header_value(head, "host"); host && !is_loopback_host(*host)) {
            reply_and_close(s, 403, "{\"error\":\"host not allowed\"}");
            return;
        }
        if (header_value(head, "transfer-encoding")) { // 不支持分块体；明确拒绝而非读错
            reply_and_close(s, 501, "{\"error\":\"transfer-encoding not supported\"}");
            return;
        }

        std::size_t contentLen = 0;
        if (const auto cl = header_value(head, "content-length")) {
            const auto n = std::strtoull(cl->c_str(), nullptr, 10);
            if (n > kMaxBodyBytes) {
                reply_and_close(s, 413, "{\"error\":\"request body too large\"}");
                return;
            }
            contentLen = static_cast<std::size_t>(n);
        }
        while (body.size() < contentLen) {
            const auto rd = read_some(s, body, deadline);
            if (rd == Read::Deadline) {
                reply_and_close(s, 408, "{\"error\":\"request timeout\"}");
                return;
            }
            if (rd == Read::Closed) break; // 对端提前关闭：按已收到的部分处理（体被截断）
        }
        body.resize(std::min(body.size(), contentLen));

        const std::string resp = route(method, target, query, body); // 已是完整 HTTP 响应
        send_all(s, resp.c_str(), resp.size());
        closesocket(s);
    }

    static std::string query_param(const std::string& q, const std::string& key,
                                   const std::string& def) {
        const auto at = q.find(key + "=");
        if (at == std::string::npos) return def;
        const auto start = at + key.size() + 1;
        const auto end = q.find('&', start);
        return q.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    std::string route(const std::string& method, const std::string& path,
                      const std::string& query, const std::string& body) {
        using nlohmann::json;
        try {
            if (method == "GET" && path == "/status")
                return cb.status ? http_response(200, cb.status())
                                 : http_response(501, "{\"error\":\"no status\"}");
            if (method == "GET" && path == "/log") {
                if (!cb.log_tail) return http_response(501, "{\"error\":\"no log\"}");
                const auto n = std::strtoul(query_param(query, "n", "100").c_str(), nullptr, 10);
                return http_response(200, cb.log_tail(n ? n : 100));
            }
            if (method == "DELETE" && path == "/log") {
                if (cb.clear_log) cb.clear_log();
                return http_response(200, "{\"ok\":true}");
            }
            if (method == "POST" && path == "/attach") {
                if (!cb.attach) return http_response(501, "{\"error\":\"no attach\"}");
                const auto j = json::parse(body.empty() ? "{}" : body);
                const auto name = j.value("profile", std::string());
                if (name.empty()) return http_response(400, "{\"error\":\"profile required\"}");
                return http_response(200, cb.attach(name));
            }
            if (method == "POST" && path == "/detach")
                return cb.detach ? http_response(200, cb.detach())
                                 : http_response(501, "{\"error\":\"no detach\"}");
            if (method == "POST" && path == "/shutdown")
                return cb.shutdown ? http_response(200, cb.shutdown())
                                   : http_response(501, "{\"error\":\"shutdown not supported\"}");
            if (method == "POST" && path == "/event") {
                if (!cb.event) return http_response(501, "{\"error\":\"no event\"}");
                const auto j = json::parse(body.empty() ? "{}" : body);
                const auto type = j.value("type", std::string());
                if (type.empty()) return http_response(400, "{\"error\":\"type required\"}");
                return http_response(200, cb.event(type));
            }
            if (method == "POST" && path == "/inject") {
                if (!cb.inject) return http_response(501, "{\"error\":\"no inject\"}");
                return http_response(200, cb.inject(body));
            }
            if (method == "POST" && path == "/source")
                return cb.source ? http_response(200, cb.source(body))
                                 : http_response(501, "{\"error\":\"no source\"}");
            if (method == "POST" && path == "/capture")
                return cb.capture ? http_response(200, cb.capture(body))
                                  : http_response(501, "{\"error\":\"no capture\"}");
            if (method == "POST" && path == "/monitor")
                return cb.monitor ? http_response(200, cb.monitor(body))
                                  : http_response(501, "{\"error\":\"no monitor\"}");
            return http_response(404, "{\"error\":\"not found\"}");
        } catch (const json::exception& e) {
            return http_response(400, std::string("{\"error\":\"bad json: ") + e.what() + "\"}");
        } catch (const std::exception& e) {
            return http_response(500, std::string("{\"error\":\"") + e.what() + "\"}");
        }
    }
};

ControlApi::ControlApi() : impl_(new Impl{}, ImplDeleter{}) {}
ControlApi::~ControlApi() { stop(); }
void ControlApi::ImplDeleter::operator()(Impl* p) const { delete p; }

void ControlApi::set_request_timeout_ms(int ms) { impl_->requestTimeoutMs.store(ms); }

bool ControlApi::start(std::uint16_t port, ControlCallbacks cb) {
    wsa_init_once();
    impl_->cb = std::move(cb);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR ||
        listen(ls, 8) == SOCKET_ERROR) {
        closesocket(ls);
        return false;
    }
    impl_->listener.store(ls);
    impl_->acceptThread = std::thread([this] { // uac-cov:thread
        for (;;) {
            if (impl_->stopping.load()) return;
            SOCKET ls = impl_->listener.load();
            if (ls == INVALID_SOCKET) return;
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(ls, &rf);
            timeval tv{0, 100 * 1000};
            if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET s = accept(ls, nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                // transient failure: keep serving instead of dying; backoff
                // prevents a select-readable/accept-fail busy loop
                if (impl_->stopping.load()) return;
                Sleep(50);
                continue;
            }
            if (impl_->stopping.load()) { closesocket(s); return; }
            impl_->serve(s); // 串行处理；单个请求有总时限（requestTimeoutMs），不会被空闲连接拖死
        }
    });
    return true;
}

void ControlApi::stop() {
    if (impl_->stopping.exchange(true)) return;
    SOCKET ls = impl_->listener.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) { shutdown(ls, SD_BOTH); closesocket(ls); }
    if (impl_->acceptThread.joinable()) impl_->acceptThread.join();
}

std::uint16_t ControlApi::port() const {
    SOCKET ls = impl_->listener.load();
    if (ls == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

} // namespace uac
