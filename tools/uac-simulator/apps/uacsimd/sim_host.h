// SimHost —— uacsimd 的服务编排：DeviceModel + UsbIpServer + ControlApi 生命周期
#pragma once
#include <uac/control_api.h>
#include <uac/device_model.h>
#include <uac/profile.h>
#include <uac/usbip_server.h>
#include <filesystem>
#include <mutex>

namespace uac {

class SimHost {
public:
    explicit SimHost(std::string profilesDir);

    bool start(std::uint16_t usbipPort, std::uint16_t httpPort);
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

    DeviceModel* model() { return model_.get(); }

private:
    std::string profiles_dir_;
    mutable std::mutex mtx_;
    std::unique_ptr<DeviceModel> model_;
    UsbIpServer server_;
    ControlApi api_;
    std::string current_profile_;
    std::atomic<bool> running_{false};
};

} // namespace uac
