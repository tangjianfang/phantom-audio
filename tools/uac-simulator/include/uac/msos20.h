#pragma once
// MS OS 2.0 描述符（BOS 入口）+ 多语言字符串支持。
// 用途：向 usbaudio2.sys 注入 jack 信息（通道映射/颜色）等注册表属性；
//       多语言字符串（LANGID 表 + 每语言索引）。
#include <cstdint>
#include <map>
#include <vector>

namespace uac::msos20 {

// BOS 描述符（GET_DESCRIPTOR type 0x0F）：头 + WebUSB/平台能力（这里只放 MS OS 2.0 平台）
// vendorCode 由调用方选定（如 0x01）；MS OS 描述符集经 vendor-specific 控制请求获取
std::vector<std::uint8_t> build_bos(std::uint8_t vendorCode);

// MS OS 2.0 描述符集：SetHeader + Registry Property（T1_J01_ChannelMapping = 前右）
// 参照 Microsoft 官方文档示例（usb-2-0-audio-drivers）参数化
std::vector<std::uint8_t> build_descriptor_set();

// 多语言字符串描述符：索引 0 → LANGID 表；各语言文本 → 各自索引
// 简化模型：一组 (LANGID, 文本) 列表；索引 0 为 LANGID 表，随后按序 1..n
std::vector<std::uint8_t> build_langid_table(const std::vector<std::uint16_t>& langids);

} // namespace uac::msos20
