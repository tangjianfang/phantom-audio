#pragma once
// 故障注入目录：STALL / 延迟 / 描述符损坏 / 半枚举 / 断连 / iso error_count。
// 查询点由 DeviceModel 在关键路径调用；触发事件全部入 LogStore。
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace uac {

struct FaultRule {
    enum class Kind { StallNth, DelayMs, CorruptDescByteAt, HalfEnumerate, Disconnect, IsoError };
    Kind kind = Kind::StallNth;

    // 匹配（-1 = 任意）
    int bmRequestType = -1;
    int bRequest = -1;
    int nth = 1; // 命中第 nth 条匹配请求（1 起）

    int ms = 0;                        // DelayMs：延迟毫秒；Disconnect：afterMs 后断连
    int byteOffset = 0;                // CorruptDescByteAt：配置描述符内偏移
    unsigned char xorByte = 0xFF;      // CorruptDescByteAt：异或字节
    int stopAtStep = 1;                // HalfEnumerate：第 N 条控制请求后断连
    int everyN = 2, errCount = 1;      // IsoError：每 everyN 个 iso RET 注入 error_count
};

class FaultInjector {
public:
    void arm(FaultRule r);
    void clear();

    // —— 查询点 ——
    // EP0：是否 STALL 该请求（匹配 bmRequestType/bRequest 的第 nth 条）
    bool should_stall(std::uint8_t bmRequestType, std::uint8_t bRequest);
    // EP0：该请求应延迟多少毫秒
    int delay_for(std::uint8_t bmRequestType, std::uint8_t bRequest);
    // GET_DESCRIPTOR(config) 响应修改：损坏注入 + 半枚举计数
    // 返回 true 表示应立即断连（半枚举触发）
    bool mutate_config_response(std::vector<std::uint8_t>& cfgData);
    // 计数一条 EP0 请求（半枚举/断连计时）
    void on_ep0_request();
    // 是否应断连（HalfEnumerate 计数到 / Disconnect afterMs 到）
    bool should_disconnect();
    // iso RET 注入：返回应设置的 error_count（0 = 不注入）
    std::uint32_t iso_error_count();

    bool empty() const;
    std::vector<std::string> fired() const; // 已触发的注入事件（供日志/断言）

private:
    mutable std::mutex mtx_;
    std::vector<FaultRule> rules_;
    struct Counters {
        int stallSeen = 0, delaySeen = 0;
        int ep0Count = 0;
        std::uint64_t armMs = 0; // arm 时刻（Disconnect 计时用）
        int isoRetCount = 0;
    } cnt_;
    std::vector<std::string> fired_;
    void note(const std::string& ev);
    static std::uint64_t now_ms();
};

} // namespace uac
