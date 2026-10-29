#pragma once
// UAC2 设备模板（ADC-2 2.0）：IAD 复合、Clock Source、显式 feedback 端点。
// 硬约束（usbaudio2.sys 官方文档）：单一 Clock Source；异步 OUT 必须显式反馈端点；
// AS alt0 零端点、非零 alt 必须含等时数据端点；采样率经 Clock Source CUR_FREQ 协商
// （描述符中不含速率表，与 UAC1 不同）。
#include <uac/usb_desc.h>
#include <uac/ude_adapter.h>
#include <map>
#include <string>
#include <vector>

namespace uac::uac2 {

struct Uac2Profile {
    std::uint16_t vid = 0x1D50, pid = 0x6222, bcdDevice = 0x0100;
    std::string manufacturer = "Phantom Audio Labs";
    std::string product = "UAC2 Simulator";
    std::string serial;

    std::uint8_t channels = 2;
    std::vector<std::uint32_t> rates = {44100, 48000, 96000};
    struct {
        std::uint8_t subslot = 4; // 4 字节槽
        std::uint8_t bits = 32;
    } pcm;
    struct {
        std::int16_t minDb = -6144, maxDb = 0, resDb = 256;
    } vol;

    bool spkOut = true;
    bool micIn = true;
    bool feedbackEp = true; // 异步 OUT 显式反馈端点（usbaudio2.sys 不支持 implicit feedback）
    bool acInterruptEp = true; // UAC2 Status Word 中断端点（R6 主通道）
};

// 实体/接口 ID
inline constexpr std::uint8_t kItSpeaker = 1, kFuSpeaker = 2, kOtSpeaker = 3, kClock = 4;
inline constexpr std::uint8_t kItMic = 5, kFuMic = 6, kOtMic = 7;
inline constexpr std::uint8_t kInterfaceAC = 0, kInterfaceSpk = 1, kInterfaceMic = 2;

desc::DeviceDesc build_device_desc(const Uac2Profile& p); // bcdUSB 0x0200 + IAD 类码 0xEF/2/1
std::vector<std::uint8_t> build_device_desc_encoded(const Uac2Profile& p);
std::vector<std::uint8_t> build_config_desc(const Uac2Profile& p); // 原始（未经 UDE 适配）
std::vector<std::uint8_t> build_qualifier_desc(const Uac2Profile& p); // 10B HS 设备限定描述符
std::map<std::uint8_t, std::vector<std::uint8_t>> build_string_descs(const Uac2Profile& p);
std::uint16_t max_packet_size(const Uac2Profile& p);

// UAC2 专属 lint（叠加 ude::lint_common）：
//   uac2-single-clock-source / uac2-async-out-needs-feedback / uac2-alt-ascending / uac2-cs-ep
std::vector<struct uac::ude::LintError> lint_uac2(const desc::ConfigView& v,
                                                  const std::vector<std::uint8_t>& rawCfg);

} // namespace uac::uac2
