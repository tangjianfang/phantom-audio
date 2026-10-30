// 名字/路径白名单校验：每条规则都有"通过"和"拒绝"两侧的用例（检查要有牙齿）
#include <doctest/doctest.h>
#include <uac/path_policy.h>

using namespace uac::policy;

namespace {
constexpr char kBs = 0x5C; // 反斜杠。测试里不手写 "\\"：转义写错正是这个 bug 的来源
std::string bs(const char* s) { // 把 '/' 换成真正的反斜杠，便于写 Windows 路径
    std::string r = s;
    for (auto& c : r)
        if (c == '/') c = kBs;
    return r;
}
} // namespace

TEST_CASE("profile name: accepts plain identifiers, optional .json") {
    CHECK(valid_profile_name("uac2-48k"));
    CHECK(valid_profile_name("uac1-cmedia-sim"));
    CHECK(valid_profile_name("uac2_hid_headset"));
    CHECK(valid_profile_name("uac2-48k.json"));
    CHECK(valid_profile_name(std::string(64, 'a')));
}

TEST_CASE("profile name: rejects empty, too long, and every path-ish form") {
    CHECK_FALSE(valid_profile_name(""));
    CHECK_FALSE(valid_profile_name(".json")); // 只有后缀
    CHECK_FALSE(valid_profile_name(std::string(65, 'a')));
    CHECK_FALSE(valid_profile_name("a/b"));
    CHECK_FALSE(valid_profile_name("a:b"));
    CHECK_FALSE(valid_profile_name(".."));
    CHECK_FALSE(valid_profile_name("a..b"));
    CHECK_FALSE(valid_profile_name("a.txt"));
    CHECK_FALSE(valid_profile_name("a.json.json")); // 去一次后缀后仍含 '.'
    CHECK_FALSE(valid_profile_name("a b"));
    CHECK_FALSE(valid_profile_name(std::string("a") + '\0' + "b"));
}

TEST_CASE("profile name: the backslash forms that slipped through the old blacklist") {
    // 回归：旧实现的黑名单字符串是 ":/\.."，C++ 把 \. 当成 '.'，反斜杠没进黑名单。
    CHECK_FALSE(valid_profile_name(std::string(1, kBs) + "Windows" + kBs + "win"));
    CHECK_FALSE(valid_profile_name(std::string(2, kBs) + "host" + kBs + "share" + kBs + "x"));
    CHECK_FALSE(valid_profile_name(std::string("a") + kBs + "b"));
    CHECK_FALSE(valid_profile_name(std::string(1, kBs)));
}

TEST_CASE("profile name: non-ASCII bytes are rejected (no locale-dependent isalnum)") {
    CHECK_FALSE(valid_profile_name("caf\xC3\xA9"));
    CHECK_FALSE(valid_profile_name("\xFF"));
}

TEST_CASE("media path: accepts plain relative/absolute .wav") {
    std::string why;
    CHECK(valid_media_path("capture.wav", &why));
    CHECK(valid_media_path("out/capture.WAV"));
    CHECK(valid_media_path(bs("C:/Users/x/Desktop/a.wav")));
    CHECK(valid_media_path("C:/tmp/a.wav"));
    CHECK(valid_media_path("my.capture.wav"));
}

TEST_CASE("media path: extension must be .wav") {
    std::string why;
    CHECK_FALSE(valid_media_path("capture.txt", &why));
    CHECK(why == "only .wav files are allowed");
    CHECK_FALSE(valid_media_path("capture"));
    CHECK_FALSE(valid_media_path(".wav")); // 无文件名
    CHECK_FALSE(valid_media_path("capture.wav.exe"));
    CHECK_FALSE(valid_media_path("capture.wav ")); // 末尾空格（Windows 会忽略）
    CHECK_FALSE(valid_media_path("capture.wav."));
}

TEST_CASE("media path: UNC and device namespace are rejected") {
    std::string why;
    CHECK_FALSE(valid_media_path(bs("//host/share/a.wav"), &why));
    CHECK(why == "UNC or device path not allowed");
    CHECK_FALSE(valid_media_path("//host/share/a.wav"));
    CHECK_FALSE(valid_media_path(bs("//?/C:/a.wav")));
    CHECK_FALSE(valid_media_path(bs("//./PhysicalDrive0/a.wav")));
}

TEST_CASE("media path: ':' only as drive designator followed by a separator") {
    std::string why;
    CHECK_FALSE(valid_media_path("a.wav:stream.wav", &why)); // NTFS 备用数据流
    CHECK(why == "':' only allowed as drive designator");
    CHECK_FALSE(valid_media_path("C:a.wav", &why)); // 盘符相对路径
    CHECK(why == "drive-relative path not allowed");
    CHECK_FALSE(valid_media_path("1:/a.wav", &why)); // 盘符只能是字母
    CHECK(why == "bad drive letter");
}

TEST_CASE("media path: '..' components are rejected, '..' inside a name is fine") {
    std::string why;
    CHECK_FALSE(valid_media_path("../a.wav", &why));
    CHECK(why == "'..' component not allowed");
    CHECK_FALSE(valid_media_path(bs("x/../../a.wav")));
    CHECK_FALSE(valid_media_path(bs("C:/x/../a.wav")));
    CHECK(valid_media_path("a..b.wav")); // '..' 不是独立分量
}

TEST_CASE("media path: illegal characters and reserved device names") {
    std::string why;
    CHECK_FALSE(valid_media_path("a<b.wav", &why));
    CHECK(why == "illegal character in path");
    CHECK_FALSE(valid_media_path("a|b.wav"));
    CHECK_FALSE(valid_media_path("a*.wav"));
    CHECK_FALSE(valid_media_path("a?.wav"));
    CHECK_FALSE(valid_media_path("a\"b.wav"));
    CHECK_FALSE(valid_media_path(std::string("a") + '\t' + ".wav"));
    CHECK_FALSE(valid_media_path("con.wav", &why)); // CON.wav 仍是控制台设备
    CHECK(why == "reserved device name");
    CHECK_FALSE(valid_media_path("NUL.wav"));
    CHECK_FALSE(valid_media_path("Com1.wav"));
    CHECK_FALSE(valid_media_path(bs("dir/LPT9.wav")));
    CHECK(valid_media_path("com10.wav")); // COM10 不是保留名
    CHECK(valid_media_path("console.wav"));
}

TEST_CASE("media path: empty and over-long are rejected, why is optional") {
    CHECK_FALSE(valid_media_path(""));
    CHECK_FALSE(valid_media_path(std::string(260, 'a') + ".wav"));
    CHECK(valid_media_path(std::string(255, 'a') + ".wav")); // 259 字符，恰在上限
}
