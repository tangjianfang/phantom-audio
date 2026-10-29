#pragma once
// UAC1 设备模板：生成拓扑 IT→FU→OT 双路径（播放 + 采集）的完整描述符集。
// 字节风格对标 C-Media 0D8C:0103（黄金基准见 tests/test_usb_desc.cpp）。
#include <uac/usb_desc.h>
#include <map>
#include <string>
#include <vector>

namespace uac::uac1 {

struct Uac1Profile {
    std::uint16_t vid = 0x0D8C, pid = 0x0103, bcdDevice = 0x0010;
    std::string manufacturer = "Phantom Audio Labs";
    std::string product = "UAC1 Simulator";
    std::string serial; // 空 = 不暴露

    std::uint8_t channels = 2;
    std::vector<std::uint32_t> rates = {48000}; // 1..? 档；wMaxPacketSize 取最大档
    struct {
        std::int16_t minDb = -6144; // -24.00 dB（1/256 dB 单位）
        std::int16_t maxDb = 0;
        std::int16_t resDb = 256;   // 1.00 dB
    } vol;

    bool spkOut = true; // AS 播放接口（iso OUT EP）
    bool micIn = true;  // AS 采集接口（iso IN EP）
    bool acInterruptEp = false; // AC 状态中断端点（R6 UAC1 通道，Windows 支持性 M2 实测）
};

// 实体 ID（写死，控制处理器与之匹配）
inline constexpr std::uint8_t kItSpeaker = 1, kFuSpeaker = 2, kOtSpeaker = 3;
inline constexpr std::uint8_t kItMic = 4, kFuMic = 5, kOtMic = 6;
inline constexpr std::uint8_t kInterfaceAC = 0, kInterfaceSpk = 1, kInterfaceMic = 2;

desc::DeviceDesc build_device_desc(const Uac1Profile& p);
std::vector<std::uint8_t> build_device_desc_encoded(const Uac1Profile& p); // 18B 线上格式
std::vector<std::uint8_t> build_config_desc(const Uac1Profile& p); // 已含 UDE 无关的原始 FS 语义
std::map<std::uint8_t, std::vector<std::uint8_t>> build_string_descs(const Uac1Profile& p);
std::uint16_t max_packet_size(const Uac1Profile& p); // 最大采样率 × 声道 × 2B

} // namespace uac::uac1
