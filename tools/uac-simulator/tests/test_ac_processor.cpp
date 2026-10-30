// AC 处理器与日志测试
#include <doctest/doctest.h>
#include <uac/ac_processor.h>
#include <uac/desc_uac2.h>
#include <uac/device_model.h>
#include <uac/as_engine.h>
#include <memory>
#include <uac/desc_uac1.h>
#include <algorithm>
#include <uac/usb_desc.h>

using namespace uac;
namespace {
constexpr std::uint8_t SET_CUR = 0x01, GET_CUR = 0x81, GET_MIN = 0x82, GET_MAX = 0x83,
                      GET_RES = 0x84;
constexpr std::uint8_t CS_MUTE = 0x01, CS_VOL = 0x02, CS_SAMFREQ = 0x01;

struct Fixture {
    AcState st;
    LogStore log;
    AcProcessor ac{st, log};
    Fixture() {
        uac1::Uac1Profile p;
        p.serial = "S1";
        ac.set_descriptors(uac1::build_device_desc_encoded(p), uac1::build_config_desc(p),
                           uac1::build_string_descs(p));
        ac.set_feature_unit_ids({uac1::kFuSpeaker, uac1::kFuMic});
    }
};

std::vector<std::uint8_t> le16b(std::int16_t v) {
    return {static_cast<std::uint8_t>(v & 0xff), static_cast<std::uint8_t>((v >> 8) & 0xff)};
}
} // namespace

TEST_CASE("volume set/get roundtrip per channel") {
    Fixture f;
    // SET_CUR ch=1 vol=-10dB(-2560)：wValue=(CS<<8)|CN=0x0201，wIndex=(FU2<<8)|IF0
    auto r1 = f.ac.on_control(0x21, SET_CUR, (CS_VOL << 8) | 1, (uac1::kFuSpeaker << 8) | 0, 2,
                              le16b(-2560));
    CHECK(r1.kind == ControlResult::Kind::Ok);
    auto r2 = f.ac.on_control(0xA1, GET_CUR, (CS_VOL << 8) | 1, (uac1::kFuSpeaker << 8) | 0, 2, {});
    REQUIRE(r2.kind == ControlResult::Kind::Ok);
    CHECK(r2.data == le16b(-2560));
}

TEST_CASE("volume range golden bytes little endian") {
    Fixture f;
    auto mn = f.ac.on_control(0xA1, GET_MIN, (CS_VOL << 8) | 0, (uac1::kFuSpeaker << 8) | 0, 2, {});
    CHECK(mn.data == std::vector<std::uint8_t>{0x00, 0xE8}); // -6144 = 0xE800
    auto mx = f.ac.on_control(0xA1, GET_MAX, (CS_VOL << 8) | 0, (uac1::kFuSpeaker << 8) | 0, 2, {});
    CHECK(mx.data == std::vector<std::uint8_t>{0x00, 0x00});
    auto rs = f.ac.on_control(0xA1, GET_RES, (CS_VOL << 8) | 0, (uac1::kFuSpeaker << 8) | 0, 2, {});
    CHECK(rs.data == std::vector<std::uint8_t>{0x00, 0x01}); // 256 = 0x0100
}

TEST_CASE("mute per channel + state update") {
    Fixture f;
    auto s = f.ac.on_control(0x21, SET_CUR, (CS_MUTE << 8) | 1, (uac1::kFuSpeaker << 8) | 0, 1, std::vector<std::uint8_t>{1});
    CHECK(s.kind == ControlResult::Kind::Ok);
    CHECK(f.st.mute[0] == true); // 声道 1 → 数组下标 0
    auto g = f.ac.on_control(0xA1, GET_CUR, (CS_MUTE << 8) | 1, (uac1::kFuSpeaker << 8) | 0, 1, {});
    REQUIRE(g.kind == ControlResult::Kind::Ok);
    CHECK(g.data == std::vector<std::uint8_t>{1});
}

TEST_CASE("endpoint sample rate set/get (3B LE)") {
    Fixture f;
    auto s = f.ac.on_control(0x22, SET_CUR, CS_SAMFREQ << 8, 0x86, 3, std::vector<std::uint8_t>{0x80, 0xBB, 0x00});
    CHECK(s.kind == ControlResult::Kind::Ok);
    CHECK(f.st.sampleRate == 48000);
    auto g = f.ac.on_control(0xA2, GET_CUR, CS_SAMFREQ << 8, 0x86, 3, {});
    REQUIRE(g.kind == ControlResult::Kind::Ok);
    CHECK(g.data == std::vector<std::uint8_t>{0x80, 0xBB, 0x00});
}

TEST_CASE("unknown request stalls and gets logged") {
    Fixture f;
    auto r = f.ac.on_control(0xA1, 0x77, 0, 0, 4, {});
    CHECK(r.kind == ControlResult::Kind::Stall);
    CHECK(f.ac.request_count() == 1);
    CHECK(f.log.count() == 1);
    auto lines = f.log.tail(1);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("\"resp\":\"stall\"") != std::string::npos);
    CHECK(lines[0].find("\"bRequest\":119") != std::string::npos);
}

TEST_CASE("get descriptor device/config/string with wLength truncation") {
    Fixture f;
    auto d = f.ac.on_control(0x80, 0x06, 0x0100, 0, 18, {});
    REQUIRE(d.kind == ControlResult::Kind::Ok);
    CHECK(d.data.size() == 18);
    CHECK(d.data[0] == 0x12);

    auto dShort = f.ac.on_control(0x80, 0x06, 0x0100, 0, 8, {});
    CHECK(dShort.data.size() == 8); // 截断

    auto c = f.ac.on_control(0x80, 0x06, 0x0200, 0, 255, {});
    REQUIRE(c.kind == ControlResult::Kind::Ok);
    CHECK(c.data.size() > 100); // 完整配置（<255）

    auto s = f.ac.on_control(0x80, 0x06, 0x0302, 0x0409, 255, {});
    REQUIRE(s.kind == ControlResult::Kind::Ok);
    CHECK(s.data[0] == 2 + 2 * std::string("UAC1 Simulator").size());
    CHECK(s.data[1] == 0x03);

    auto sMissing = f.ac.on_control(0x80, 0x06, 0x0399, 0x0409, 4, {});
    CHECK(sMissing.kind == ControlResult::Kind::Stall); // 无此字符串索引
}

// USB 2.0 §9.6.7：字符串索引 0 返回支持的语言 ID 数组。第一次 Sandbox 运行里 Windows 对索引 0 请求了 3 次
// （UAC2 两次、UAC1 一次），全部被 STALL（设备侧日志里 wValue=768, resp=stall）；其余索引的请求则带着 LANGID 0x0409 或 0 才成功。
TEST_CASE("string descriptor 0 is the LANGID list (US English), other missing indexes still stall") {
    Fixture f;
    const std::vector<std::uint8_t> want{0x04, 0x03, 0x09, 0x04};
    auto r = f.ac.on_control(0x80, 0x06, 0x0300, 0, 255, {});
    REQUIRE(r.kind == ControlResult::Kind::Ok);
    CHECK(r.data == want);
    // Windows 也会用 wLength=4 / 2 来问
    auto r4 = f.ac.on_control(0x80, 0x06, 0x0300, 0, 4, {});
    REQUIRE(r4.kind == ControlResult::Kind::Ok);
    CHECK(r4.data == want);
    auto r2 = f.ac.on_control(0x80, 0x06, 0x0300, 0, 2, {});
    REQUIRE(r2.kind == ControlResult::Kind::Ok);
    CHECK(r2.data.size() == 2);
    // 对照：不存在的索引照旧 STALL（不是"所有字符串请求都成功"）
    CHECK(f.ac.on_control(0x80, 0x06, 0x0399, 0x0409, 4, {}).kind == ControlResult::Kind::Stall);
    CHECK(f.ac.on_control(0x80, 0x06, 0x0304, 0x0409, 255, {}).kind == ControlResult::Kind::Stall); // 这个 profile 有序列号时是 3，没有 4
}

// 第二次 Sandbox 运行的设备侧日志里，usbaudio2.sys 启动时对中断端点 0x81 发了 CLEAR_FEATURE(ENDPOINT_HALT)
// （bmRequestType=2, bRequest=1, wIndex=129），被 STALL。USB 2.0 §9.4.1：对存在的端点清除 halt 必须成功。
TEST_CASE("endpoint requests: halt can be cleared/set on endpoints that exist, not on ones that do not") {
    Fixture f;
    for (int epInt : {0x06, 0x86}) { // 两个默认 profile 都有的数据端点（喇叭 OUT / 麦克风 IN）
        const auto ep = static_cast<std::uint16_t>(epInt);
        INFO("endpoint " << ep);
        CHECK(f.ac.on_control(0x02, 0x01, 0 /*ENDPOINT_HALT*/, ep, 0, {}).kind == ControlResult::Kind::Ok); // CLEAR_FEATURE
        CHECK(f.ac.on_control(0x02, 0x03, 0 /*ENDPOINT_HALT*/, ep, 0, {}).kind == ControlResult::Kind::Ok); // SET_FEATURE
        auto st = f.ac.on_control(0x82, 0x00, 0, ep, 2, {}); // GET_STATUS：没有 halt
        REQUIRE(st.kind == ControlResult::Kind::Ok);
        CHECK(st.data == std::vector<std::uint8_t>{0, 0});
    }
    // 对照：不存在的端点、不是 ENDPOINT_HALT 的特性值，都照旧 STALL
    CHECK(f.ac.on_control(0x02, 0x01, 0, 0x05, 0, {}).kind == ControlResult::Kind::Stall);
    CHECK(f.ac.on_control(0x02, 0x01, 0, 0x8F, 0, {}).kind == ControlResult::Kind::Stall);
    CHECK(f.ac.on_control(0x82, 0x00, 0, 0x05, 2, {}).kind == ControlResult::Kind::Stall);
    CHECK(f.ac.on_control(0x02, 0x01, 1 /*不是 ENDPOINT_HALT*/, 0x06, 0, {}).kind == ControlResult::Kind::Stall);
}

TEST_CASE("set/get configuration status") {
    Fixture f;
    auto sc = f.ac.on_control(0x00, 0x09, 1, 0, 0, {});
    CHECK(sc.kind == ControlResult::Kind::Ok);
    auto gc = f.ac.on_control(0x80, 0x08, 0, 0, 1, {});
    REQUIRE(gc.kind == ControlResult::Kind::Ok);
    CHECK(gc.data == std::vector<std::uint8_t>{1});
}

TEST_CASE("log store ring behavior") {
    LogStore ls;
    for (int i = 0; i < 100; ++i) ls.append("{\"i\":" + std::to_string(i) + "}");
    CHECK(ls.count() == 100);
    auto t = ls.tail(3);
    REQUIRE(t.size() == 3);
    CHECK(t[2] == "{\"i\":99}");
    ls.clear();
    CHECK(ls.count() == 0);
}

// ---------------- UAC2 协议分支（Task 11）----------------
TEST_CASE("uac2 clock source CUR/RANGE negotiation") {
    AcState st; LogStore log; AcProcessor ac(st, log);
    uac2::Uac2Profile p;
    ac.set_protocol(Protocol::Uac2);
    ac.set_rates(p.rates);              // {44100, 48000, 96000}
    ac.set_clock_id(uac2::kClock);
    ac.set_feature_unit_ids({uac2::kFuSpeaker, uac2::kFuMic});
    ac.set_descriptors(uac2::build_device_desc_encoded(p), uac2::build_config_desc(p),
                       uac2::build_string_descs(p), uac2::build_qualifier_desc(p));

    // RANGE：numSubRanges=3 + 3×{MIN,MAX,RES}
    auto rg = ac.on_control(0xA1, 0x02 /*RANGE*/, 0x01 << 8, (uac2::kClock << 8) | 0, 38, {});
    REQUIRE(rg.kind == ControlResult::Kind::Ok);
    REQUIRE(rg.data.size() == 2 + 36);
    CHECK(rg.data[0] == 3);
    CHECK(rg.data[2] == 0x44); CHECK(rg.data[3] == 0xAC); CHECK(rg.data[4] == 0x00); // 44100
    CHECK(rg.data[6] == 0x44); CHECK(rg.data[7] == 0xAC); // MAX1=MIN1（离散档）
    CHECK(rg.data[14] == 0x80); CHECK(rg.data[15] == 0xBB); // 48000

    // SET CUR 48000（OUT 数据 4B LE）
    auto sc = ac.on_control(0x21, 0x01 /*CUR*/, 0x01 << 8, (uac2::kClock << 8) | 0, 4,
                            std::vector<std::uint8_t>{0x80, 0xBB, 0x00, 0x00});
    CHECK(sc.kind == ControlResult::Kind::Ok);
    CHECK(st.sampleRate == 48000);
    auto gc = ac.on_control(0xA1, 0x01, 0x01 << 8, (uac2::kClock << 8) | 0, 4, {});
    REQUIRE(gc.kind == ControlResult::Kind::Ok);
    CHECK(gc.data.size() == 4);
    CHECK(gc.data[0] == 0x80); CHECK(gc.data[1] == 0xBB);

    // Clock Valid CUR = 1
    auto cv = ac.on_control(0xA1, 0x01, 0x02 << 8, (uac2::kClock << 8) | 0, 1, {});
    REQUIRE(cv.kind == ControlResult::Kind::Ok);
    CHECK(cv.data == std::vector<std::uint8_t>{1});
}

TEST_CASE("uac2 FU volume CUR + RANGE 2B") {
    AcState st; LogStore log; AcProcessor ac(st, log);
    uac2::Uac2Profile p;
    ac.set_protocol(Protocol::Uac2);
    ac.set_clock_id(uac2::kClock);
    ac.set_feature_unit_ids({uac2::kFuSpeaker});
    ac.set_descriptors({}, {}, {}, {});

    auto sv = ac.on_control(0x21, 0x01, (0x02 << 8) | 1, (uac2::kFuSpeaker << 8) | 0, 2,
                            std::vector<std::uint8_t>{0x00, 0xF6});
    CHECK(sv.kind == ControlResult::Kind::Ok);
    auto gv = ac.on_control(0xA1, 0x01, (0x02 << 8) | 1, (uac2::kFuSpeaker << 8) | 0, 2, {});
    REQUIRE(gv.kind == ControlResult::Kind::Ok);
    CHECK(gv.data.size() == 2);
    CHECK(gv.data[1] == 0xF6);
    auto vr = ac.on_control(0xA1, 0x02, (0x02 << 8) | 0, (uac2::kFuSpeaker << 8) | 0, 8, {});
    REQUIRE(vr.kind == ControlResult::Kind::Ok);
    CHECK(vr.data.size() == 8); // num(2)+MIN(2)+MAX(2)+RES(2)
    CHECK(vr.data[0] == 1);
    CHECK(vr.data[2] == 0x00); CHECK(vr.data[3] == 0xE8); // -6144
}

TEST_CASE("uac2 device qualifier via GET_DESCRIPTOR") {
    AcState st; LogStore log; AcProcessor ac(st, log);
    uac2::Uac2Profile p;
    ac.set_descriptors({}, {}, {}, uac2::build_qualifier_desc(p));
    auto q = ac.on_control(0x80, 0x06, 0x0600, 0, 10, {});
    REQUIRE(q.kind == ControlResult::Kind::Ok);
    CHECK(q.data.size() == 10);
    CHECK(q.data[1] == 0x06);
    // UAC1（无 qualifier）→ stall
    AcState st2; LogStore log2; AcProcessor ac2(st2, log2);
    auto q2 = ac2.on_control(0x80, 0x06, 0x0600, 0, 10, {});
    CHECK(q2.kind == ControlResult::Kind::Stall);
}

TEST_CASE("uac2 snapshot wiring + 32-bit slot packing") {
    auto s = make_uac2_default_snapshot();
    CHECK(s.protocol == Protocol::Uac2);
    CHECK(s.qualifier.size() == 10);
    auto v = desc::parse_config(s.config);
    REQUIRE(v.has_value());
    CHECK(uac2::lint_uac2(*v, s.config).empty());
    // 32bit 槽：1ms @48k 立体声 = 48×2×4=384B/包
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(std::move(s), std::move(clock));
    auto* mc = dynamic_cast<ManualClock*>(&m.clock());
    REQUIRE(mc != nullptr);
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 1;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 6;
    c.submit.number_of_packets = 1;
    int got = 0;
    std::vector<unsigned char> payload;
    m.engine().on_iso_in(c, [&](Completion&& cm) { payload = std::move(cm.payload); ++got; });
    mc->advance_to(1000);
    m.pump();
    REQUIRE(got == 1);
    CHECK(payload.size() == 384);
}
