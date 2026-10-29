// UDE 适配层实现
#include <uac/ude_adapter.h>

namespace uac::ude {

int adapt_config_for_ude(std::vector<uint8_t>& cfg) {
    auto v = desc::parse_config(cfg);
    if (!v) return -1;
    int changed = 0;
    for (auto&& e : v->endpoints) {
        const uint8_t xfer = e.attributes & 0x03;
        if (xfer == 0x01 /*iso*/ || xfer == 0x03 /*interrupt*/) {
            if (cfg[e.offset + 6] < 4) {
                cfg[e.offset + 6] = 4; // HS 微帧语义：2^(4-1)=8×125µs=1ms
                ++changed;
            }
        }
    }
    return changed;
}

std::vector<LintError> lint_common(const desc::ConfigView& cfg) {
    std::vector<LintError> errs;
    bool hasAc = false;
    for (auto&& iv : cfg.interfaces)
        if (iv.cls == 0x01 && iv.sub == 0x01) hasAc = true;
    if (!hasAc)
        errs.push_back({"missing-audio-control-interface", "无 AudioControl 接口(cls=1,sub=1)", 0});

    for (auto&& iv : cfg.interfaces) {
        if (!(iv.cls == 0x01 && iv.sub == 0x02)) continue; // 只检 AS
        if (iv.alt == 0 && iv.nEndpoints != 0)
            errs.push_back({"as-alt0-has-endpoint",
                            "AS alt0 必须零端点（usbaudio2.sys 约束）", iv.offset});
        if (iv.alt != 0) {
            bool hasIso = false;
            for (auto&& e : cfg.endpoints)
                if (e.iface_index < cfg.interfaces.size()) {
                    const auto& owner = cfg.interfaces[e.iface_index];
                    if (owner.num == iv.num && owner.alt == iv.alt && (e.attributes & 0x03) == 0x01)
                        hasIso = true;
                }
            if (!hasIso)
                errs.push_back({"as-alt-no-iso-endpoint",
                                "AS 非 0 alt 必须含等时数据端点", iv.offset});
        }
    }
    return errs;
}

} // namespace uac::ude
