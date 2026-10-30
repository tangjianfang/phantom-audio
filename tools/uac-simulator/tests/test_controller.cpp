// uacstudio 控制层单测：按钮表自洽、请求体构建、以及"GUI 发出的每个请求都被仿真器接受"。
// 最后一类是关键：GUI 的按钮表和服务端的 on_event/on_inject 是两处各自维护的东西，
// 不把按钮真的发给仿真器跑一遍，改一边就会悄悄漂移。
#include <doctest/doctest.h>
#include <controller.h>
#include <http_client.h>
#include <nlohmann/json.hpp>
#include <sim_host.h>

using nlohmann::json;
using namespace uacstudio;

namespace {
struct Host {
    uac::SimHost h{std::string(UAC_SOURCE_DIR) + "/profiles"};
    Host() { REQUIRE(h.start(0, 0)); }
    ~Host() { h.stop(); }
};
} // namespace

TEST_CASE("controller: action table is self-consistent") {
    CHECK(ValidateActions().empty());
    CHECK(Actions().size() > static_cast<std::size_t>(kActionsInFirstRow));
    REQUIRE(FindAction(200) != nullptr);
    CHECK(std::string(FindAction(200)->path) == "/event");
    CHECK(FindAction(99999) == nullptr);
    CHECK_FALSE(ProfileNames().empty());
}

TEST_CASE("controller: ValidateActionTable catches each kind of bad table") {
    const Action good{1, L"ok", "/event", R"({"type":"mute"})", "ok", Tone::Neutral};
    CHECK(ValidateActionTable({good}).empty());

    Action dup = good;
    CHECK(ValidateActionTable({good, dup}).find("duplicate action id 1") != std::string::npos);

    Action badRoute = good;
    badRoute.id = 2;
    badRoute.path = "/nope";
    CHECK(ValidateActionTable({good, badRoute}).find("unknown route") != std::string::npos);

    Action badJson = good;
    badJson.id = 3;
    badJson.body = "{not json";
    CHECK(ValidateActionTable({good, badJson}).find("not a JSON object") != std::string::npos);

    Action notObject = good;
    notObject.id = 4;
    notObject.body = "[1,2]";
    CHECK(ValidateActionTable({good, notObject}).find("not a JSON object") != std::string::npos);

    Action noLabel = good;
    noLabel.id = 5;
    noLabel.label = L"";
    CHECK(ValidateActionTable({good, noLabel}).find("empty label") != std::string::npos);

    Action noName = good;
    noName.id = 6;
    noName.what = "";
    CHECK(ValidateActionTable({good, noName}).find("empty log name") != std::string::npos);
}

TEST_CASE("controller: every GUI one-key button is accepted by the simulator (over HTTP)") {
    Host host;
    const auto port = host.h.http_port();
    std::string out;
    REQUIRE(http_post(port, "/attach", BuildAttachBody("uac2-48k"), &out));
    REQUIRE(json::parse(out).value("attached", "") == "uac2-48k");

    for (const auto& a : Actions()) {
        INFO("button id " << a.id << " (" << a.what << ") -> " << a.path << " " << a.body);
        out.clear();
        REQUIRE(http_post(port, a.path, a.body, &out));
        const auto j = json::parse(out, nullptr, false);
        REQUIRE(j.is_object());
        CHECK_FALSE(j.contains("error")); // 被拒会是 {"error":...}
        CHECK(j.value("ok", false));
    }
}

// 用户在 GUI 里把下拉框逐个选过去点 Attach：日志里实例数 3、4，然后每次都是
// "instance limit reached: 4"。模拟器的 /attach 是"追加一个实例"，GUI 的 Attach 却想表达"切换到这个设备"。
TEST_CASE("controller: the GUI Attach plan replaces the device instead of piling up instances") {
    Host host;
    const auto port = host.h.http_port();
    auto run = [&](const std::string& profile) {
        std::string out;
        for (const auto& step : BuildAttachPlan(profile)) {
            INFO("step " << step.what << " " << step.path);
            REQUIRE(http_post(port, step.path, step.body, &out));
        }
        return json::parse(out, nullptr, false); // 最后一步（attach）的应答
    };
    // 比上限（4）多切换几次：每次都应成功，实例数始终是 1，而且是刚选的那个
    for (const char* name : {"uac2-48k", "uac1-cmedia-sim", "uac2-hid-headset", "uac1-razer-clio", "uac2-48k", "uac1-cmedia-sim"}) {
        INFO("switching to " << name);
        const auto j = run(name);
        REQUIRE(j.is_object());
        CHECK_FALSE(j.contains("error"));
        CHECK(j.value("attached", "") == name);
        CHECK(j.value("instances", 0) == 1);
        CHECK(j.value("busid", "") == "1-1");
    }
    // 对照（旧行为）：只发 /attach 不先 /detach，第 5 次就撞上限——证明上面的断言有牙齿
    host.h.on_detach();
    std::string out;
    int ok = 0;
    for (int i = 0; i < 6; ++i) {
        REQUIRE(http_post(port, "/attach", BuildAttachBody("uac2-48k"), &out));
        if (!json::parse(out).contains("error")) ++ok;
    }
    CHECK(ok == 4);
}

TEST_CASE("controller: the attach plan is detach first, then attach, and carries the profile") {
    const auto plan = BuildAttachPlan("uac1-razer-clio");
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].path == "/detach");
    CHECK(plan[1].path == "/attach");
    CHECK(json::parse(plan[1].body).value("profile", "") == "uac1-razer-clio");
    CHECK_FALSE(plan[0].what.empty());
    CHECK_FALSE(plan[1].what.empty());
}

TEST_CASE("controller: DescribeStatus names the profile and the instance count") {
    const auto idle = DescribeStatus(R"({"listening":false,"imported":false,"profile":"","instances":0})");
    CHECK(idle.led == 0);
    CHECK(idle.text.find(L"Idle") != std::wstring::npos);

    const auto one = DescribeStatus(R"({"listening":true,"imported":false,"profile":"uac1-razer-clio","instances":1})");
    CHECK(one.led == 1);
    CHECK(one.text.find(L"uac1-razer-clio") != std::wstring::npos);
    CHECK(one.text.find(L"instances") == std::wstring::npos); // 只有一个时不啰嗦

    const auto many = DescribeStatus(R"({"listening":true,"imported":true,"profile":"uac2-hid-headset","instances":3})");
    CHECK(many.led == 2);
    CHECK(many.text.find(L"uac2-hid-headset") != std::wstring::npos);
    CHECK(many.text.find(L"3 instances") != std::wstring::npos);

    // 坏输入不崩、不乱报：当作"没有信息"
    CHECK(DescribeStatus("").led == 0);
    CHECK(DescribeStatus("not json").led == 0);
}

TEST_CASE("controller: every profile in the dropdown can be attached") {
    Host host;
    for (const auto& w : ProfileNames()) {
        std::string name;
        for (const wchar_t c : w) name.push_back(static_cast<char>(c)); // 名字全是 ASCII
        INFO("profile " << name);
        host.h.on_detach(); // 每次从空开始，不触及 4 实例上限
        const auto j = json::parse(host.h.on_attach(name));
        CHECK(j.value("attached", "") == name);
    }
}

TEST_CASE("controller: sine body accepts plain decimals only") {
    auto hz = [](const char* t) {
        const auto b = BuildSineBody(t);
        return b.empty() ? -1.0 : json::parse(b).value("hz", -2.0);
    };
    CHECK(hz("1000") == 1000.0);
    CHECK(hz("440.5") == 440.5);
    CHECK(hz("1e3") == 1000.0);
    CHECK(hz("24000") == 24000.0);
    CHECK(json::parse(BuildSineBody("1000")).value("kind", "") == "sine");

    for (const char* bad : {"", "abc", "440abc", " 440", "0", "-1", "24001", "nan", "inf", "0x10",
                            "1,5", "1e999"})
        CHECK_MESSAGE(BuildSineBody(bad).empty(), "input [" << bad << "] must be rejected");
}

TEST_CASE("controller: capture / attach bodies are valid JSON for awkward strings") {
    const char bs = 0x5C;
    const std::string path = std::string("C:") + bs + "tmp" + bs + "a\"b" + '\t' + ".wav";
    const auto j = json::parse(BuildCaptureBody(path));
    CHECK(j.value("path", "") == path); // 反斜杠、引号、制表符都能往返
    CHECK(json::parse(BuildCaptureBody("")).value("path", "x") == ""); // 空 = 停止录音

    const std::string name = std::string("a") + bs + "b\"c";
    CHECK(json::parse(BuildAttachBody(name)).value("profile", "") == name);
}
