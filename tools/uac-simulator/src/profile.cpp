// Profile 解析实现（nlohmann/json）
#include <uac/profile.h>
#include <uac/msos20.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace uac {

using nlohmann::json;

namespace {
std::uint16_t parse_u16(const json& j, const char* key, std::uint16_t def) {
    if (!j.contains(key)) return def;
    const auto& v = j.at(key);
    if (v.is_number_integer()) return static_cast<std::uint16_t>(v.get<int>());
    if (v.is_string()) {
        const auto s = v.get<std::string>();
        try { return static_cast<std::uint16_t>(std::stoul(s, nullptr, 0)); }
        catch (...) { throw ProfileError(std::string("usb.") + key + " is invalid: " + s); }
    }
    throw ProfileError(std::string("usb.") + key + " has the wrong JSON type");
}

std::vector<std::uint8_t> parse_hex_bytes(const std::string& s) {
    std::vector<std::uint8_t> out;
    std::uint8_t cur = 0;
    int nibbles = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else continue; // 空格/换行等分隔符
        cur = static_cast<std::uint8_t>((cur << 4) | d);
        if (++nibbles == 2) {
            out.push_back(cur);
            nibbles = 0;
            cur = 0;
        }
    }
    if (nibbles != 0) throw ProfileError("rawConfigOverride has a dangling half byte (odd number of hex digits)");
    return out;
}
} // namespace

Profile parse_profile_json(const std::string& text, const std::string& name) {
    Profile p;
    p.name = name;
    json j;
    try { j = json::parse(text); }
    catch (const json::exception& e) { throw ProfileError(std::string("JSON parse error: ") + e.what()); }

    if (!j.contains("protocol")) throw ProfileError("missing the protocol field");
    const auto proto = j.at("protocol").get<std::string>();
    if (proto == "uac1") {
        p.protocol = Protocol::Uac1;
        p.subslot = 2; p.bits = 16;
    } else if (proto == "uac2") {
        p.protocol = Protocol::Uac2;
        p.subslot = 4; p.bits = 32;
        p.acInterruptEp = true;
    } else {
        throw ProfileError("protocol must be uac1 or uac2, got: " + proto);
    }

    if (j.contains("usb")) {
        const auto& u = j.at("usb");
        p.vid = parse_u16(u, "vid", p.vid);
        p.pid = parse_u16(u, "pid", p.pid);
        p.bcdDevice = parse_u16(u, "bcdDevice", p.bcdDevice);
        if (u.contains("manufacturer")) p.manufacturer = u.at("manufacturer").get<std::string>();
        if (u.contains("product")) p.product = u.at("product").get<std::string>();
        if (u.contains("serial")) p.serial = u.at("serial").get<std::string>();
    }

    if (j.contains("audio")) {
        const auto& a = j.at("audio");
        if (a.contains("channels")) p.channels = static_cast<std::uint8_t>(a.at("channels").get<int>());
        if (a.contains("rates")) {
            p.rates.clear();
            for (auto& r : a.at("rates")) {
                const auto rate = r.get<std::uint32_t>();
                if (rate < 8000 || rate > 768000) throw ProfileError("sample rate out of range: " + std::to_string(rate));
                p.rates.push_back(rate);
            }
            if (p.rates.empty()) throw ProfileError("rates must not be empty");
        }
        if (a.contains("subslot")) p.subslot = static_cast<std::uint8_t>(a.at("subslot").get<int>());
        if (a.contains("bits")) p.bits = static_cast<std::uint8_t>(a.at("bits").get<int>());
        if (a.contains("micIn")) p.micIn = a.at("micIn").get<bool>();
        if (a.contains("spkOut")) p.spkOut = a.at("spkOut").get<bool>();
        if (a.contains("acInterruptEp")) p.acInterruptEp = a.at("acInterruptEp").get<bool>();
        if (a.contains("feedbackEp")) p.feedbackEp = a.at("feedbackEp").get<bool>();
        if (a.contains("volumeDb")) {
            const auto& v = a.at("volumeDb");
            const auto to256 = [](const json& x, double defaultDb) {
                const double db = x.is_number() ? x.get<double>() : defaultDb;
                return static_cast<std::int16_t>(db * 256.0);
            };
            p.volMin = to256(v.contains("min") ? v.at("min") : json(), -24.0);
            p.volMax = to256(v.contains("max") ? v.at("max") : json(), 0.0);
            p.volRes = to256(v.contains("res") ? v.at("res") : json(), 1.0);
            if (p.volMin >= p.volMax) throw ProfileError("volumeDb: min must be less than max");
        }
    }

    if (j.contains("hid")) {
        if (j.at("hid").contains("consumerVolume"))
            p.hidConsumerVolume = j.at("hid").at("consumerVolume").get<bool>();
    }

    if (j.contains("rawConfigOverride")) {
        p.rawConfigOverride = parse_hex_bytes(j.at("rawConfigOverride").get<std::string>());
        if (p.rawConfigOverride.size() < 9) throw ProfileError("rawConfigOverride is shorter than 9 bytes");
    }
    return p;
}

Profile load_profile(const std::string& jsonPath) {
    std::ifstream f(jsonPath);
    if (!f) throw ProfileError("cannot open profile: " + jsonPath);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto slash = jsonPath.find_last_of("/\\");
    auto name = jsonPath.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return parse_profile_json(ss.str(), name);
}

DeviceSnapshot build_snapshot(const Profile& p) {
    DeviceSnapshot s;
    s.protocol = p.protocol;
    s.rates = p.rates;
    s.clockId = uac2::kClock;

    if (p.protocol == Protocol::Uac1) {
        uac1::Uac1Profile t;
        t.vid = p.vid; t.pid = p.pid; t.bcdDevice = p.bcdDevice;
        t.manufacturer = p.manufacturer; t.product = p.product; t.serial = p.serial;
        t.channels = p.channels; t.rates = p.rates;
        t.vol.minDb = p.volMin; t.vol.maxDb = p.volMax; t.vol.resDb = p.volRes;
        t.micIn = p.micIn; t.spkOut = p.spkOut;
        t.acInterruptEp = p.acInterruptEp;
        s.dev = uac1::build_device_desc(t);
        s.config = uac1::build_config_desc(t);
        s.strings = uac1::build_string_descs(t);
        s.featureUnitIds = {uac1::kFuSpeaker, uac1::kFuMic};
        s.stream = StreamConfig{p.rates.front(), p.channels, 2, 1000, 0};
        s.name = p.name;
        if (p.hidConsumerVolume) {
            s.config = hid::append_hid_interface(s.config, 3);
            s.hidConsumerVolume = true;
            s.hidReportDesc = hid::build_report_desc();
        }
    } else {
        uac2::Uac2Profile t;
        t.vid = p.vid; t.pid = p.pid; t.bcdDevice = p.bcdDevice;
        t.manufacturer = p.manufacturer; t.product = p.product; t.serial = p.serial;
        t.channels = p.channels; t.rates = p.rates;
        t.pcm.subslot = p.subslot; t.pcm.bits = p.bits;
        t.vol.minDb = p.volMin; t.vol.maxDb = p.volMax; t.vol.resDb = p.volRes;
        t.micIn = p.micIn; t.spkOut = p.spkOut;
        t.feedbackEp = p.feedbackEp;
        t.acInterruptEp = p.acInterruptEp;
        s.dev = uac2::build_device_desc(t);
        s.config = uac2::build_config_desc(t);
        s.qualifier = uac2::build_qualifier_desc(t);
        s.strings = uac2::build_string_descs(t);
        s.featureUnitIds = {uac2::kFuSpeaker, uac2::kFuMic};
        s.stream = StreamConfig{p.rates.front(), p.channels, p.subslot, 1000, 0};
        s.name = p.name;
        // MS OS 2.0: BOS entry + jack-registry descriptor set (vendor code 1)
        s.msosVendorCode = 0x01;
        s.bos = msos20::build_bos(0x01);
        s.msosSet = msos20::build_descriptor_set();
    }
    if (p.hidConsumerVolume && p.protocol == Protocol::Uac2) {
        s.config = hid::append_hid_interface(s.config, 3);
        s.hidConsumerVolume = true;
        s.hidReportDesc = hid::build_report_desc();
    }
    if (!p.rawConfigOverride.empty()) s.config = p.rawConfigOverride;
    ude::adapt_config_for_ude(s.config);
    s.ac.volMin = p.volMin; s.ac.volMax = p.volMax; s.ac.volRes = p.volRes;
    s.ac.sampleRate = p.rates.front();
    s.hasSpeakerOut = p.spkOut;
    s.hasMicIn = p.micIn;
    return s;
}

bool snapshot_requires_reattach(const DeviceSnapshot& a, const DeviceSnapshot& b) {
    return a.config != b.config ||
           desc::encode_device_desc(a.dev) != desc::encode_device_desc(b.dev);
}

} // namespace uac
