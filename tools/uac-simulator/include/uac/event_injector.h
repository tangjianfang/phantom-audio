#pragma once
// 事件注入器（R6 主通道）：设备侧音量键/静音键 → UAC2 AC 中断端点 Status Word。
// ADC-2 6.1.2 Status Word 格式（6B，小端）：
//   b0     : bStatusType（D0=0 实体/1 接口，D1=0 AudioStreaming…）
//   b1     : bOriginator（bEntityID 或 bInterfaceNumber）
//   b2     : bNotify（CUR_CHANGED=0x01 等）
//   b3..b4 : wValue（被通知控制的 CN，如音量变化=0x0001）
//   b5     : bmInterruptType（D0=bStatusType Aborted…）
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace uac {

enum class NotifyKind : std::uint8_t { CurChanged = 0x01, RangeChanged = 0x02 };

class EventInjector {
public:
    explicit EventInjector(std::uint8_t interruptPayloadLen = 6);

    // 场景注入：改状态并入队 Status Word（entityID 指向 FU）
    void volume_changed(std::uint8_t entityId, std::uint8_t channel = 0);
    void mute_changed(std::uint8_t entityId, std::uint8_t channel = 0);
    // UAC1 兼容：2 字节 {bStatusType, bOriginator}
    void set_uac1_mode();

    // 中断 IN 端点数据消费：有挂起通知则弹出；返回 nullopt 表示应保持挂起（NAK 语义）
    std::optional<std::vector<std::uint8_t>> poll_interrupt_in();
    bool pending() const;
    void clear();

private:
    void push(std::uint8_t originator, std::uint8_t notify, std::uint16_t wValue);
    mutable std::mutex mtx_;
    std::vector<std::vector<std::uint8_t>> queue_;
    std::uint8_t payloadLen_ = 6; // UAC2=6，UAC1=2
};

} // namespace uac
