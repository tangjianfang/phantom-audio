#pragma once
// USB/IP server 传输层：只监听 127.0.0.1。
// 生命周期：start → (DEVLIST 查询连接…) → IMPORT 连接进入 URB 循环 → 断开 → 可再次 IMPORT。
#include <uac/usbip_proto.h>
#include <uac/urb_dispatcher.h>
#include <memory>
#include <vector>

namespace uac {

class UsbIpServer {
public:
    UsbIpServer();
    ~UsbIpServer();
    UsbIpServer(const UsbIpServer&) = delete;
    UsbIpServer& operator=(const UsbIpServer&) = delete;

    // 绑定 127.0.0.1:port（0=随机），成功后开始接受连接
    bool start(std::uint16_t port, UrbDispatcher& dispatcher,
               std::vector<usbip::UsbIpDevice> exportable);
    void stop();

    bool attached() const;    // IMPORT 生效中（URB 通道占用）
    void kick();             // 主动断开当前 IMPORT 连接（故障注入"拔线"，触发 on_detach）
    std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uac
