#pragma once
// URB 分发接口 —— 传输层（UsbIpServer）与设备模型（DeviceModel）之间的边界。
// 传输层负责线上编解码与线程；设备模型只实现本接口。
#include <uac/usbip_proto.h>
#include <functional>
#include <span>

namespace uac {

// 一次 RET_SUBMIT 的完整内容（头字段 + IN 负载 + iso 描述符）
struct Completion {
    usbip::RetSubmit ret{};
    std::vector<unsigned char> payload;   // IN 方向的数据
    std::vector<usbip::IsoPacket> iso;    // ISO 时的每包描述符
};
using CompletionFn = std::function<void(Completion&&)>;
using UnlinkResultFn = std::function<void(std::int32_t status)>;

class UrbDispatcher {
public:
    virtual ~UrbDispatcher() = default;
    // IMPORT 成功（设备已被 vhci 认领）
    virtual void on_import(const usbip::UsbIpDevice& dev) = 0;
    // 收到 CMD_SUBMIT。complete 可任意线程调用一次；被 UNLINK 抢先则不得调用。
    virtual void on_urb(const usbip::UrbCommand& cmd, std::span<const unsigned char> payload,
                        std::span<const usbip::IsoPacket> isoDescs, CompletionFn complete) = 0;
    // 收到 CMD_UNLINK（取消挂起的 unlink_seqnum）。done 回传 RET_UNLINK 的 status
    //（成功取消 -ECONNRESET=Linux -104；已完成后续到的 UNLINK 为 0）。
    virtual void on_unlink(std::uint32_t seqnum, std::uint32_t unlink_seqnum, UnlinkResultFn done) = 0;
    // 连接关闭（设备拔出/仿真器主动断开）
    virtual void on_detach() = 0;
};

} // namespace uac
