#pragma once
// 虚拟微帧时钟：USB 高速总线 125µs 微帧的抽象时间源。
// 测试注入 ManualClock + 手动 pump()；生产用 WallClock + 引擎内部自动 pump。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <windows.h>

namespace uac {

class MicroframeClock {
public:
    virtual ~MicroframeClock() = default;
    virtual std::uint64_t now_us() const = 0; // 单调微秒
};

class ManualClock final : public MicroframeClock {
public:
    std::uint64_t now_us() const override { return us_.load(std::memory_order_relaxed); }
    void advance_to(std::uint64_t us) { us_.store(us, std::memory_order_relaxed); }
    void advance_us(std::uint64_t d) { advance_to(now_us() + d); }

private:
    std::atomic<std::uint64_t> us_{0};
};

class WallClock final : public MicroframeClock {
public:
    WallClock() { QueryPerformanceFrequency(&freq_); }
    std::uint64_t now_us() const override {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<std::uint64_t>(c.QuadPart * 1000000ULL / freq_.QuadPart);
    }

private:
    LARGE_INTEGER freq_{};
};

} // namespace uac
