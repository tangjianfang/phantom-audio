#pragma once
// USB/IP server transport layer (loopback only, 127.0.0.1).
// Lifecycle: start -> (DEVLIST query connections...) -> IMPORT connection enters
// the URB loop -> disconnect -> another IMPORT is possible.
// Multi-instance: each exported device carries its own UrbDispatcher; URBs on a
// connection are routed to the dispatcher of the imported busid.
#include <uac/usbip_proto.h>
#include <uac/urb_dispatcher.h>
#include <memory>
#include <utility>
#include <vector>

namespace uac {

class UsbIpServer {
public:
    UsbIpServer();
    ~UsbIpServer();
    UsbIpServer(const UsbIpServer&) = delete;
    UsbIpServer& operator=(const UsbIpServer&) = delete;

    // Multi-device form: every exported device pairs with the dispatcher that
    // owns its device model.
    using Export = std::pair<usbip::UsbIpDevice, UrbDispatcher*>;
    bool start(std::uint16_t port, const std::vector<Export>& exportable);

    // Single-device convenience form (all URBs to one dispatcher).
    bool start(std::uint16_t port, UrbDispatcher& dispatcher,
               std::vector<usbip::UsbIpDevice> exportable);

    void stop();

    // 控制连接（DEVLIST/IMPORT 握手阶段）的空闲时限，默认 10000 ms。accept 线程串行
    // 处理握手，没有时限的话一条"连上不发数据"的连接会挡住后续所有 attach；
    // 超时则关闭该连接。已进入 URB 通道的连接不受此限。
    void set_control_idle_timeout_ms(int ms);

    bool attached() const;    // IMPORT active on any channel
    void kick();              // drop every imported connection (fault "unplug")
    std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uac
