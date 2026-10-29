// 描述符库测试：黄金基准 = usbip-win2 issue #35 的 C-Media 0D8C:0103 真机 dump
#include <doctest/doctest.h>
#include <uac/usb_desc.h>

using namespace uac::desc;

namespace {
// 116 字节配置描述符（USBView dump，字节级黄金）
constexpr u8 GOLDEN_CMEDIA_CFG[116] = {
    0x09,0x02,0x74,0x00,0x02,0x01,0x00,0x80,0xFA,
    0x09,0x04,0x00,0x00,0x00,0x01,0x01,0x00,0x00,
    0x09,0x24,0x01,0x00,0x01,0x2B,0x00,0x01,0x01,
    0x0C,0x24,0x02,0x01,0x01,0x01,0x00,0x02,0x03,0x00,0x00,0x00,
    0x0D,0x24,0x06,0x0D,0x01,0x02,0x01,0x02,0x02,0x00,0x02,0x00,0x00,
    0x09,0x24,0x03,0x03,0x01,0x03,0x00,0x0D,0x00,
    0x09,0x04,0x01,0x00,0x00,0x01,0x02,0x00,0x00,
    0x09,0x04,0x01,0x01,0x01,0x01,0x02,0x00,0x00,
    0x07,0x24,0x01,0x01,0x01,0x01,0x00,
    0x0E,0x24,0x02,0x01,0x02,0x02,0x10,0x02,0x44,0xAC,0x00,0x80,0xBB,0x00,
    0x09,0x05,0x06,0x09,0xC0,0x00,0x01,0x00,0x00,
    0x07,0x25,0x01,0x01,0x00,0x00,0x00,
};
// 18 字节设备描述符
constexpr u8 GOLDEN_CMEDIA_DEV[18] = {
    0x12,0x01,0x10,0x01,0x00,0x00,0x00,0x08,0x8C,0x0D,0x03,0x01,0x10,0x00,0x01,0x02,0x00,0x01,
};

// 从黄金字节重建等价配置（同时是 UAC1 模板的样例拼装）
std::vector<u8> rebuild_cmedia_cfg() {
    constexpr u8 ac_header[9]  = {0x09,0x24,0x01,0x00,0x01,0x2B,0x00,0x01,0x01};
    constexpr u8 input_term[12]= {0x0C,0x24,0x02,0x01,0x01,0x01,0x00,0x02,0x03,0x00,0x00,0x00};
    constexpr u8 feature_unit[13]={0x0D,0x24,0x06,0x0D,0x01,0x02,0x01,0x02,0x02,0x00,0x02,0x00,0x00};
    constexpr u8 output_term[9]= {0x09,0x24,0x03,0x03,0x01,0x03,0x00,0x0D,0x00};
    constexpr u8 as_general[7]  = {0x07,0x24,0x01,0x01,0x01,0x01,0x00}; // wFormatTag=0x0001 PCM
    constexpr u8 as_format[14] = {0x0E,0x24,0x02,0x01,0x02,0x02,0x10,0x02,0x44,0xAC,0x00,0x80,0xBB,0x00};
    constexpr u8 cs_ep[7]      = {0x07,0x25,0x01,0x01,0x00,0x00,0x00};

    Builder b;
    b.config(2, 1, 0x80, 0xFA);
    b.interface_(0, 0, 0, 0x01, 0x01, 0x00);              // AC
    b.raw(ac_header).raw(input_term).raw(feature_unit).raw(output_term);
    b.interface_(1, 0, 0, 0x01, 0x02, 0x00);              // AS alt0（零端点）
    b.interface_(1, 1, 1, 0x01, 0x02, 0x00);              // AS alt1
    b.raw(as_general);
    b.raw(as_format);
    b.endpoint_audio(0x06, 0x09, 0x00C0, 0x01);           // iso OUT adaptive 192B/1ms（9B）
    b.raw(cs_ep);
    return b.build();
}
} // namespace

TEST_CASE("device descriptor encode golden") {
    DeviceDesc d;
    d.bcdUSB = 0x0110; d.bMaxPacketSize0 = 8;
    d.idVendor = 0x0D8C; d.idProduct = 0x0103; d.bcdDevice = 0x0010;
    d.iManufacturer = 1; d.iProduct = 2; d.iSerialNumber = 0;
    auto v = encode_device_desc(d);
    REQUIRE(v.size() == 18);
    CHECK(std::equal(v.begin(), v.end(), std::begin(GOLDEN_CMEDIA_DEV)));
}

TEST_CASE("parse golden C-Media config") {
    auto v = parse_config(GOLDEN_CMEDIA_CFG);
    REQUIRE(v.has_value());
    CHECK(v->wTotalLength == 116);
    CHECK(v->bNumInterfaces == 2);
    CHECK(v->bConfigurationValue == 1);
    CHECK(v->bmAttributes == 0x80);
    CHECK(v->bMaxPower == 0xFA);
    REQUIRE(v->interfaces.size() == 3); // IF0alt0, IF1alt0, IF1alt1
    CHECK(v->interfaces[0].num == 0);  CHECK(v->interfaces[0].alt == 0);
    CHECK(v->interfaces[0].cls == 0x01); CHECK(v->interfaces[0].sub == 0x01); // AC
    CHECK(v->interfaces[0].nEndpoints == 0);
    CHECK(v->interfaces[1].num == 1);  CHECK(v->interfaces[1].alt == 0);
    CHECK(v->interfaces[1].nEndpoints == 0);                                 // AS alt0 零端点
    CHECK(v->interfaces[2].num == 1);  CHECK(v->interfaces[2].alt == 1);
    CHECK(v->interfaces[2].sub == 0x02);                                     // AS
    CHECK(v->interfaces[2].nEndpoints == 1);
    REQUIRE(v->endpoints.size() == 1);
    CHECK(v->endpoints[0].address == 0x06);
    CHECK(v->endpoints[0].attributes == 0x09);   // ISO + Adaptive
    CHECK(v->endpoints[0].maxPacketSize == 192); // 48k*2ch*2B / 1ms
    CHECK(v->endpoints[0].interval == 1);
    CHECK(v->endpoints[0].length == 9);          // 音频等时端点 9B
    CHECK(v->endpoints[0].iface_index == 2);
}

TEST_CASE("builder rebuild reproduces golden bytes exactly") {
    auto rebuilt = rebuild_cmedia_cfg();
    REQUIRE(rebuilt.size() == 116);
    CHECK(std::equal(rebuilt.begin(), rebuilt.end(), std::begin(GOLDEN_CMEDIA_CFG)));
}

TEST_CASE("string descriptor UTF-16LE") {
    auto s = string_desc("A");
    REQUIRE(s.size() == 4);
    CHECK(s[0] == 0x04); CHECK(s[1] == 0x03); CHECK(s[2] == 'A'); CHECK(s[3] == 0x00);
    auto hello = string_desc("Hi");
    REQUIRE(hello.size() == 6);
    CHECK(hello[2] == 'H'); CHECK(hello[4] == 'i');
}

TEST_CASE("parse rejects malformed config") {
    std::vector<u8> short_cfg(GOLDEN_CMEDIA_CFG, GOLDEN_CMEDIA_CFG + 50);
    CHECK_FALSE(parse_config(short_cfg).has_value());           // 截断
    auto bad = std::vector<u8>(std::begin(GOLDEN_CMEDIA_CFG), std::end(GOLDEN_CMEDIA_CFG));
    bad[2] = 0x60;                                               // wTotalLength 错误
    CHECK_FALSE(parse_config(bad).has_value());
    auto zero = std::vector<u8>{};
    CHECK_FALSE(parse_config(zero).has_value());
}
