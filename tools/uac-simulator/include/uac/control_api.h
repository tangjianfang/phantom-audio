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

private:
    struct Impl;
    struct ImplDeleter {
        void operator()(Impl*) const;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
};

} // namespace uac
