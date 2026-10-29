// 控制台 API 实现：极简 HTTP/1.1 + JSON 路由（仅 127.0.0.1）
#include <uac/control_api.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <cstring>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
std::once_flag g_wsa_once;
void wsa_init_once() {
    std::call_once(g_wsa_once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}
} // namespace

namespace uac {

namespace {
bool send_all(SOCKET s, const char* p, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        int r = ::send(s, p + off, static_cast<int>(n - off), 0);
        if (r <= 0) return false;
        off += static_cast<std::size_t>(r);
    }
    return true;
}
bool recv_all(SOCKET s, char* p, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        int r = ::recv(s, p + off, static_cast<int>(n - off), 0);
        if (r <= 0) return false;
        off += static_cast<std::size_t>(r);
    }
    return true;
}

std::string http_response(int code, const std::string& body) {
    const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request"
                         : code == 404  ? "Not Found"
                         : code == 501  ? "Not Implemented"
                                        : "Internal Server Error";
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason +
                    "\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    return h + body;
}
} // namespace

struct ControlApi::Impl {
    std::atomic<bool> stopping{false};
    std::atomic<SOCKET> serving{INVALID_SOCKET}; // connection being served right now
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::thread acceptThread;
    ControlCallbacks cb;

    void serve(SOCKET s) {
        // 读到 \r\n\r\n
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
            int r = ::recv(s, buf, sizeof(buf), 0);
            if (r <= 0) { closesocket(s); return; }
            req.append(buf, static_cast<std::size_t>(r));
        }
        const auto hdrEnd = req.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) { closesocket(s); return; }
        std::string head = req.substr(0, hdrEnd), body = req.substr(hdrEnd + 4);

        // 请求行：METHOD /path?query HTTP/1.1
        const auto lineEnd = head.find("\r\n");
        const auto line = head.substr(0, lineEnd);
        const auto m1 = line.find(' ');
        const auto m2 = line.find(' ', m1 + 1);
        if (m1 == std::string::npos || m2 == std::string::npos) {
            const auto resp = http_response(400, "{\"error\":\"bad request\"}");
            send_all(s, resp.c_str(), resp.size());
            closesocket(s);
            return;
        }
        const std::string method = line.substr(0, m1);
        std::string target = line.substr(m1 + 1, m2 - m1 - 1);
        std::string query;
        if (const auto q = target.find('?'); q != std::string::npos) {
            query = target.substr(q + 1);
            target = target.substr(0, q);
        }
        // Content-Length
        std::size_t contentLen = 0;
        if (const auto cl = head.find("Content-Length:"); cl != std::string::npos)
            contentLen = static_cast<std::size_t>(
                std::strtoull(head.c_str() + cl + 15, nullptr, 10));
        while (body.size() < contentLen) {
            int r = ::recv(s, buf, sizeof(buf), 0);
            if (r <= 0) break;
            body.append(buf, static_cast<std::size_t>(r));
        }
        body.resize(std::min(body.size(), contentLen));

        std::string resp = route(method, target, query, body);
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
            impl_->serve(s); // 串行足够（本地单客户端场景）
        }
    });
    return true;
}

void ControlApi::stop() {
    if (impl_->stopping.exchange(true)) return;
    SOCKET ls = impl_->listener.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) { shutdown(ls, SD_BOTH); closesocket(ls); }
    SOCKET sv = impl_->serving.exchange(INVALID_SOCKET);
    if (sv != INVALID_SOCKET) { shutdown(sv, SD_BOTH); closesocket(sv); }
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
