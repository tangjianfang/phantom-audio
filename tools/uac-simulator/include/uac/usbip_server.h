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

    bool attached() const;    // IMPORT active on any channel
    void kick();              // drop every imported connection (fault "unplug")
    std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uac
