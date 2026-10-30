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

// ---- ADC 2.0 一致性（来自第一次 Sandbox 运行：UAC1 设备被 usbaudio.sys 认领并探测了所有控件，而同样流程的 UAC2
// 设备在 SET_CONFIGURATION 之后没有收到任何类请求。核对描述符发现两处违反规范：CS AS General 少了 iChannelNames
// （15 字节，规范 16），CS 端点描述符类型写成 0x24（应为 0x25）。）----
namespace {
std::vector<std::size_t> offsets_of(const std::vector<u8>& cfg, u8 type, int subtype = -1) {
    std::vector<std::size_t> r;
    for (std::size_t off = 0; off + 2 <= cfg.size() && cfg[off] >= 2; off += cfg[off])
        if (cfg[off + 1] == type && (subtype < 0 || (off + 2 < cfg.size() && cfg[off + 2] == subtype))) r.push_back(off);
    return r;
}
bool has_code(const std::vector<ude::LintError>& errs, const char* code) {
    for (auto&& e : errs)
        if (e.code == code) return true;
    return false;
}
// 被篡改的描述符可能连 parse_config 都过不了；lint 仍要独立报出原因，所以解析失败时用空视图
std::vector<ude::LintError> lint_of(const std::vector<u8>& cfg) {
    auto v = desc::parse_config(cfg);
    const desc::ConfigView none{};
    return uac2::lint_uac2(v ? *v : none, cfg);
}
} // namespace

TEST_CASE("uac2 class-specific descriptors have the lengths and types ADC 2.0 requires") {
    uac2::Uac2Profile p; // 默认：喇叭 + 麦克风 + 反馈端点
    const auto cfg = uac2::build_config_desc(p);

    const auto asGeneral = offsets_of(cfg, 0x24, 0x01); // AC header 也是 subtype 1：只取 AS 里的（长度不是 9）
    int asCount = 0;
    for (auto off : asGeneral)
        if (cfg[off] != 9) {
            ++asCount;
            CHECK_MESSAGE(cfg[off] == 16, "CS AS General must be 16 bytes (ends with iChannelNames), got " << int(cfg[off]));
        }
    CHECK(asCount == 2); // 喇叭 + 麦克风

    // CS 端点描述符：类型 0x25、长度 8，且只跟在等时*数据*端点后面（数据端点两个：0x06 与 0x86）
    const auto csEp = offsets_of(cfg, 0x25);
    CHECK(csEp.size() == 2);
    for (auto off : csEp) {
        CHECK(cfg[off] == 8);
        CHECK(cfg[off + 2] == 0x01);
    }
    // 没有任何 0x24 描述符直接跟在端点描述符后面（那是把 CS 端点写错了类型）
    for (auto off : offsets_of(cfg, 0x05)) {
        const auto next = off + cfg[off];
        if (next + 1 < cfg.size()) CHECK_MESSAGE(cfg[next + 1] != 0x24, "a CS_INTERFACE descriptor follows an endpoint at offset " << next);
    }
    // 反馈端点（0x84）后面不跟 CS 端点描述符
    for (auto off : offsets_of(cfg, 0x05))
        if (cfg[off + 2] == 0x84) {
            const auto next = off + cfg[off];
            if (next + 1 < cfg.size()) CHECK(cfg[next + 1] != 0x25);
        }
    CHECK(lint_of(cfg).empty());
}

TEST_CASE("uac2 lint has teeth: wrong CS endpoint type, short AS General, misplaced CS endpoint") {
    uac2::Uac2Profile p;
    const auto good = uac2::build_config_desc(p);
    REQUIRE(lint_of(good).empty()); // 对照：同一份描述符，没被破坏时不报

    { // CS 端点类型改成 0x24（旧缺陷）
        auto cfg = good;
        const auto csEp = offsets_of(cfg, 0x25);
        REQUIRE(!csEp.empty());
        cfg[csEp[0] + 1] = 0x24;
        CHECK(has_code(lint_of(cfg), "uac2-cs-endpoint-type"));
    }
    { // AS General 的 bLength 改成 15（旧缺陷）
        auto cfg = good;
        std::size_t at = 0;
        for (auto off : offsets_of(cfg, 0x24, 0x01))
            if (cfg[off] == 16) { at = off; break; }
        REQUIRE(at != 0);
        cfg[at] = 15;
        CHECK(has_code(lint_of(cfg), "uac2-cs-descriptor-length"));
    }
    { // 把反馈端点改成数据端点的属性位之外的位置：直接让 CS 端点跟在反馈端点后面
        auto cfg = good;
        for (auto off : offsets_of(cfg, 0x05))
            if (cfg[off + 2] == 0x06) { cfg[off + 2] = 0x84; break; } // 数据 OUT 端点伪装成反馈端点地址
        // 数据端点属性 0x05（usage=数据），反馈端点是 0x11；这里把 *属性* 改成反馈：CS 端点于是跟在反馈端点后
        for (auto off : offsets_of(cfg, 0x05))
            if (cfg[off + 2] == 0x84 && cfg[off + 3] == 0x05) { cfg[off + 3] = 0x11; break; }
        CHECK(has_code(lint_of(cfg), "uac2-cs-endpoint-misplaced"));
    }
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
