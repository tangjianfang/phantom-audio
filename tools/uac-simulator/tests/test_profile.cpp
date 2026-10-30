// Profile 加载/校验/快照组装测试
#include <doctest/doctest.h>
#include <uac/profile.h>
#include <fstream>
#include <cstdio>

using namespace uac;

namespace {
std::string write_tmp(const char* name, const std::string& content) {
    const std::string path = std::string(name);
    std::ofstream f(path);
    f << content;
    return path;
}
// profiles 目录自定位：源码树运行（cwd=tools/uac-simulator）或 ctest（cwd=build/*）
std::string profile_path(const char* name) {
    for (const char* base : {"profiles/", "../../profiles/", "../../../profiles/"}) {
        std::string p = std::string(base) + name;
        std::ifstream f(p);
        if (f.good()) return p;
    }
    return std::string("profiles/") + name; // 让 load_profile 报出清晰错误
}
} // namespace

TEST_CASE("built-in profiles load and build valid snapshots") {
    const char* files[] = {"uac1-cmedia-sim.json", "uac2-48k.json", "uac2-hid-headset.json", "uac1-razer-clio.json"};
    for (auto* f : files) {
        auto p = load_profile(profile_path(f));
        CAPTURE(f);
        auto snap = build_snapshot(p);
        auto v = desc::parse_config(snap.config);
        REQUIRE(v.has_value());
        CHECK(ude::lint_common(*v).empty());
        if (p.protocol == Protocol::Uac2)
            CHECK(uac2::lint_uac2(*v, snap.config).empty());
    }
}

TEST_CASE("uac1 profile fields propagate to snapshot") {
    auto p = load_profile(profile_path("uac1-cmedia-sim.json"));
    CHECK(p.protocol == Protocol::Uac1);
    CHECK(p.vid == 0x0D8C);
    CHECK(p.volMin == -6144); // -24dB × 256
    auto snap = build_snapshot(p);
    CHECK(snap.dev.idVendor == 0x0D8C);
    CHECK(snap.dev.idProduct == 0x0103);
    CHECK(snap.ac.volRes == 256);
    auto enc = desc::encode_device_desc(snap.dev);
    CHECK(enc[0] == 0x12);
}

// Razer Clio (Synapse: folderName/deviceName "carol", rzDevice25Carol, PID 1392 = 0x0570). Only the USB identity can be
// simulated - the Synapse-side fields (Chinese display name, claimInterface, ...) are not part of any USB descriptor.
TEST_CASE("razer clio profile: VID 0x1532 / PID 0x0570 reach the device descriptor and the product string") {
    auto p = load_profile(profile_path("uac1-razer-clio.json"));
    CHECK(p.protocol == Protocol::Uac1);
    auto snap = build_snapshot(p);
    CHECK(snap.dev.idVendor == 0x1532);
    CHECK(snap.dev.idProduct == 0x0570);
    CHECK(snap.dev.idProduct == 1392); // the decimal PID from the Synapse device list
    const auto enc = desc::encode_device_desc(snap.dev);
    CHECK(enc[8] == 0x32); // idVendor little-endian
    CHECK(enc[9] == 0x15);
    CHECK(enc[10] == 0x70); // idProduct little-endian
    CHECK(enc[11] == 0x05);
    // iProduct (index 2) is "Razer Clio" in UTF-16LE: 2 + 2*10 bytes
    REQUIRE(snap.strings.count(2) == 1);
    const auto& s = snap.strings.at(2);
    REQUIRE(s.size() == 2 + 2 * 10);
    CHECK(s[1] == 0x03);
    CHECK(s[2] == 'R');
    CHECK(s[3] == 0x00);
    // Control: the other UAC1 profile has a different identity (the test fails if the ids were hard-wired)
    auto other = build_snapshot(load_profile(profile_path("uac1-cmedia-sim.json")));
    CHECK(other.dev.idVendor != snap.dev.idVendor);
    CHECK(other.dev.idProduct != snap.dev.idProduct);
}

TEST_CASE("uac2-hid-headset enables hid and adjusts volume range") {
    auto p = load_profile(profile_path("uac2-hid-headset.json"));
    CHECK(p.hidConsumerVolume);
    CHECK(p.volRes == 512); // 2dB × 256
    auto snap = build_snapshot(p);
    CHECK(snap.hidConsumerVolume);
    // UAC2 + HID：接口数 4（AC+2AS+HID）
    auto v = desc::parse_config(snap.config);
    REQUIRE(v.has_value());
    CHECK(v->bNumInterfaces == 4);
}

TEST_CASE("invalid json and fields raise ProfileError") {
    CHECK_THROWS_AS(parse_profile_json("not json", "x"), ProfileError);
    CHECK_THROWS_AS(parse_profile_json(R"({"protocol":"uac3"})", "x"), ProfileError);
    CHECK_THROWS_AS(parse_profile_json(R"({"audio":{}})", "x"), ProfileError); // 缺 protocol
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","usb":{"vid":"0xZZ"}})", "x"), ProfileError);
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","audio":{"volumeDb":{"min":0,"max":0}}})", "x"), ProfileError);
    CHECK_THROWS_AS(parse_profile_json(
        R"({"protocol":"uac1","rawConfigOverride":"09 02 0"})", "x"), ProfileError);
    CHECK_THROWS_AS(load_profile("profiles/does-not-exist.json"), ProfileError);
}

TEST_CASE("raw override replaces config descriptor") {
    // 黄金 C-Media 116B 作为 raw 覆盖
    const std::string hex =
        "09 02 74 00 02 01 00 80 FA 09 04 00 00 00 01 01"
        "00 00 09 24 01 00 01 2B 00 01 01 0C 24 02 01 01"
        "01 00 02 03 00 00 00 0D 24 06 0D 01 02 01 02 02"
        "00 02 00 00 09 24 03 03 01 03 00 0D 00 09 04 01"
        "00 00 01 02 00 00 09 04 01 01 01 01 02 00 00 07"
        "24 01 01 01 01 00 0E 24 02 01 02 02 10 02 44 AC"
        "00 80 BB 00 09 05 06 09 C0 00 01 00 00 07 25 01"
        "01 00 00 00";
    auto p = parse_profile_json(
        R"({"protocol":"uac1","rawConfigOverride":")" + hex + "\"}", "raw-test");
    auto snap = build_snapshot(p);
    REQUIRE(snap.config.size() == 116);
    // UDE 适配后 interval 变 4
    CHECK(snap.config[106] == 4);
    auto v = desc::parse_config(snap.config);
    REQUIRE(v.has_value());
}

TEST_CASE("snapshot diff drives reattach decision") {
    auto a = build_snapshot(load_profile(profile_path("uac1-cmedia-sim.json")));
    auto b = build_snapshot(load_profile(profile_path("uac2-48k.json")));
    auto c = build_snapshot(load_profile(profile_path("uac1-cmedia-sim.json")));
    CHECK(snapshot_requires_reattach(a, b));
    CHECK_FALSE(snapshot_requires_reattach(a, c));
}

TEST_CASE("string hex vid with 0x prefix and decimal both parse") {
    auto p1 = parse_profile_json(R"({"protocol":"uac1","usb":{"vid":305,"pid":259}})", "d");
    CHECK(p1.vid == 305);
    CHECK(p1.pid == 259);
    auto p2 = parse_profile_json(R"({"protocol":"uac1","usb":{"vid":"0x1D50","pid":"0x6222"}})", "h");
    CHECK(p2.vid == 0x1D50);
    CHECK(p2.pid == 0x6222);
}
