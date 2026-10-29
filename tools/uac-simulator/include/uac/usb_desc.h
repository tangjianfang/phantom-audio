#pragma once
// USB 标准描述符构建/解析。
// 注意：USB 描述符字段为小端（与 USB/IP 协议的大端相反）。
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace uac::desc {

using u8 = std::uint8_t;
using u16 = std::uint16_t;

struct DeviceDesc {
    u16 bcdUSB = 0x0110;
    u8 bDeviceClass = 0, bDeviceSubClass = 0, bDeviceProtocol = 0;
    u8 bMaxPacketSize0 = 8;
    u16 idVendor = 0, idProduct = 0, bcdDevice = 0;
    u8 iManufacturer = 1, iProduct = 2, iSerialNumber = 0;
    u8 bNumConfigurations = 1;
};
std::vector<u8> encode_device_desc(const DeviceDesc&);

// 配置描述符线性拼装器：按 USB 顺序追加段，build() 回填 wTotalLength。
class Builder {
public:
    // 配置描述符头（wTotalLength 先占位，build 时回填）
    Builder& config(u8 bNumInterfaces, u8 bConfigurationValue, u8 bmAttributes, u8 bMaxPower_ma_div2);
    Builder& interface_(u8 bInterfaceNumber, u8 bAlternateSetting, u8 bNumEndpoints,
                        u8 bInterfaceClass, u8 bInterfaceSubClass, u8 bInterfaceProtocol,
                        u8 iInterface = 0);
    // 标准端点（length=7）
    Builder& endpoint(u8 bEndpointAddress, u8 bmAttributes, u16 wMaxPacketSize, u8 bInterval);
    // 音频等时端点（length=9，含 bRefresh/bSynchAddress）
    Builder& endpoint_audio(u8 bEndpointAddress, u8 bmAttributes, u16 wMaxPacketSize, u8 bInterval,
                            u8 bRefresh = 0, u8 bSynchAddress = 0);
    Builder& raw(std::span<const u8> bytes); // 任意 class-specific 段 / 附加字节
    std::vector<u8> build() const;

private:
    std::vector<u8> buf_;
    size_t config_at_ = 0; // wTotalLength 回填位置
};

// UTF-16LE 字符串描述符（bLength/bDescriptorType(3)/UTF-16LE，含结尾可选 NUL 不含）
std::vector<u8> string_desc(std::string_view ascii);

// ---- 配置描述符解析（结构视图，lint 与测试用） ----
struct IfaceView {
    u8 num, alt, nEndpoints, cls, sub, proto;
    size_t offset; // 该接口描述符起始偏移
};
struct EpView {
    u8 address, attributes, interval, length;
    u16 maxPacketSize;
    size_t offset;
    size_t iface_index; // 所属 IfaceView 下标
};
struct ConfigView {
    u16 wTotalLength = 0;
    u8 bNumInterfaces = 0, bConfigurationValue = 0, bmAttributes = 0, bMaxPower = 0;
    std::vector<IfaceView> interfaces;
    std::vector<EpView> endpoints;
};
// 严格解析：长度一致、描述符链完整，失败返回 nullopt
std::optional<ConfigView> parse_config(std::span<const u8> cfg);

} // namespace uac::desc
