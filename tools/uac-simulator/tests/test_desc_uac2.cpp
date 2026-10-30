// UAC2 模板 + lint 测试
#include <doctest/doctest.h>
#include <uac/desc_uac2.h>
#include <uac/ude_adapter.h>

using namespace uac;
using u8 = std::uint8_t;

TEST_CASE("uac2 config structure: IAD + clock + feedback + dual AS") {
    uac2::Uac2Profile p;
    auto cfg = uac2::build_config_desc(p);
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    CHECK(v->bNumInterfaces == 3);
    CHECK(v->interfaces.size() == 5); // AC + spk alt0/1 + mic alt0/1
    CHECK(v->endpoints.size() == 4);  // int 0x81 + spk OUT 0x06 + fb 0x84 + mic IN 0x86

    bool intEp = false, outEp = false, fbEp = false, inEp = false;
    for (auto&& e : v->endpoints) {
        if (e.address == 0x81) { intEp = true; CHECK((e.attributes & 0x03) == 0x03); CHECK(e.maxPacketSize == 6); }
        if (e.address == 0x06) { outEp = true; CHECK(e.attributes == 0x05); CHECK(e.length == 9); }
        if (e.address == 0x84) { fbEp = true; CHECK(e.attributes == 0x11); CHECK(e.maxPacketSize == 4); CHECK(e.interval == 4); }
        if (e.address == 0x86) { inEp = true; CHECK(e.attributes == 0x05); }
    }
    CHECK(intEp); CHECK(outEp); CHECK(fbEp); CHECK(inEp);
    // IAD 在配置头之后
    CHECK(cfg[9] == 0x08);
    CHECK(cfg[10] == 0x0B); // IAD 类型
    CHECK(cfg[11] == 0x00); // bFirstInterface = 0
    CHECK(cfg[13] == 0x01); // IAD functionClass=audio
    CHECK(cfg[14] == 0x00); // functionSubClass
    CHECK(cfg[15] == 0x20); // functionProtocol = ADC 2.0
    // 协议字段
    CHECK(v->interfaces[0].proto == 0x20); // AC protocol
    CHECK(v->interfaces[1].proto == 0x20); // AS protocol
    // 通用 lint + UAC2 lint 全过
    CHECK(ude::lint_common(*v).empty());
    CHECK(uac2::lint_uac2(*v, cfg).empty());
}

TEST_CASE("uac2 max packet: 96k x 2ch x 4B") {
    uac2::Uac2Profile p;
    p.rates = {44100, 96000};
    CHECK(uac2::max_packet_size(p) == (96000 * 2 * 4 + 999) / 1000); // 768
}

TEST_CASE("uac2 device desc IAD class + qualifier") {
    uac2::Uac2Profile p;
    auto enc = uac2::build_device_desc_encoded(p);
    CHECK(enc[2] == 0x00); // bcdUSB 0x0200 低字节
    CHECK(enc[3] == 0x02);
    CHECK(enc[4] == 0xEF); // IAD
    CHECK(enc[5] == 0x02);
    CHECK(enc[6] == 0x01);
    CHECK(enc[7] == 64); // bMaxPacketSize0
    auto q = uac2::build_qualifier_desc(p);
    REQUIRE(q.size() == 10);
    CHECK(q[0] == 0x0A);
    CHECK(q[1] == 0x06);
    CHECK(q[7] == 64);
}

TEST_CASE("uac2 lint catches missing feedback ep") {
    uac2::Uac2Profile p;
    p.feedbackEp = false;
    auto cfg = uac2::build_config_desc(p);
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    auto errs = uac2::lint_uac2(*v, cfg);
    REQUIRE(errs.size() == 1);
    CHECK(errs[0].code == "uac2-async-out-needs-feedback");
}

TEST_CASE("uac2 lint catches non-ascending alt") {
    uac2::Uac2Profile p;
    auto cfg = uac2::build_config_desc(p);
    // 手工破坏：把 spk alt1 改成 alt1 再插一个 alt1 副本 —— 改用直接构造法：
    // 找到第二个 AS spk 接口描述符（IF1 alt1），把 alt 字节改为 0
    auto v = desc::parse_config(cfg);
    REQUIRE(v.has_value());
    for (auto&& iv : v->interfaces)
        if (iv.num == uac2::kInterfaceSpk && iv.alt == 1) {
            cfg[iv.offset + 3] = 0; // alt1 改回 0：序列 0,0 构成非升序
            break;
        }
    // 重新解析并 lint（spk alt 序列 0,0 —— 违反升序）
    auto v2 = desc::parse_config(cfg);
    REQUIRE(v2.has_value());
    bool caught = false;
    for (auto&& e : uac2::lint_uac2(*v2, cfg))
        if (e.code == "uac2-alt-ascending") caught = true;
    CHECK(caught);
}

TEST_CASE("uac2 ac wTotalLength backfilled consistently") {
    uac2::Uac2Profile p;
    auto cfg = uac2::build_config_desc(p);
    // AC header 位于 config(9)+IAD(8)+IF0(9)=26；wTotalLength @ +5
    const std::size_t acHdrAt = 26;
    const std::uint16_t csLen = static_cast<std::uint16_t>(cfg[acHdrAt + 6] | (cfg[acHdrAt + 7] << 8));
    // 手工累加：header(9)+clock(8)+spk(17+14+12)+mic(17+14+12)
    CHECK(csLen == 9 + 8 + 17 + 6 + 4 * 3 + 12 + 17 + 6 + 4 * 3 + 12);
}
