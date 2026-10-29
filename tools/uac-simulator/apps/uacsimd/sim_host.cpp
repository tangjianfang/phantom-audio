// SimHost 实现
#include "sim_host.h"
#include <uac/version.h>
#include <nlohmann/json.hpp>
#include <windows.h>

namespace uac {

using nlohmann::json;

SimHost::SimHost(std::string profilesDir) : profiles_dir_(std::move(profilesDir)) {}

bool SimHost::start(std::uint16_t usbipPort, std::uint16_t httpPort) {
    ControlCallbacks cb;
    cb.status = [this] { return on_status(); };
    cb.log_tail = [this](std::size_t n) {
        std::lock_guard lk(mtx_);
        if (!model_) return std::string("[]");
        json arr = json::array();
        for (auto& l : model_->log().tail(n)) arr.push_back(l);
        return arr.dump();
    };
    cb.clear_log = [this] {
        std::lock_guard lk(mtx_);
        if (model_) model_->log().clear();
    };
    cb.attach = [this](const std::string& p) { return on_attach(p); };
    cb.detach = [this] { return on_detach(); };
    cb.event = [this](const std::string& e) { return on_event(e); };
    cb.inject = [this](const std::string& b) { return on_inject(b); };
    cb.source = [this](const std::string& b) { return on_source(b); };
    cb.capture = [this](const std::string& b) { return on_capture(b); };

    if (!api_.start(httpPort, std::move(cb))) return false;
    running_.store(true);
    return true;
}

void SimHost::stop() {
    running_.store(false);
    on_detach();
    api_.stop();
}

std::string SimHost::on_status() {
    std::lock_guard lk(mtx_);
    json j;
    j["version"] = kVersion;
    j["running"] = running_.load();
    j["profile"] = current_profile_;
    j["instances"] = models_.size();
    j["listening"] = model_ != nullptr;      // usbip 端口在监听（可 attach）
    j["imported"] = server_.attached();       // vhci 已 IMPORT（真占用中）
    if (model_) {
        auto ac = model_->ac_state();
        j["volCurDb"] = ac.volCur[0] / 256.0;
        j["mute"] = ac.mute[0];
        j["sampleRate"] = ac.sampleRate;
        j["logCount"] = model_->log().count();
        auto st = model_->engine().stats();
        j["inBytes"] = st.inBytes;
        j["outBytes"] = st.outBytes;
        j["interfaceAlt"] = json::object();
    }
    return j.dump();
}

std::string SimHost::on_attach(const std::string& profileName) {
    std::lock_guard lk(mtx_);
    try {
        namespace fs = std::filesystem;
        // profile 名可带 .json 也可不带；也可为内置名
        std::string path;
        // Fallback resolution against the exe directory: local only, so a
        // caller-supplied --profiles dir is never silently overwritten.
        std::string dir = profiles_dir_;
        if (!std::filesystem::exists(dir)) {
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
                namespace fs = std::filesystem;
                const fs::path exeDir = fs::path(exe).parent_path();
                for (auto cand : {exeDir / "profiles", exeDir.parent_path() / "profiles",
                                  exeDir.parent_path().parent_path() / "tools" /
                                      "uac-simulator" / "profiles"}) {
                    if (fs::exists(cand)) {
                        dir = cand.string();
                        break;
                    }
                }
            }
        }
        // resolve profile file inside the (possibly relocated) dir
        for (auto cand : {profileName + ".json", profileName}) {
            if (fs::exists(fs::path(dir) / cand)) {
                path = (fs::path(dir) / cand).string();
                break;
            }
        }
        DeviceSnapshot snap;
        if (!path.empty()) {
            snap = build_snapshot(load_profile(path));
        } else if (profileName == "uac1-cmedia-sim") {
            snap = make_uac1_default_snapshot();
        } else if (profileName == "uac1-hid-sim") {
            snap = make_uac1_hid_snapshot();
        } else if (profileName == "uac2-sim") {
            snap = make_uac2_default_snapshot();
        } else {
            return json{{"error", "profile not found: " + profileName}}.dump();
        }
        if (models_.size() >= 4)
            return json{{"error", "实例已达上限 4（vhci 端口数）"}}.dump();
        server_.stop(); // 重建清单：断开现有连接（vhci 侧表现为拔出再插入）
        auto model = std::make_unique<DeviceModel>(std::move(snap));
        model->engine().set_source_sine(1000.0, 0.5);
        model->set_disconnect_hook([this] { server_.kick(); });
        if (!logFile_.empty()) model->log().set_file_sink(logFile_); // per-model NDJSON sink
        models_.emplace_back(profileName, std::move(model));
        model_ = models_.back().second.get();
        // 重建导出清单：每实例独立 busid/devnum，且与各自 dispatcher 配对，
        // 使 URB 按导入的 busid 路由到正确的设备模型（此前全部走 models_[0]）
        std::vector<UsbIpServer::Export> list;
        for (std::size_t i = 0; i < models_.size(); ++i) {
            auto d = models_[i].second->usbip_device();
            d.busid = "1-" + std::to_string(i + 1);
            d.devnum = static_cast<std::uint32_t>(i + 1);
            d.path = "/uacsim/inst" + std::to_string(i);
            list.emplace_back(std::move(d), models_[i].second.get());
        }
        if (!server_.start(usbipPort_, list))
            return json{{"error", "usbip listen " + std::to_string(usbipPort_) + " failed"}}
                .dump();
        current_profile_ = profileName;
        return json{{"attached", profileName},
                    {"busid", "1-" + std::to_string(models_.size())},
                    {"instances", models_.size()}}.dump();
    } catch (const std::exception& e) {
        return json{{"error", e.what()}}.dump();
    }
}

std::string SimHost::on_detach() {
    std::lock_guard lk(mtx_);
    server_.stop();
    models_.clear();
    model_ = nullptr;
    current_profile_.clear();
    return json{{"ok", true}}.dump();
}

std::string SimHost::on_event(const std::string& type) {
    std::lock_guard lk(mtx_);
    if (!model_) return json{{"error", "not attached"}}.dump();
    if (type == "volume_up") model_->inject_volume_step(1);
    else if (type == "volume_down") model_->inject_volume_step(-1);
    else if (type == "mute") model_->inject_mute_toggle();
    else if (type == "hid_vol_up") model_->inject_hid_keys(hid::kVolUp);
    else if (type == "hid_vol_down") model_->inject_hid_keys(hid::kVolDown);
    else if (type == "hid_mute") model_->inject_hid_keys(hid::kMute);
    else if (type == "hid_release") model_->inject_hid_keys(0);
    else return json{{"error", "unknown event: " + type}}.dump();
    return json{{"ok", true}, {"event", type}}.dump();
}

std::string SimHost::on_inject(const std::string& body) {
    std::lock_guard lk(mtx_);
    if (!model_) return json{{"error", "not attached"}}.dump();
    try {
        const auto j = json::parse(body.empty() ? "{}" : body);
        const auto kind = j.value("kind", std::string());
        FaultRule r;
        if (kind == "stall") {
            r.kind = FaultRule::Kind::StallNth;
            r.bRequest = j.value("bRequest", -1);
            r.bmRequestType = j.value("bmRequestType", -1);
            r.nth = j.value("nth", 1);
        } else if (kind == "delay_ms") {
            r.kind = FaultRule::Kind::DelayMs;
            r.bRequest = j.value("bRequest", -1);
            r.ms = j.value("ms", 100);
        } else if (kind == "corrupt_desc_at") {
            r.kind = FaultRule::Kind::CorruptDescByteAt;
            r.byteOffset = j.value("byteOffset", 100);
            r.xorByte = j.value<unsigned char>("xor", 0xFF);
        } else if (kind == "half_enumerate") {
            r.kind = FaultRule::Kind::HalfEnumerate;
            r.stopAtStep = j.value("stopAtStep", 2);
        } else if (kind == "disconnect") {
            r.kind = FaultRule::Kind::Disconnect;
            r.ms = j.value("afterMs", 0);
        } else if (kind == "iso_error") {
            r.kind = FaultRule::Kind::IsoError;
            r.everyN = j.value("everyN", 2);
            r.errCount = j.value("errCount", 1);
        } else if (kind == "clear") {
            model_->faults().clear();
            return json{{"ok", true}, {"kind", "clear"}}.dump();
        } else {
            return json{{"error", "unknown kind: " + kind}}.dump();
        }
        model_->faults().arm(r);
        return json{{"ok", true}, {"kind", kind}}.dump();
    } catch (const std::exception& e) {
        return json{{"error", e.what()}}.dump();
    }
}

std::string SimHost::on_source(const std::string& body) {
    std::lock_guard lk(mtx_);
    if (!model_) return json{{"error", "not attached"}}.dump();
    try {
        const auto j = json::parse(body.empty() ? "{}" : body);
        const auto kind = j.value("kind", std::string("sine"));
        if (kind == "sine") {
            model_->engine().set_source_sine(j.value("hz", 1000.0), j.value("amp", 0.5));
        } else if (kind == "silence") {
            model_->engine().set_source_silence();
        } else if (kind == "file") {
            model_->engine().set_source_file(j.value("file", std::string()));
        } else {
            return json{{"error", "unknown kind"}}.dump();
        }
        return json{{"ok", true}, {"kind", kind}}.dump();
    } catch (const std::exception& e) {
        return json{{"error", e.what()}}.dump();
    }
}

std::string SimHost::on_capture(const std::string& body) {
    std::lock_guard lk(mtx_);
    if (!model_) return json{{"error", "not attached"}}.dump();
    try {
        const auto j = json::parse(body.empty() ? "{}" : body);
        const auto path = j.value("path", std::string());
        model_->engine().set_capture_sink(path); // 空路径=关闭落盘
        return json{{"ok", true}, {"path", path}}.dump();
    } catch (const std::exception& e) {
        return json{{"error", e.what()}}.dump();
    }
}

} // namespace uac
