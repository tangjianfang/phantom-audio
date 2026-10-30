// UAC2 模板实现（ADC-2 描述符布局）
#include <uac/desc_uac2.h>
#include <uac/ude_adapter.h>
#include <algorithm>

namespace uac::uac2 {

namespace {
void le16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void le32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

// ---- AC 内 class-specific 段（ADC-2 4.7.2）----
// Clock Source：bLength 8，内部可编程时钟，CUR_FREQ 读写
std::vector<std::uint8_t> clock_source(std::uint8_t id) {
    return {0x08, 0x24, 0x0A, id, 0x03 /*internal programmable*/, 0x03 /*freq RW*/,
            0x00 /*bAssocTerminal*/, 0x00 /*iClockSource*/};
}
// Input Terminal：bLength 17
std::vector<std::uint8_t> input_terminal(std::uint8_t id, std::uint16_t type, std::uint8_t clockId,
                                         std::uint8_t channels) {
    std::vector<std::uint8_t> v{0x11, 0x24, 0x02, id};
    le16(v, type);
    v.push_back(0x00); // bAssocTerminal
    v.push_back(clockId);
    v.push_back(channels);
    le32(v, 0x00000003); // bmChannelConfig: FL|FR
    v.push_back(0x00);   // iChannelNames
    v.push_back(0x00);   // bmControls lo（copy protect 无）
    v.push_back(0x00);   // bmControls hi
    v.push_back(0x00);   // iTerminal
    return v;
}
// Output Terminal：bLength 12
std::vector<std::uint8_t> output_terminal(std::uint8_t id, std::uint16_t type, std::uint8_t sourceId,
                                          std::uint8_t clockId) {
    std::vector<std::uint8_t> v{0x0C, 0x24, 0x03, id};
    le16(v, type);
    v.push_back(0x00); // bAssocTerminal
    v.push_back(sourceId);
    v.push_back(clockId);
    v.push_back(0x00); // bmControls lo
    v.push_back(0x00); // bmControls hi
    v.push_back(0x00); // iTerminal
    return v;
}
// Feature Unit：bLength = 6 + 4*(ch+1)；bmaControls 双位编码（11=RW）：
// D1..0 Mute(RW=0x3)，D3..D2 Volume(RW=0xC)
std::vector<std::uint8_t> feature_unit(std::uint8_t id, std::uint8_t sourceId, std::uint8_t ch) {
    std::vector<std::uint8_t> v{0x00, 0x24, 0x06, id, sourceId};
    le32(v, 0x0000000F); // master: Mute+Volume RW
    for (std::uint8_t c = 0; c < ch; ++c) le32(v, 0x0000000C); // 各声道 Volume RW
    v.push_back(0x00); // iFeature
    v[0] = static_cast<std::uint8_t>(v.size());
    return v;
}
// ---- AS 内 class-specific 段（ADC-2 4.9/4.10）----
// AS General：bLength 15
std::vector<std::uint8_t> as_general(std::uint8_t terminalLink, std::uint8_t channels) {
    std::vector<std::uint8_t> v{0x0F, 0x24, 0x01, terminalLink, 0x00 /*bmControls*/,
                                0x01 /*bFormatType I*/};
    le32(v, 0x00000001); // bmFormats: PCM
    v.push_back(channels);
    le32(v, 0x00000003);
    return v;
}
// Type I 格式（UAC2）：bLength 6 —— 采样率不在描述符中（走 Clock Source）
std::vector<std::uint8_t> type_i_format(std::uint8_t subslot, std::uint8_t bits) {
    return {0x06, 0x24, 0x02, 0x01, subslot, bits};
}
// CS AS 等时音频数据端点：bLength 8
std::vector<std::uint8_t> cs_iso_ep() {
    return {0x08, 0x24, 0x01, 0x00 /*bmAttributes*/, 0x00 /*bmControls*/,
            0x00 /*bLockDelayUnits*/, 0x00, 0x00 /*wLockDelay*/};
}
} // namespace

std::uint16_t max_packet_size(const Uac2Profile& p) {
    const std::uint32_t maxRate = *std::max_element(p.rates.begin(), p.rates.end());
    return static_cast<std::uint16_t>((maxRate * p.channels * p.pcm.subslot + 999) / 1000);
}

desc::DeviceDesc build_device_desc(const Uac2Profile& p) {
    desc::DeviceDesc d;
    d.bcdUSB = 0x0200;
    d.bDeviceClass = 0xEF; // IAD 复合
    d.bDeviceSubClass = 0x02;
    d.bDeviceProtocol = 0x01;
    d.bMaxPacketSize0 = 64;
    d.idVendor = p.vid;
    d.idProduct = p.pid;
    d.bcdDevice = p.bcdDevice;
    d.iManufacturer = 1;
    d.iProduct = 2;
    d.iSerialNumber = p.serial.empty() ? 0 : 3;
    return d;
}

std::vector<std::uint8_t> build_device_desc_encoded(const Uac2Profile& p) {
    return desc::encode_device_desc(build_device_desc(p));
}

std::vector<std::uint8_t> build_qualifier_desc(const Uac2Profile& p) {
    // HS 设备限定描述符：10B，速度字段全 0（仅 HS 一档）
    (void)p;
    return {0x0A, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00};
}

std::map<std::uint8_t, std::vector<std::uint8_t>> build_string_descs(const Uac2Profile& p) {
    std::map<std::uint8_t, std::vector<std::uint8_t>> m;
    m[1] = desc::string_desc(p.manufacturer);
    m[2] = desc::string_desc(p.product);
    if (!p.serial.empty()) m[3] = desc::string_desc(p.serial);
    return m;
}

std::vector<std::uint8_t> build_config_desc(const Uac2Profile& p) {
    using desc::Builder;
    const bool mic = p.micIn, spk = p.spkOut;
    const std::uint8_t nIf = 1 + (mic ? 1 : 0) + (spk ? 1 : 0);
    const std::uint16_t maxPkt = max_packet_size(p);

    Builder b;
    b.config(nIf, 1, 0x80, 0xFA);
    // IAD（音频功能）：bLength 8
    {
        std::vector<std::uint8_t> iad{0x08, 0x0B, kInterfaceAC, static_cast<std::uint8_t>(nIf),
                                      0x01 /*class audio*/, 0x00 /*subclass UNDEF*/,
                                      0x20 /*protocol ADC2.0*/, 0x00 /*iFunction*/};
        b.raw(iad);
    }
    // AC 接口
    b.interface_(kInterfaceAC, 0, p.acInterruptEp ? 1 : 0, 0x01, 0x01, 0x20);
    {
        // ADC-2 4.7.2: bcdADC[3..4] bCategory[5] wTotalLength[6..7] bmControls[8]
        std::vector<std::uint8_t> h{0x09, 0x24, 0x01,
                                    0x00, 0x02,   // bcdADC 0x0200
                                    0x00,         // bCategory: UNDEFINED (IO)
                                    0x00, 0x00,   // wTotalLength (backfilled)
                                    0x00};        // bmControls
        b.raw(h);
    }
    b.raw(clock_source(kClock));
    if (spk) {
        b.raw(input_terminal(kItSpeaker, 0x0101, kClock, p.channels));
        b.raw(feature_unit(kFuSpeaker, kItSpeaker, p.channels));
        b.raw(output_terminal(kOtSpeaker, 0x0301, kFuSpeaker, kClock));
    }
    if (mic) {
        b.raw(input_terminal(kItMic, 0x0201, kClock, p.channels));
        b.raw(feature_unit(kFuMic, kItMic, p.channels));
        b.raw(output_terminal(kOtMic, 0x0101, kFuMic, kClock));
    }
    if (p.acInterruptEp)
        b.endpoint(0x81, 0x03, 6 /*UAC2 Status Word 6B*/, 4 /*HS 1ms*/);

    // AS 播放（异步 OUT + 显式反馈）
    if (spk) {
        b.interface_(kInterfaceSpk, 0, 0, 0x01, 0x02, 0x20);
        b.interface_(kInterfaceSpk, 1, p.feedbackEp ? 2 : 1, 0x01, 0x02, 0x20);
        b.raw(as_general(kItSpeaker, p.channels));
        b.raw(type_i_format(p.pcm.subslot, p.pcm.bits));
        b.endpoint_audio(0x06, 0x05 /*iso async OUT*/, maxPkt, 4);
        b.raw(cs_iso_ep());
        if (p.feedbackEp) {
            b.endpoint(0x84, 0x11 /*iso + usage feedback*/, 4, 4);
            b.raw(cs_iso_ep());
        }
    }
    // AS 采集（异步 IN）
    if (mic) {
        b.interface_(kInterfaceMic, 0, 0, 0x01, 0x02, 0x20);
        b.interface_(kInterfaceMic, 1, 1, 0x01, 0x02, 0x20);
        b.raw(as_general(kOtMic, p.channels));
        b.raw(type_i_format(p.pcm.subslot, p.pcm.bits));
        b.endpoint_audio(0x86, 0x05 /*iso async IN*/, maxPkt, 4);
        b.raw(cs_iso_ep());
    }

    auto cfg = b.build();
    // 回填 AC class-specific wTotalLength（偏移 6..7）：IAD(8)+IF(9) 之后
    const std::size_t acHdrAt = 9 /*config*/ + 8 /*IAD*/ + 9 /*IF0*/;
    std::uint16_t csLen = 9 /*header*/ + 8 /*clock source*/;
    if (spk) csLen += 17 + static_cast<std::uint16_t>(6 + 4 * (p.channels + 1)) + 12;
    if (mic) csLen += 17 + static_cast<std::uint16_t>(6 + 4 * (p.channels + 1)) + 12;
    cfg[acHdrAt + 6] = static_cast<std::uint8_t>(csLen);
    cfg[acHdrAt + 7] = static_cast<std::uint8_t>(csLen >> 8);
    return cfg;
}

std::vector<ude::LintError> lint_uac2(const desc::ConfigView& v,
                                      const std::vector<std::uint8_t>& rawCfg) {
    std::vector<ude::LintError> errs;
    // 单一 Clock Source：0x24 0x0A 段只出现一次
    int clockCount = 0;
    for (std::size_t off = 0; off + 8 <= rawCfg.size();) {
        const std::uint8_t len = rawCfg[off];
        if (len < 2 || off + len > rawCfg.size()) break;
        if (rawCfg[off + 1] == 0x24 && off + 2 < rawCfg.size() && rawCfg[off + 2] == 0x0A)
            ++clockCount;
        off += len;
    }
    if (clockCount != 1)
        errs.push_back({"uac2-single-clock-source", "Clock Source 数量=" + std::to_string(clockCount)});

    // 异步 OUT 数据端点必须有同 alt 的反馈 IN 端点
    for (auto&& iv : v.interfaces) {
        if (!(iv.cls == 0x01 && iv.sub == 0x02 && iv.alt != 0)) continue;
        bool asyncOut = false, feedbackIn = false;
        for (auto&& e : v.endpoints) {
            if (e.iface_index >= v.interfaces.size()) continue;
            const auto& owner = v.interfaces[e.iface_index];
            if (owner.num != iv.num || owner.alt != iv.alt) continue;
            const std::uint8_t sync = (e.attributes >> 2) & 0x03;
            const std::uint8_t usage = (e.attributes >> 4) & 0x03;
            if ((e.address & 0x80) == 0 && sync == 0x01) asyncOut = true; // OUT async
            if ((e.address & 0x80) && usage == 0x01) feedbackIn = true;  // IN feedback
        }
        if (asyncOut && !feedbackIn)
            errs.push_back({"uac2-async-out-needs-feedback",
                            "usbaudio2.sys 不支持隐式反馈：异步 OUT 的 alt 缺反馈端点", iv.offset});
    }
    // alt 升序
    std::map<std::uint8_t, int> lastAlt;
    for (auto&& iv : v.interfaces) {
        auto it = lastAlt.find(iv.num);
        if (it != lastAlt.end() && iv.alt <= it->second)
            errs.push_back({"uac2-alt-ascending", "AS alt 序号必须升序", iv.offset});
        lastAlt[iv.num] = iv.alt;
    }
    return errs;
}

} // namespace uac::uac2
