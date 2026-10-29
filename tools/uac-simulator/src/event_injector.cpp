// 事件注入器实现
#include <uac/event_injector.h>

namespace uac {

EventInjector::EventInjector(std::uint8_t interruptPayloadLen) : payloadLen_(interruptPayloadLen) {
    if (payloadLen_ != 2) payloadLen_ = 6;
}

void EventInjector::set_uac1_mode() {
    std::lock_guard lk(mtx_);
    payloadLen_ = 2;
}

void EventInjector::volume_changed(std::uint8_t entityId, std::uint8_t channel) {
    push(entityId, static_cast<std::uint8_t>(NotifyKind::CurChanged),
         static_cast<std::uint16_t>(channel ? channel : 0x0001));
}
void EventInjector::mute_changed(std::uint8_t entityId, std::uint8_t channel) {
    push(entityId, static_cast<std::uint8_t>(NotifyKind::CurChanged),
         static_cast<std::uint16_t>(channel ? channel : 0x0001));
}

void EventInjector::push(std::uint8_t originator, std::uint8_t notify, std::uint16_t wValue) {
    std::lock_guard lk(mtx_);
    std::vector<std::uint8_t> w;
    if (payloadLen_ == 2) {
        // UAC1（audio10）：bStatusType(bit0=0 实体) + bOriginator
        w = {0x00, originator};
    } else {
        // UAC2（ADC-2 6.1.2）
        w = {0x00 /*bStatusType: entity*/, originator, notify,
             static_cast<std::uint8_t>(wValue & 0xff), static_cast<std::uint8_t>(wValue >> 8),
             0x00 /*bmInterruptType*/};
    }
    queue_.push_back(std::move(w));
}

std::optional<std::vector<std::uint8_t>> EventInjector::poll_interrupt_in() {
    std::lock_guard lk(mtx_);
    if (queue_.empty()) return std::nullopt;
    auto v = std::move(queue_.front());
    queue_.erase(queue_.begin());
    return v;
}

bool EventInjector::pending() const {
    std::lock_guard lk(mtx_);
    return !queue_.empty();
}
void EventInjector::clear() {
    std::lock_guard lk(mtx_);
    queue_.clear();
}

} // namespace uac
