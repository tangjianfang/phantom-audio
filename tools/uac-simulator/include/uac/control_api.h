#pragma once
// 本地控制 API：127.0.0.1 上的极简 HTTP/1.1 JSON 服务（三客户端：CLI/GUI/CI 共享）。
// 线程模型：每请求一线程处理回调；回调自行保证线程安全。
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace uac {

struct ControlCallbacks {
    // GET /status → JSON 文本
    std::function<std::string()> status;
    // GET /log?n=100 → NDJSON 行数组文本（JSON 数组）
    std::function<std::string(std::size_t)> log_tail;
    // DELETE /log
    std::function<void()> clear_log;
    // POST /attach {"profile":"uac2-48k"} → 结果文本
    std::function<std::string(const std::string&)> attach;
    // POST /detach
    std::function<std::string()> detach;
    // POST /shutdown → 结果文本。只"请求"进程退出（回应发出后由进程主循环收尾），不在请求线程里停服务。
    // 与其它端点同一套 Origin / Host 策略，所以网页碰不到它。
    std::function<std::string()> shutdown;
    // POST /event {"type":"volume_up"} → 结果文本
    std::function<std::string(const std::string&)> event;
    // POST /inject {...} → 结果文本
    std::function<std::string(const std::string& body)> inject;
    // POST /source {...} / POST /capture {...}
    std::function<std::string(const std::string& body)> source;
    std::function<std::string(const std::string& body)> capture;
};

class ControlApi {
public:
    ControlApi();
    ~ControlApi();
    ControlApi(const ControlApi&) = delete;
    ControlApi& operator=(const ControlApi&) = delete;

    bool start(std::uint16_t port, ControlCallbacks cb); // port=0 随机
    void stop();
    std::uint16_t port() const;

    // 单个请求（读完请求行+头+体）的总时限，默认 5000 ms。API 线程串行处理连接，
    // 没有总时限的话，一条"连上就不发数据"的连接会让所有后续请求排队不动；
    // 超时回 408 并关闭。可在 start 前后调用（原子量，立即生效）。
    void set_request_timeout_ms(int ms);

    // 安全策略（回环 + 无认证的本地 API 只能靠这两条防浏览器攻击）：
    //  - 请求带 Origin 头一律 403：浏览器跨域 POST 必带 Origin，CLI/GUI/脚本不带；
    //  - 带 Host 头但不是 127.0.0.1/localhost（可带端口）→ 403，防 DNS 重绑定。
    // 缺 Host 的请求放行（原始 socket 客户端；浏览器不可能不发 Host）。
    // 请求体上限 1 MiB（413）。

private:
    struct Impl;
    struct ImplDeleter {
        void operator()(Impl*) const;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
};

} // namespace uac
