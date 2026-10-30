#pragma once
// AudioControl 请求处理器：标准 EP0 请求（描述符/配置）+ UAC1 类请求（FU 音量/静音、
// 端点采样率）。全量请求进日志；STALL 由 FaultInjector 在上层拦截（Task 17 接入）。
#include <uac/log_store.h>
#include <map>
#include <span>
#include <stdint.h>
#include <vector>

namespace uac {

struct AcState {
    std::int16_t volCur[2]{0, 0}; // 1/256 dB，声道 0/1（master 同步写 0）
    std::int16_t volMin = -6144, volMax = 0, volRes = 256;
    bool mute[2]{false, false};
    std::uint32_t sampleRate = 48000;
};

struct ControlResult {
    enum class Kind { Ok, Stall };
    Kind kind = Kind::Ok;
    std::vector<std::uint8_t> data; // IN 方向响应数据（已截断至 wLength）
};

enum class Protocol : std::uint8_t { Uac1, Uac2 };

class AcProcessor {
public:
    AcProcessor(AcState& state, LogStore& log);

    // 描述符注入（枚举用；qualifier 可选 —— UAC2 HS 设备必需）
    void set_descriptors(std::vector<std::uint8_t> device, std::vector<std::uint8_t> config,
                         std::map<std::uint8_t, std::vector<std::uint8_t>> strings,
                         std::vector<std::uint8_t> qualifier = {});
    void set_msos(std::vector<std::uint8_t> bos, std::vector<std::uint8_t> descriptorSet,
                  std::uint8_t vendorCode) {
        bos_ = std::move(bos);
        msosSet_ = std::move(descriptorSet);
        msosVendorCode_ = vendorCode;
    }
    void set_protocol(Protocol p) { proto_ = p; }
    void set_rates(std::vector<std::uint32_t> rates) { rates_ = std::move(rates); }
    void set_clock_id(std::uint8_t id) { clockId_ = id; }

    ControlResult on_control(std::uint8_t bmRequestType, std::uint8_t bRequest,
                             std::uint16_t wValue, std::uint16_t wIndex, std::uint16_t wLength,
                             std::span<const std::uint8_t> outData);

    std::uint64_t request_count() const { return requestCount_; }

    // UAC1 实体 ID（与 desc_uac1 一致；用于过滤 FU 请求）
    void set_feature_unit_ids(std::vector<std::uint8_t> ids) { fuIds_ = std::move(ids); }

private:
    AcState& st_;
    LogStore& log_;
    std::uint64_t requestCount_ = 0;
    std::vector<std::uint8_t> dev_, cfg_, qualifier_;
    std::map<std::uint8_t, std::vector<std::uint8_t>> strings_;
    std::vector<std::uint8_t> fuIds_;
    std::vector<std::uint8_t> bos_, msosSet_;
    std::uint8_t msosVendorCode_ = 0;
    Protocol proto_ = Protocol::Uac1;
    std::vector<std::uint32_t> rates_{48000};
    std::uint8_t clockId_ = 4;

    void log_req(std::uint8_t bmRequestType, std::uint8_t bRequest, std::uint16_t wValue,
                 std::uint16_t wIndex, std::uint16_t wLength, const char* outcome, std::size_t n);
};

} // namespace uac
