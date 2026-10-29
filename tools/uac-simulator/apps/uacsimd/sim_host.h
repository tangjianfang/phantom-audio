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

    // 控制 API 回调实现（线程安全）
    std::string on_status();
    std::string on_attach(const std::string& profileName);
    std::string on_detach();
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
};

} // namespace uac
