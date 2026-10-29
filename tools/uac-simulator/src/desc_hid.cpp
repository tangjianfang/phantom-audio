// HID Consumer Volume 描述符实现
#include <uac/desc_hid.h>
#include <uac/usb_desc.h>

namespace uac::hid {

std::vector<std::uint8_t> build_report_desc() {
    return {
        0x05, 0x0C,       // Usage Page (Consumer)
        0x09, 0x01,       // Usage (Consumer Control)
        0xA1, 0x01,       // Collection (Application)
        0x85, 0x01,       //   Report ID (1)
        0x15, 0x00,       //   Logical Minimum (0)
        0x25, 0x01,       //   Logical Maximum (1)
        0x75, 0x01,       //   Report Size (1)
        0x95, 0x03,       //   Report Count (3)
        0x09, 0xE9,       //   Usage (Volume Increment)
        0x09, 0xEA,       //   Usage (Volume Decrement)
        0x09, 0xE2,       //   Usage (Mute)
        0x81, 0x02,       //   Input (Data, Var, Abs)
        0x75, 0x05,       //   Report Size (5) —— 补齐到字节
        0x95, 0x01,       //   Report Count (1)
        0x81, 0x03,       //   Input (Constant) —— 填充
        0xC0,             // End Collection
    };
}

HidInterfaceDesc build_hid_descs() {
    HidInterfaceDesc d;
    const auto rep = build_report_desc();
    const std::uint16_t repLen = static_cast<std::uint16_t>(rep.size());
    d.hid_class_desc = {0x09, 0x21, 0x11, 0x01, // bcdHID 0x0111
                        0x00,                    // bCountryCode
                        0x01,                    // bNumDescriptors
                        0x22,                    // bDescriptorType: Report
                        static_cast<std::uint8_t>(repLen & 0xff),
                        static_cast<std::uint8_t>(repLen >> 8)};
    d.report_desc = rep;
    return d;
}

std::vector<std::uint8_t> encode_report(std::uint8_t keys) {
    return {0x01, static_cast<std::uint8_t>(keys & 0x07)};
}

std::vector<std::uint8_t> append_hid_interface(const std::vector<std::uint8_t>& uacCfg,
                                               std::uint8_t hidInterfaceNumber) {
    desc::Builder b;
    auto cfg = uacCfg;
    // 修正头部：bNumInterfaces+1；wTotalLength 由 build() 回填
    const std::uint8_t oldN = cfg[4];
    desc::Builder out;
    // 先拷贝既有字节再追加 —— Builder 无逐字节接口，这里直接手写：
    std::vector<std::uint8_t> v(uacCfg.begin(), uacCfg.end());
    v[4] = static_cast<std::uint8_t>(oldN + 1);
    // HID 接口描述符
    const std::uint8_t ifc[9] = {0x09, 0x04, hidInterfaceNumber, 0x00, 0x01,
                                 0x03 /*HID*/, 0x00, 0x00, 0x00};
    v.insert(v.end(), ifc, ifc + 9);
    // HID 类描述符（仅引用报告描述符；报告本体经 GET_DESCRIPTOR(Report) 获取，不嵌入 config）
    auto hd = build_hid_descs();
    v.insert(v.end(), hd.hid_class_desc.begin(), hd.hid_class_desc.end());
    // 中断 IN EP 0x82（bInterval=1，UDE 适配会提升到 4）
    const std::uint8_t ep[7] = {0x07, 0x05, 0x82, 0x03, 0x02, 0x00 /*wMaxPacketSize=2*/, 0x01};
    v.insert(v.end(), ep, ep + 7);
    // 回填 wTotalLength
    const std::uint16_t total = static_cast<std::uint16_t>(v.size());
    v[2] = static_cast<std::uint8_t>(total & 0xff);
    v[3] = static_cast<std::uint8_t>(total >> 8);
    return v;
}

} // namespace uac::hid
