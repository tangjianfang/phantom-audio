// UAC1 模板 + UDE 适配 + lint 测试
#include <doctest/doctest.h>
#include <uac/desc_uac1.h>
#include <uac/ude_adapter.h>

using namespace uac;
using u8 = uint8_t;

namespace {
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
} // namespace

TEST_CASE("ude adapter bumps iso/int interval to >=4 only") {
    auto cfg = std::vector<u8>(std::begin(GOLDEN_CMEDIA_CFG), std::end(GOLDEN_CMEDIA_CFG));
    const auto before = cfg;
    const int changed = ude::adapt_config_for_ude(cfg);
    REQUIRE(changed == 1); // 黄金中仅一个 iso EP
    // EP interval 位置：偏移 100 + 6
    CHECK(cfg[100 + 6] == 4);
    CHECK(before[100 + 6] == 1);
    // 其余字节不变
    auto restored = cfg;
    restored[100 + 6] = 1;
    CHECK(restored == before);
    // 幂等
    CHECK(ude::adapt_config_for_ude(cfg) == 0);
}

TEST_CASE("build full-duplex uac1 config: structure") {
    uac1::Uac1Profile p;
    p.rates = {44100, 48000};
    auto cfg = uac1::build_config_desc(p);
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    CHECK(v->bNumInterfaces == 3); // AC + AS播放 + AS采集
    CHECK(v->interfaces.size() == 5); // AC, AS1 alt0, AS1 alt1, AS2 alt0, AS2 alt1
    CHECK(v->endpoints.size() == 2);  // 0x06 OUT + 0x86 IN
    bool has_out = false, has_in = false;
    for (auto&& e : v->endpoints) {
        if (e.address == 0x06) { has_out = true; CHECK(e.maxPacketSize == 192); } // 48k max
        if (e.address == 0x86) { has_in = true; CHECK((e.attributes & 0x03) == 0x01); }
    }
    CHECK(has_out);
    CHECK(has_in);
    // 两个 AS 的 alt0 都必须零端点
    for (auto&& iv : v->interfaces)
        if (iv.cls == 0x01 && iv.sub == 0x02 && iv.alt == 0) CHECK(iv.nEndpoints == 0);
    // lint 通用规则通过
    CHECK(ude::lint_common(*v).empty());
}

TEST_CASE("speaker-only profile halves interfaces") {
    uac1::Uac1Profile p;
    p.micIn = false;
    auto cfg = uac1::build_config_desc(p);
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    CHECK(v->bNumInterfaces == 2);
    CHECK(v->endpoints.size() == 1);
    CHECK(v->endpoints[0].address == 0x06);
}

TEST_CASE("ac interrupt ep adds int IN endpoint") {
    uac1::Uac1Profile p;
    p.acInterruptEp = true;
    auto cfg = uac1::build_config_desc(p);
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    REQUIRE(v->endpoints.size() == 3);
    int intEp = 0;
    for (auto&& e : v->endpoints)
        if ((e.attributes & 0x03) == 0x03) { intEp++; CHECK(e.address == 0x81); CHECK(e.maxPacketSize == 2); } // UAC1 状态中断 2B
    CHECK(intEp == 1);
    // UDE 适配后中断 EP interval 也会被提升
    auto adapted = cfg;
    ude::adapt_config_for_ude(adapted);
    auto v2 = desc::parse_config(adapted);
    for (auto&& e : v2->endpoints)
        if ((e.attributes & 0x03) == 0x03) CHECK(e.interval >= 4);
}

TEST_CASE("device desc and strings") {
    uac1::Uac1Profile p;
    p.serial = "SIM-001";
    auto d = uac1::build_device_desc(p);
    CHECK(d.idVendor == 0x0D8C);
    auto enc = desc::encode_device_desc(d);
    CHECK(enc.size() == 18);
    CHECK(enc[16] == 3); // iSerialNumber=3（serial 非空）
    auto strings = uac1::build_string_descs(p);
    CHECK(strings.count(1));
    CHECK(strings.count(2));
    CHECK(strings.count(3));
    CHECK(strings.at(2).size() == 2 + 2 * std::string("UAC1 Simulator").size());
}

TEST_CASE("max packet size follows highest rate") {
    uac1::Uac1Profile p;
    p.rates = {44100, 96000};
    CHECK(uac1::max_packet_size(p) == 96000 * 2 * 2 / 1000); // 384
}

TEST_CASE("lint catches as-alt0-with-endpoint") {
    // 构造坏配置：AS alt0 挂一个端点
    desc::Builder b;
    b.config(2, 1, 0x80, 0x32);
    b.interface_(0, 0, 0, 0x01, 0x01, 0x00);
    constexpr u8 ac_hdr[9] = {0x09,0x24,0x01,0x00,0x01,0x2B,0x00,0x01,0x01};
    b.raw(ac_hdr);
    b.interface_(1, 0, 1, 0x01, 0x02, 0x00); // alt0 但声明 1 EP —— 违规
    b.endpoint_audio(0x06, 0x09, 192, 1);
    auto cfg = b.build();
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    auto errs = ude::lint_common(*v);
    REQUIRE(errs.size() >= 1);
    CHECK(errs[0].code == "as-alt0-has-endpoint");
}

TEST_CASE("lint catches missing ac interface") {
    desc::Builder b;
    b.config(1, 1, 0x80, 0x32);
    b.interface_(0, 0, 0, 0x01, 0x02, 0x00); // 只有 AS，无 AC
    auto cfg = b.build();
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    auto errs = ude::lint_common(*v);
    REQUIRE(!errs.empty());
    CHECK(errs[0].code == "missing-audio-control-interface");
}
