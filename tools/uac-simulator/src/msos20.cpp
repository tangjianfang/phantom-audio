// MS OS 2.0 描述符实现（长度全部由内容推导，测试校验自洽）
#include <uac/msos20.h>
#include <string>
#include <cstring>

namespace uac::msos20 {

namespace {
void le16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void le32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
std::vector<std::uint8_t> utf16le(const char* s) {
    std::vector<std::uint8_t> v;
    for (const char* p = s; *p; ++p) {
        v.push_back(static_cast<std::uint8_t>(*p));
        v.push_back(0x00);
    }
    return v;
}
// Registry Property 描述符（REG_DWORD）
std::vector<std::uint8_t> registry_property(const std::string& name, std::uint32_t value) {
    auto n = utf16le(name.c_str());
    const std::uint16_t len = static_cast<std::uint16_t>(14 + n.size());
    std::vector<std::uint8_t> v;
    le16(v, len);
    le16(v, 0x0004); // MS_OS_20_FEATURE_REGISTRY_PROPERTY
    le16(v, 0x0004); // REG_DWORD
    le16(v, static_cast<std::uint16_t>(n.size()));
    v.insert(v.end(), n.begin(), n.end());
    le16(v, 6); // wPropertyDataLength（4B 数据 + 长度字段 2B）
    le32(v, value);
    return v;
}
} // namespace

std::vector<std::uint8_t> build_bos(std::uint8_t vendorCode) {
    static const std::uint8_t kMsOs20Uuid[16] = {0xDF, 0x60, 0xDD, 0xF8, 0x3B, 0x45, 0x55,
                                                 0x9D, 0xC6, 0x3D, 0x6E, 0xF7, 0x7B, 0x29,
                                                 0x81, 0x94};
    std::vector<std::uint8_t> bos;
    const std::size_t total = 5 + 28;
    bos.push_back(0x05);
    bos.push_back(0x0F); // BOS
    le16(bos, static_cast<std::uint16_t>(total));
    bos.push_back(0x01);
    bos.push_back(28);   // bLength
    bos.push_back(0x10); // bDescriptorType: DEVICE CAPABILITY
    bos.push_back(0x05); // bDevCapabilityType: PLATFORM
    bos.insert(bos.end(), kMsOs20Uuid, kMsOs20Uuid + 16);
    bos.push_back(0x00); // bReserved
    le32(bos, 0x06030000);
    auto set = build_descriptor_set();
    le16(bos, static_cast<std::uint16_t>(set.size())); // wMSOSDescriptorSetTotalLength
    bos.push_back(vendorCode);
    bos.push_back(0x00);
    return bos;
}

std::vector<std::uint8_t> build_descriptor_set() {
    // SetHeader + 两个 REG_DWORD：T1_J01_Color=RED(0x00ff0000)、
    // T1_J01_ChannelMapping=SPEAKER_FRONT_RIGHT(0x00000002)
    auto p1 = registry_property("T1_J01_Color", 0x00ff0000u);
    auto p2 = registry_property("T1_J01_ChannelMapping", 0x00000002u);
    const std::uint16_t total = static_cast<std::uint16_t>(10 + p1.size() + p2.size());
    std::vector<std::uint8_t> v;
    le16(v, 10);
    le16(v, 0x0000);     // MS_OS_20_SET_HEADER_DESCRIPTOR
    le32(v, 0x06030000); // dwWindowsVersion
    le16(v, total);
    v.insert(v.end(), p1.begin(), p1.end());
    v.insert(v.end(), p2.begin(), p2.end());
    return v;
}

std::vector<std::uint8_t> build_langid_table(const std::vector<std::uint16_t>& langids) {
    std::vector<std::uint8_t> v;
    v.push_back(static_cast<std::uint8_t>(2 + 2 * langids.size()));
    v.push_back(0x03);
    for (auto l : langids) {
        v.push_back(static_cast<std::uint8_t>(l & 0xff));
        v.push_back(static_cast<std::uint8_t>(l >> 8));
    }
    return v;
}

} // namespace uac::msos20
