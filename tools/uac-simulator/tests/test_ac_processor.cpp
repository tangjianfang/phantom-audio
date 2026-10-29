// AC 处理器与日志测试
#include <doctest/doctest.h>
#include <uac/ac_processor.h>
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
