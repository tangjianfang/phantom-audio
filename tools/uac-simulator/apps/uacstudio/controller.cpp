// uacstudio 控制层实现
#include "controller.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdlib>
#include <set>

namespace uacstudio {

using nlohmann::json;

const std::vector<Action>& Actions() {
    // Order = layout order: the first kActionsInFirstRow in row one, the rest in row two.
    // Bodies match SimHost's on_event / on_inject (the unit tests send every button to a real simulator).
    // Labels and log names are English on purpose: the Windows Sandbox image has no CJK fonts and Chinese showed as boxes.
    static const std::vector<Action> kActions = {
        {200, L"Vol +", "/event", R"({"type":"volume_up"})", "Vol +", Tone::Neutral},
        {201, L"Vol -", "/event", R"({"type":"volume_down"})", "Vol -", Tone::Neutral},
        {202, L"Mute", "/event", R"({"type":"mute"})", "Mute", Tone::Neutral},
        {203, L"HID Vol+", "/event", R"({"type":"hid_vol_up"})", "HID Vol+", Tone::Neutral},
        {204, L"HID Vol-", "/event", R"({"type":"hid_vol_down"})", "HID Vol-", Tone::Neutral},
        {205, L"HID Mute", "/event", R"({"type":"hid_mute"})", "HID Mute", Tone::Neutral},
        {206, L"HID Release", "/event", R"({"type":"hid_release"})", "HID Release", Tone::Quiet},
        {210, L"STALL", "/inject", R"({"kind":"stall","bRequest":129,"nth":1})", "STALL", Tone::Neutral},
        {211, L"Delay", "/inject", R"({"kind":"delay_ms","bRequest":6,"ms":200})", "Delay 200 ms", Tone::Neutral},
        {212, L"Unplug", "/inject", R"({"kind":"disconnect","afterMs":0})", "Unplug", Tone::Fault},
        {213, L"Clear", "/inject", R"({"kind":"clear"})", "Clear faults", Tone::Quiet},
    };
    return kActions;
}

const Action* FindAction(int id) {
    for (const auto& a : Actions())
        if (a.id == id) return &a;
    return nullptr;
}

const std::vector<std::wstring>& ProfileNames() {
    static const std::vector<std::wstring> k = {L"uac2-48k", L"uac1-cmedia-sim", L"uac2-hid-headset",
                                                L"uac2-sim", L"uac1-razer-clio"};
    return k;
}

std::string BuildAttachBody(const std::string& profile) { return json{{"profile", profile}}.dump(); }

std::vector<Step> BuildAttachPlan(const std::string& profile) {
    return {{"/detach", "", "Detach (before switching)"}, {"/attach", BuildAttachBody(profile), "Attach"}};
}

StatusView DescribeStatus(const std::string& statusBody) {
    StatusView v;
    v.text = L"Idle - pick a profile, then Attach";
    const auto j = json::parse(statusBody, nullptr, false);
    if (!j.is_object()) return v;
    const bool listening = j.value("listening", false);
    const bool imported = j.value("imported", false);
    if (!listening && !imported) return v;
    // Profile names are restricted to [A-Za-z0-9_-] by the simulator, so widening byte by byte is safe
    std::wstring profile;
    if (j.contains("profile") && j.at("profile").is_string())
        for (const char c : j.at("profile").get<std::string>()) profile.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    const int instances = j.value("instances", 0);
    v.led = imported ? 2 : 1;
    v.text = std::wstring(imported ? L"Imported" : L"Listening") + (profile.empty() ? L"" : L": " + profile) +
             (imported ? L" - held by the virtual host controller" : L" - waiting for usbip attach") +
             (instances > 1 ? L" (" + std::to_wstring(instances) + L" instances)" : L"");
    return v;
}

std::string BuildCaptureBody(const std::string& wavPath) { return json{{"path", wavPath}}.dump(); }

std::string BuildSineBody(const std::string& hzText) {
    if (hzText.empty()) return {};
    // strtod 还会接受十六进制浮点、"nan"、"inf"：先按字面限定为十进制数字写法
    if (hzText.find_first_not_of("0123456789.+-eE") != std::string::npos) return {};
    char* end = nullptr;
    const double hz = std::strtod(hzText.c_str(), &end);
    if (end == hzText.c_str() || *end != '\0') return {}; // 含非数字尾巴（"440abc"）
    if (!std::isfinite(hz) || hz <= 0.0 || hz > 24000.0) return {};
    return json{{"kind", "sine"}, {"hz", hz}}.dump();
}

std::string ValidateActions() { return ValidateActionTable(Actions()); }

std::string ValidateActionTable(const std::vector<Action>& table) {
    static const std::set<std::string> kRoutes = {"/event", "/inject", "/source", "/capture",
                                                  "/attach", "/detach"};
    std::set<int> ids;
    std::string errors;
    for (const auto& a : table) {
        const std::string name = a.what ? a.what : "?";
        if (!ids.insert(a.id).second) errors += "duplicate action id " + std::to_string(a.id) + "; ";
        if (!a.label || !*a.label) errors += name + ": empty label; ";
        if (!a.path || !kRoutes.count(a.path)) errors += name + ": unknown route; ";
        if (!a.what || !*a.what) errors += "action " + std::to_string(a.id) + ": empty log name; ";
        const auto j = json::parse(a.body ? a.body : "", nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) errors += name + ": body is not a JSON object; ";
    }
    return errors;
}

} // namespace uacstudio
