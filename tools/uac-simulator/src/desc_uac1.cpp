// UAC1 模板实现 —— 拓扑：IT(usb-stream)→FU→OT(speaker) 与 IT(mic)→FU→OT(usb-stream)
#include <uac/desc_uac1.h>

#include <algorithm>
#include <cstring>

namespace uac::uac1 {

namespace {
void le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void le24(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
}

// Input Terminal: len12, subtype 0x02
std::vector<uint8_t> input_terminal(uint8_t id, uint16_t terminalType, uint8_t channels,
                                    uint16_t channelConfig) {
    std::vector<uint8_t> v{0x0C, 0x24, 0x02, id};
    le16(v, terminalType);
    v.push_back(0x00);            // bAssocTerminal
    v.push_back(channels);
    le16(v, channelConfig);
    v.insert(v.end(), {0x00, 0x00}); // iChannelNames, iTerminal
    return v;
}
// Output Terminal: len9, subtype 0x03
std::vector<uint8_t> output_terminal(uint8_t id, uint16_t terminalType, uint8_t sourceId) {
    std::vector<uint8_t> v{0x09, 0x24, 0x03, id};
    le16(v, terminalType);
    v.push_back(0x00);            // bAssocTerminal
    v.push_back(sourceId);
    v.push_back(0x00);            // iTerminal
    return v;
}
// Feature Unit（subtype 0x06）：bControlSize=2，master+ch 通道 bmaControls，尾随 iFeature
// 长度 = 6 + 2*(ch+1) + 1；bmaControls 位定义：D0 Mute, D1 Volume
std::vector<uint8_t> feature_unit(uint8_t id, uint8_t sourceId, uint8_t channels) {
    std::vector<uint8_t> v{0x00 /*bLength 回填*/, 0x24, 0x06, id, sourceId, 0x02 /*bControlSize*/};
    le16(v, 0x0003);              // master: Mute+Volume
    for (uint8_t c = 0; c < channels; ++c) le16(v, 0x0002); // 各声道 Volume
    v.push_back(0x00);            // iFeature
    v[0] = static_cast<uint8_t>(v.size());
    return v;
}
// AS General: len7, subtype 0x01
std::vector<uint8_t> as_general(uint8_t terminalLink) {
    return {0x07, 0x24, 0x01, terminalLink, 0x01 /*bDelay=1ms*/, 0x01, 0x00 /*PCM*/};
}
// Type I Format: 8+3n
std::vector<uint8_t> type_i_format(uint8_t channels, const std::vector<uint32_t>& rates) {
    std::vector<uint8_t> v{static_cast<uint8_t>(0x08 + 3 * rates.size()), 0x24, 0x02,
                           0x01 /*bFormatType I*/, channels, 0x02 /*bSubframeSize*/,
                           0x10 /*16bit*/, static_cast<uint8_t>(rates.size())};
    for (auto r : rates) le24(v, r);
    return v;
}
// CS Audio Data EP: len7, subtype 0x01（bmAttributes=01 MaxPacketsOnly? 0 普通模式）
std::vector<uint8_t> cs_audio_ep() { return {0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00}; }
} // namespace

uint16_t max_packet_size(const Uac1Profile& p) {
    const uint32_t maxRate = *std::max_element(p.rates.begin(), p.rates.end());
    return static_cast<uint16_t>((maxRate * p.channels * 2 + 999) / 1000); // ceil
}

desc::DeviceDesc build_device_desc(const Uac1Profile& p) {
    desc::DeviceDesc d;
    d.bcdUSB = 0x0110;
    d.idVendor = p.vid;
    d.idProduct = p.pid;
    d.bcdDevice = p.bcdDevice;
    d.iManufacturer = 1;
    d.iProduct = 2;
    d.iSerialNumber = p.serial.empty() ? 0 : 3;
    return d;
}

std::vector<uint8_t> build_device_desc_encoded(const Uac1Profile& p) {
    return desc::encode_device_desc(build_device_desc(p));
}

std::map<uint8_t, std::vector<uint8_t>> build_string_descs(const Uac1Profile& p) {
    std::map<uint8_t, std::vector<uint8_t>> m;
    m[1] = desc::string_desc(p.manufacturer);
    m[2] = desc::string_desc(p.product);
    if (!p.serial.empty()) m[3] = desc::string_desc(p.serial);
    return m;
}

std::vector<uint8_t> build_config_desc(const Uac1Profile& p) {
    using desc::Builder;
    const bool mic = p.micIn, spk = p.spkOut;
    const uint8_t nIf = 1 + (mic ? 1 : 0) + (spk ? 1 : 0);
    const uint16_t maxPkt = max_packet_size(p);

    Builder b;
    b.config(nIf, 1, 0x80, 0xFA); // 500mA（宽松取值，测试台不受限）

    // ---- AC 接口（IF0）----
    b.interface_(kInterfaceAC, 0, p.acInterruptEp ? 1 : 0, 0x01, 0x01, 0x00);
    { // AC header：bcdADC 0x0100，wTotalLength 先占位后回填
        std::vector<uint8_t> h{0x00 /*bLength 回填*/, 0x24, 0x01, 0x00, 0x01,
                               0x00, 0x00 /*wTotalLength 占位*/,
                               static_cast<uint8_t>(nIf - 1)};
        if (spk) h.push_back(kInterfaceSpk);
        if (mic) h.push_back(kInterfaceMic);
        h[0] = static_cast<uint8_t>(h.size());
        b.raw(h);
    }
    if (spk) {
        b.raw(input_terminal(kItSpeaker, 0x0101 /*USB streaming*/, p.channels, 0x0003));
        b.raw(feature_unit(kFuSpeaker, kItSpeaker, p.channels));
        b.raw(output_terminal(kOtSpeaker, 0x0301 /*speaker*/, kFuSpeaker));
    }
    if (mic) {
        b.raw(input_terminal(kItMic, 0x0201 /*microphone*/, p.channels, 0x0003));
        b.raw(feature_unit(kFuMic, kItMic, p.channels));
        b.raw(output_terminal(kOtMic, 0x0101 /*USB streaming*/, kFuMic));
    }
    if (p.acInterruptEp)
        b.endpoint(0x81, 0x03 /*interrupt IN*/, 2 /*UAC1 状态字 2B*/, 1 /*FS 1ms*/);

    // ---- AS 播放（iso OUT）----
    if (spk) {
        b.interface_(kInterfaceSpk, 0, 0, 0x01, 0x02, 0x00);
        b.interface_(kInterfaceSpk, 1, 1, 0x01, 0x02, 0x00);
        b.raw(as_general(kItSpeaker));
        b.raw(type_i_format(p.channels, p.rates));
        b.endpoint_audio(0x06, 0x09 /*iso adaptive OUT*/, maxPkt, 1);
        b.raw(cs_audio_ep());
    }

    // ---- AS 采集（iso IN）----
    if (mic) {
        b.interface_(kInterfaceMic, 0, 0, 0x01, 0x02, 0x00);
        b.interface_(kInterfaceMic, 1, 1, 0x01, 0x02, 0x00);
        b.raw(as_general(kOtMic));
        b.raw(type_i_format(p.channels, p.rates));
        b.endpoint_audio(0x86, 0x05 /*iso async IN*/, maxPkt, 1);
        b.raw(cs_audio_ep());
    }

    auto cfg = b.build();

    // 回填 AC class-specific wTotalLength：header 实长 8+n（UAC1 布局无 bCategory）
    const size_t acHdrAt = 9 /*config*/ + 9 /*IF0*/;
    uint16_t hdrLen = static_cast<uint16_t>(8 + (nIf - 1)); // 8 + bInCollection
    uint16_t csLen = hdrLen;
    if (spk) csLen += 12 + 13 + 9; // IT + FU + OT（长度与构造函数一致）
    if (mic) csLen += 12 + 13 + 9;
    cfg[acHdrAt + 5] = static_cast<uint8_t>(csLen);
    cfg[acHdrAt + 6] = static_cast<uint8_t>(csLen >> 8);
    return cfg;
}

} // namespace uac::uac1
