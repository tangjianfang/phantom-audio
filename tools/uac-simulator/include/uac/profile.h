#pragma once
// Profile JSON：设备档案（VID/PID/字符串/音频参数/拓扑开关/raw 覆盖）→ DeviceSnapshot
#include <uac/device_model.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace uac {

struct ProfileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Profile {
    std::string name; // 文件名（去扩展）
    Protocol protocol = Protocol::Uac1;

    std::uint16_t vid = 0x1D50, pid = 0x6222, bcdDevice = 0x0100;
    std::string manufacturer = "Phantom Audio Labs";
    std::string product = "UAC Simulator";
    std::string serial;

    std::uint8_t channels = 2;
    std::vector<std::uint32_t> rates{48000};
    std::uint8_t subslot = 2, bits = 16;
    std::int16_t volMin = -6144, volMax = 0, volRes = 256;
    bool micIn = true, spkOut = true;
    bool acInterruptEp = false; // UAC1 默认关（Windows 支持性待定），UAC2 默认开
    bool feedbackEp = true;
    bool hidConsumerVolume = false;

    std::vector<std::uint8_t> rawConfigOverride; // 非空则整体替换配置描述符
};

// 解析 + 校验（缺字段/非法值抛 ProfileError）
Profile load_profile(const std::string& jsonPath);
Profile parse_profile_json(const std::string& jsonText, const std::string& name);

// Profile → 可运行快照（组合 UAC1/UAC2 模板 + HID 追加 + UDE 适配 + raw 覆盖优先）
DeviceSnapshot build_snapshot(const Profile& p);

// 热切换判定：配置描述符或设备描述符有差异即需 reattach
bool snapshot_requires_reattach(const DeviceSnapshot& a, const DeviceSnapshot& b);

} // namespace uac
