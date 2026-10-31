// SimHost —— uacsimd 的服务编排：DeviceModel + UsbIpServer + ControlApi 生命周期
#pragma once
#include <uac/control_api.h>
#include <uac/device_model.h>
#include <uac/profile.h>
#include <uac/usbip_server.h>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

namespace uac {

class SimHost {
public:
    explicit SimHost(std::string profilesDir);

    bool start(std::uint16_t usbipPort, std::uint16_t httpPort);
    // log file applied to every subsequently attached model (fixes --log-file)
    void set_log_file(const std::string& path) { logFile_ = path; }
    void stop();
    bool running() const { return running_.load(); }
    // 实际监听端口（start 传 0 时为系统分配的端口；进程内自测与单测需要）
    std::uint16_t http_port() const { return api_.port(); }
    // 测试用：缩短握手/请求时限，避免等满默认值
    void set_io_timeouts_ms(int controlApiRequestMs, int usbipControlIdleMs) {
        api_.set_request_timeout_ms(controlApiRequestMs);
        server_.set_control_idle_timeout_ms(usbipControlIdleMs);
    }

    // 控制 API 回调实现（线程安全）
    std::string on_status();
    // confineName=false 允许绝对/相对路径（CLI --profile 契约）；HTTP 层传 true 仅允许纯名字
    std::string on_attach(const std::string& profileName, bool confineName = true);
    std::string on_detach();
    // POST /shutdown: only records the request; uacsimd's main loop polls shutdown_requested() and stops the services after
    // the reply has gone out (stopping the API from inside its own request thread would deadlock).
    std::string on_shutdown();
    bool shutdown_requested() const { return shutdownRequested_.load(); }
    // 诊断：把每个实例的每端点 URB 增量写进它的日志（空闲秒自动静默）。uacsimd 主循环每秒调一次。
    void dump_urb_stats();
    std::string on_event(const std::string& type);
    std::string on_inject(const std::string& body); // Task 17 接入 FaultInjector
    std::string on_source(const std::string& body);
    std::string on_capture(const std::string& body);

    DeviceModel* model() { return model_; }
    DeviceModel* model_at(std::size_t i) {
        return i < models_.size() ? models_[i].second.get() : nullptr;
    }
    std::size_t instance_count() const { return models_.size(); }

private:
    std::string profiles_dir_;
    mutable std::mutex mtx_;
    DeviceModel* model_ = nullptr;                    // 指向 models_ 末实例（兼容单实例访问）
    std::vector<std::pair<std::string, std::unique_ptr<DeviceModel>>> models_; // 多实例（busid → 模型）
    UsbIpServer server_;
    ControlApi api_;
    std::string current_profile_;
    std::uint16_t usbipPort_ = 3240; // honored by every (re)listen
    std::string logFile_;            // per-model NDJSON sink
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};
};

// profiles/<name>.json 缺席时仍可内置提供的名字（唯一真源：sim_host.cpp 的 builtinProfiles() 表）
bool is_builtin_profile(const std::string& name);

} // namespace uac
