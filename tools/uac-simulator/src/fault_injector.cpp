// 故障注入实现
#include <uac/fault_injector.h>
#include <chrono>
#include <algorithm>

namespace uac {

std::uint64_t FaultInjector::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void FaultInjector::arm(FaultRule r) {
    std::lock_guard lk(mtx_);
    if (r.kind == FaultRule::Kind::Disconnect) cnt_.armMs = now_ms();
    rules_.push_back(std::move(r));
    note("armed:kind=" + std::to_string(static_cast<int>(r.kind)));
}

void FaultInjector::clear() {
    std::lock_guard lk(mtx_);
    rules_.clear();
    cnt_ = Counters{};
    note("cleared");
}

bool FaultInjector::empty() const {
    std::lock_guard lk(mtx_);
    return rules_.empty();
}

void FaultInjector::note(const std::string& ev) { fired_.push_back(ev); }

bool FaultInjector::should_stall(std::uint8_t t, std::uint8_t r) {
    std::lock_guard lk(mtx_);
    for (auto& rule : rules_) {
        if (rule.kind != FaultRule::Kind::StallNth) continue;
        if (rule.bmRequestType >= 0 && rule.bmRequestType != t) continue;
        if (rule.bRequest >= 0 && rule.bRequest != r) continue;
        if (++cnt_.stallSeen >= rule.nth) {
            note("fired:stall bRequest=" + std::to_string(r));
            return true;
        }
    }
    return false;
}

int FaultInjector::delay_for(std::uint8_t t, std::uint8_t r) {
    std::lock_guard lk(mtx_);
    for (auto& rule : rules_) {
        if (rule.kind != FaultRule::Kind::DelayMs) continue;
        if (rule.bmRequestType >= 0 && rule.bmRequestType != t) continue;
        if (rule.bRequest >= 0 && rule.bRequest != r) continue;
        if (++cnt_.delaySeen >= rule.nth) {
            note("fired:delay bRequest=" + std::to_string(r) + " ms=" + std::to_string(rule.ms));
            return rule.ms;
        }
    }
    return 0;
}

void FaultInjector::on_ep0_request() {
    std::lock_guard lk(mtx_);
    ++cnt_.ep0Count;
}

bool FaultInjector::mutate_config_response(std::vector<std::uint8_t>& cfgData) {
    std::lock_guard lk(mtx_);
    for (auto& rule : rules_) {
        if (rule.kind == FaultRule::Kind::CorruptDescByteAt) {
            if (rule.byteOffset >= 0 && static_cast<std::size_t>(rule.byteOffset) < cfgData.size()) {
                cfgData[static_cast<std::size_t>(rule.byteOffset)] ^=
                    static_cast<std::uint8_t>(rule.xorByte);
                note("fired:corrupt at=" + std::to_string(rule.byteOffset));
            }
        }
    }
    return false; // 半枚举断连统一由 should_disconnect() 一次性触发
}

bool FaultInjector::should_disconnect() {
    std::lock_guard lk(mtx_);
    for (auto it = rules_.begin(); it != rules_.end(); ++it) {
        bool hit = false;
        if (it->kind == FaultRule::Kind::Disconnect &&
            now_ms() - cnt_.armMs >= static_cast<std::uint64_t>(it->ms)) {
            note("fired:disconnect");
            hit = true;
        } else if (it->kind == FaultRule::Kind::HalfEnumerate && cnt_.ep0Count >= it->stopAtStep) {
            note("fired:half-enumerate");
            hit = true;
        }
        if (hit) { // 一次性消费：触发即移除，避免每请求重复 kick
            rules_.erase(it);
            return true;
        }
    }
    return false;
}

std::uint32_t FaultInjector::iso_error_count() {
    std::lock_guard lk(mtx_);
    ++cnt_.isoRetCount;
    for (auto& rule : rules_) {
        if (rule.kind != FaultRule::Kind::IsoError) continue;
        if (rule.everyN > 0 && cnt_.isoRetCount % rule.everyN == 0) {
            note("fired:iso-error #" + std::to_string(cnt_.isoRetCount));
            return static_cast<std::uint32_t>(rule.errCount);
        }
    }
    return 0;
}

std::vector<std::string> FaultInjector::fired() const {
    std::lock_guard lk(mtx_);
    return fired_;
}

} // namespace uac
