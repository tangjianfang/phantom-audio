// USB 标准描述符构建/解析实现（描述符字段小端）
#include <uac/usb_desc.h>

#include <cstring>

namespace uac::desc {

namespace {
void put16(std::vector<u8>& v, u16 x) {
    v.push_back(static_cast<u8>(x & 0xff));
    v.push_back(static_cast<u8>(x >> 8));
}
u16 get16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
} // namespace

std::vector<u8> encode_device_desc(const DeviceDesc& d) {
    return {
        0x12, 0x01,
        static_cast<u8>(d.bcdUSB & 0xff), static_cast<u8>(d.bcdUSB >> 8),
        d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol, d.bMaxPacketSize0,
        static_cast<u8>(d.idVendor & 0xff), static_cast<u8>(d.idVendor >> 8),
        static_cast<u8>(d.idProduct & 0xff), static_cast<u8>(d.idProduct >> 8),
        static_cast<u8>(d.bcdDevice & 0xff), static_cast<u8>(d.bcdDevice >> 8),
        d.iManufacturer, d.iProduct, d.iSerialNumber, d.bNumConfigurations,
    };
}

Builder& Builder::config(u8 bNumInterfaces, u8 bConfigurationValue, u8 bmAttributes,
                         u8 bMaxPower_ma_div2) {
    config_at_ = buf_.size();
    buf_.insert(buf_.end(), {0x09, 0x02, 0x00, 0x00 /*wTotalLength 占位*/, bNumInterfaces,
                             bConfigurationValue, 0x00 /*iConfiguration*/, bmAttributes,
                             bMaxPower_ma_div2});
    return *this;
}

Builder& Builder::interface_(u8 bInterfaceNumber, u8 bAlternateSetting, u8 bNumEndpoints,
                             u8 bInterfaceClass, u8 bInterfaceSubClass, u8 bInterfaceProtocol,
                             u8 iInterface) {
    buf_.insert(buf_.end(), {0x09, 0x04, bInterfaceNumber, bAlternateSetting, bNumEndpoints,
                             bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface});
    return *this;
}

Builder& Builder::endpoint(u8 bEndpointAddress, u8 bmAttributes, u16 wMaxPacketSize, u8 bInterval) {
    buf_.insert(buf_.end(), {0x07, 0x05, bEndpointAddress, bmAttributes});
    put16(buf_, wMaxPacketSize);
    buf_.push_back(bInterval);
    return *this;
}

Builder& Builder::endpoint_audio(u8 bEndpointAddress, u8 bmAttributes, u16 wMaxPacketSize,
                                 u8 bInterval, u8 bRefresh, u8 bSynchAddress) {
    buf_.insert(buf_.end(), {0x09, 0x05, bEndpointAddress, bmAttributes});
    put16(buf_, wMaxPacketSize);
    buf_.push_back(bInterval);
    buf_.push_back(bRefresh);
    buf_.push_back(bSynchAddress);
    return *this;
}

Builder& Builder::raw(std::span<const u8> bytes) {
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    return *this;
}

std::vector<u8> Builder::build() const {
    auto out = buf_;
    const auto total = static_cast<u16>(out.size());
    out[config_at_ + 2] = static_cast<u8>(total & 0xff);
    out[config_at_ + 3] = static_cast<u8>(total >> 8);
    return out;
}

std::vector<u8> string_desc(std::string_view ascii) {
    std::vector<u8> v;
    v.push_back(static_cast<u8>(2 + 2 * ascii.size()));
    v.push_back(0x03);
    for (char c : ascii) {
        v.push_back(static_cast<u8>(c));
        v.push_back(0x00); // ASCII → UTF-16LE
    }
    return v;
}

std::optional<ConfigView> parse_config(std::span<const u8> cfg) {
    if (cfg.size() < 9 || cfg[0] < 9 || cfg[1] != 0x02) return std::nullopt;
    ConfigView v;
    v.wTotalLength = get16(cfg.data() + 2);
    v.bNumInterfaces = cfg[4];
    v.bConfigurationValue = cfg[5];
    v.bmAttributes = cfg[7];
    v.bMaxPower = cfg[8];
    if (v.wTotalLength != cfg.size()) return std::nullopt;

    size_t off = 0;
    std::optional<size_t> cur_iface;
    while (off < cfg.size()) {
        const u8 len = cfg[off];
        if (len < 2 || off + len > cfg.size()) return std::nullopt;
        const u8 type = cfg[off + 1];
        switch (type) {
        case 0x04: { // interface
            if (len < 9) return std::nullopt;
            IfaceView iv{cfg[off + 2], cfg[off + 3], cfg[off + 4], cfg[off + 5],
                         cfg[off + 6], cfg[off + 7], off};
            v.interfaces.push_back(iv);
            cur_iface = v.interfaces.size() - 1;
            break;
        }
        case 0x05: { // endpoint
            if (len < 7) return std::nullopt;
            if (!cur_iface) return std::nullopt; // 端点必须挂在接口下
            EpView ev{cfg[off + 2], cfg[off + 3], cfg[off + 6], len, get16(cfg.data() + off + 4),
                      off, *cur_iface};
            v.endpoints.push_back(ev);
            break;
        }
        default:
            break; // class-specific（0x24/0x25 等）与其他段跳过
        }
        off += len;
    }
    if (off != v.wTotalLength) return std::nullopt;
    return v;
}

} // namespace uac::desc
