#pragma once
// HID Consumer Volume 复合接口（R6 回退通道）：Consumer Control 页(0x0C) 的
// Volume Increment/Decrement/Mute 三个键，报告 ID 1，1 字节位域报告。
#include <cstdint>
#include <vector>

namespace uac::hid {

// 报告位定义（report id 1 的 bit0/1/2）
constexpr std::uint8_t kVolUp = 0x01, kVolDown = 0x02, kMute = 0x04;

// 标准 HID 接口的 class-specific 描述符：HID 类描述符（9B）+ 报告描述符
// bInterfaceNumber 由调用方指定（追加在音频接口之后）
struct HidInterfaceDesc {
    std::vector<std::uint8_t> hid_class_desc;   // 9B：bcdHID 0x0111 / 国家码 / 数目 / 报告类型 0x22 / 长度
    std::vector<std::uint8_t> report_desc;      // 报告描述符本体
};
HidInterfaceDesc build_hid_descs();

// 报告描述符：Consumer Control（Usage Page 0x0C）最小实现
//   Report ID 1，3 个按钮位（Vol+ / Vol- / Mute）
std::vector<std::uint8_t> build_report_desc();

// 输入报告编码：{reportId, 位域}
std::vector<std::uint8_t> encode_report(std::uint8_t keys);

// 组合进 UAC1 配置：在 config 末尾追加 HID 接口（alt0）+ 中断 IN EP 0x82
// 返回新配置；同时更新 config 头的 bNumInterfaces
std::vector<std::uint8_t> append_hid_interface(const std::vector<std::uint8_t>& uacCfg,
                                               std::uint8_t hidInterfaceNumber);

} // namespace uac::hid
