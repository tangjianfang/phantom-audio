// MS OS 2.0 + 多语言字符串测试（长度自洽黄金）
#include <doctest/doctest.h>
#include <uac/msos20.h>
#include <uac/usb_desc.h>

using namespace uac;
using u8 = std::uint8_t;

TEST_CASE("langid table layout") {
    auto t = msos20::build_langid_table({0x0409, 0x0804});
    REQUIRE(t.size() == 6);
    CHECK(t[0] == 6);
    CHECK(t[1] == 0x03);
    CHECK(t[2] == 0x09); CHECK(t[3] == 0x04); // 0x0409 LE
    CHECK(t[4] == 0x04); CHECK(t[5] == 0x08); // 0x0804 LE
}

TEST_CASE("ms os 2.0 descriptor set: lengths self-consistent") {
    auto s = msos20::build_descriptor_set();
    REQUIRE(s.size() >= 10);
    const auto total = static_cast<std::uint16_t>(s[8] | (s[9] << 8));
    CHECK(total == s.size());
    CHECK(s[0] == 10); CHECK(s[1] == 0); // wLength=10
    CHECK(s[2] == 0x00); CHECK(s[3] == 0x00); // wDescriptorType=SET_HEADER
    // 两个属性段长度之和 = total - 10
    const auto p1len = static_cast<std::uint16_t>(s[10] | (s[11] << 8));
    const auto p2at = 10 + p1len;
    const auto p2len = static_cast<std::uint16_t>(s[p2at] | (s[p2at + 1] << 8));
    CHECK(p1len + p2len + 10 == total);
    // 属性类型与数据类型
    CHECK(s[14] == 0x04); // REG_DWORD
    // 属性名 UTF-16LE 以 "T1_" 开头
    CHECK(s[18] == 'T'); CHECK(s[19] == 0);
}

TEST_CASE("bos descriptor wraps set length") {
    auto bos = msos20::build_bos(0x01 /*vendorCode*/);
    REQUIRE(bos.size() == 33); // 5 + 28
    CHECK(bos[0] == 5); CHECK(bos[1] == 0x0F);
    const auto bosTotal = static_cast<std::uint16_t>(bos[2] | (bos[3] << 8));
    CHECK(bosTotal == bos.size());
    CHECK(bos[4] == 1);           // 1 个能力
    CHECK(bos[5] == 28);          // bLength
    CHECK(bos[6] == 0x10);        // PLATFORM
    CHECK(bos[7] == 0x05);        // bDevCapabilityType: PLATFORM
    CHECK(bos[8] == 0xDF);        // MS OS 2.0 UUID 首字节
    const auto setLen = static_cast<std::uint16_t>(bos[29] | (bos[30] << 8));
    auto set = msos20::build_descriptor_set();
    CHECK(setLen == set.size());
    CHECK(bos[31] == 0x01); // vendorCode
}

TEST_CASE("multi-language strings end-to-end via descriptors") {
    // 索引 0 = LANGID 表；0x0409 与 0x0804 各自文本
    auto lang = msos20::build_langid_table({0x0409});
    auto en = desc::string_desc("Phantom Audio Labs");
    CHECK(lang[0] == 4);
    REQUIRE(en.size() > 4);
    CHECK(en[1] == 0x03);
}
