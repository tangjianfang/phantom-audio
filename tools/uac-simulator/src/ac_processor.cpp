// AudioControl 处理器实现：标准 EP0 + UAC1 类请求 + 全量 NDJSON 日志
#include <uac/ac_processor.h>

#include <chrono>

namespace uac {

namespace {
// bRequest 码
constexpr std::uint8_t kGetStatus = 0x00, kClearFeature = 0x01, kSetFeature = 0x03, kSetInterface = 0x0B,
                        kGetInterface = 0x0A, kSetConfiguration = 0x09, kGetConfiguration = 0x08,
                        kGetDescriptor = 0x06;
constexpr std::uint8_t kSetCur = 0x01, kGetCur = 0x81, kGetMin = 0x82, kGetMax = 0x83,
                        kGetRes = 0x84;
constexpr std::uint8_t kCsMute = 0x01, kCsVol = 0x02, kCsSamFreq = 0x01;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::int16_t rd_le16(std::span<const std::uint8_t> p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
}
std::uint32_t rd_le24(std::span<const std::uint8_t> p) {
    return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
}
} // namespace

AcProcessor::AcProcessor(AcState& state, LogStore& log) : st_(state), log_(log) {}

void AcProcessor::set_descriptors(std::vector<std::uint8_t> device,
                                  std::vector<std::uint8_t> config,
                                  std::map<std::uint8_t, std::vector<std::uint8_t>> strings,
                                  std::vector<std::uint8_t> qualifier) {
    dev_ = std::move(device);
    cfg_ = std::move(config);
    strings_ = std::move(strings);
    qualifier_ = std::move(qualifier);
}

// Is there an endpoint with this address (bit 7 = direction) in the configuration descriptor?
bool AcProcessor::has_endpoint(std::uint8_t address) const {
    for (std::size_t off = 0; off + 4 <= cfg_.size() && cfg_[off] >= 2; off += cfg_[off])
        if (cfg_[off + 1] == 0x05 && cfg_[off] >= 7 && cfg_[off + 2] == address) return true;
    return false;
}

void AcProcessor::log_req(std::uint8_t bmRequestType, std::uint8_t bRequest, std::uint16_t wValue,
                          std::uint16_t wIndex, std::uint16_t wLength, const char* outcome,
                          std::size_t n, const std::uint8_t* outData, std::size_t outLen) {
    std::string data;
    if ((bmRequestType & 0x80) == 0 && outData && outLen) { // host-to-device: record what was written
        static constexpr char kHex[] = "0123456789abcdef";
        data = ",\"data\":\"";
        for (std::size_t i = 0; i < outLen && i < 8; ++i) {
            data += kHex[outData[i] >> 4];
            data += kHex[outData[i] & 0xf];
        }
        data += "\"";
    }
    log_.append("{\"ts\":" + std::to_string(now_ms()) +
                ",\"bmRequestType\":" + std::to_string(bmRequestType) +
                ",\"bRequest\":" + std::to_string(bRequest) +
                ",\"wValue\":" + std::to_string(wValue) +
                ",\"wIndex\":" + std::to_string(wIndex) +
                ",\"wLength\":" + std::to_string(wLength) + data +
                ",\"resp\":\"" + outcome + "\",\"n\":" + std::to_string(n) + "}");
}

ControlResult AcProcessor::on_control(std::uint8_t bmRequestType, std::uint8_t bRequest,
                                      std::uint16_t wValue, std::uint16_t wIndex,
                                      std::uint16_t wLength,
                                      std::span<const std::uint8_t> outData) {
    ++requestCount_;
    ControlResult res;
    const auto trunc = [&](const std::vector<std::uint8_t>& src) {
        res.kind = ControlResult::Kind::Ok;
        res.data.assign(src.begin(), src.begin() + std::min<std::size_t>(src.size(), wLength));
    };
    const bool isStall = [&] {
        // 返回 true 表示已置为 Stall
        const std::uint8_t type = (bmRequestType >> 5) & 0x03;
        const std::uint8_t recipient = bmRequestType & 0x1f;

        if (type == 0x00 && recipient == 0x00) { // standard/device
            switch (bRequest) {
            case kGetDescriptor: {
                switch (wValue >> 8) {
                case 0x01: trunc(dev_); return false;
                case 0x02: trunc(cfg_); return false;
                case 0x03: {
                    const std::uint8_t index = wValue & 0xff;
                    auto it = strings_.find(index);
                    if (it == strings_.end()) {
                        // USB 2.0 §9.6.7: index 0 is the list of supported LANGIDs. We serve English (US) only; every other
                        // string request is answered regardless of wIndex (Windows retries with LANGID 0 after a STALL).
                        // Before this, index 0 stalled and Windows could not learn that the device has strings at all.
                        if (index != 0 || strings_.empty()) return true;
                        static const std::vector<std::uint8_t> kLangId{0x04, 0x03, 0x09, 0x04};
                        trunc(kLangId);
                        return false;
                    }
                    trunc(it->second);
                    return false;
                }
                case 0x0F: // BOS（MS OS 2.0 平台能力入口）
                    if (bos_.empty()) return true;
                    trunc(bos_);
                    return false;
                case 0x06: // Device Qualifier（HS 设备必需，UAC2）
                    if (qualifier_.empty()) return true;
                    trunc(qualifier_);
                    return false;
                default: return true;
                }
            }
            case kGetConfiguration: res.kind = ControlResult::Kind::Ok; res.data = {1}; return false;
            case kSetConfiguration: res.kind = ControlResult::Kind::Ok; return false;
            case kGetStatus: res.kind = ControlResult::Kind::Ok; res.data = {0, 0}; return false;
            default: return true;
            }
        }
        if (type == 0x00 && recipient == 0x01) { // standard/interface
            switch (bRequest) {
            case kSetInterface:
            case kGetInterface: res.kind = ControlResult::Kind::Ok; return false; // 状态由 DeviceModel 维护
            default: return true;
            }
        }
        if (type == 0x00 && recipient == 0x02) { // standard/endpoint
            // USB 2.0 §9.4: halt can be cleared/set and read on any endpoint that exists. The second Sandbox run showed
            // usbaudio2.sys sending CLEAR_FEATURE(ENDPOINT_HALT) to the interrupt endpoint 0x81 during start-up and getting a STALL.
            // Nothing in this simulator ever halts, so the answer is always "not halted"; unknown endpoints still stall.
            if (!has_endpoint(static_cast<std::uint8_t>(wIndex & 0xff))) return true;
            switch (bRequest) {
            case kGetStatus: res.kind = ControlResult::Kind::Ok; res.data = {0, 0}; return false;
            case kClearFeature:
            case kSetFeature:
                if (wValue != 0x0000) return true; // only ENDPOINT_HALT (feature selector 0) is defined for endpoints
                res.kind = ControlResult::Kind::Ok;
                return false;
            default: return true;
            }
        }
        if (type == 0x01 && recipient == 0x01 && proto_ == Protocol::Uac2) {
            // UAC2：CUR=0x01 / RANGE=0x02；wIndex 高字节实体、wValue 高字节 CS
            const std::uint8_t entity = wIndex >> 8;
            const std::uint8_t cs = wValue >> 8;
            const std::uint8_t cn = wValue & 0xff;
            const bool isFu = std::find(fuIds_.begin(), fuIds_.end(), entity) != fuIds_.end();
            const bool isClock = (entity == clockId_);
            if (!isFu && !isClock) return true;
            const std::size_t idx = cn == 0 ? 0 : cn - 1;
            if (idx > 1) return true;
            const bool isCur = bRequest == 0x01, isRange = bRequest == 0x02;
            if (!isCur && !isRange) return true;
            auto put32 = [&](std::uint32_t x) {
                res.kind = ControlResult::Kind::Ok;
                for (int i = 0; i < 4; ++i) res.data.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
            };
            auto put16v = [&](std::int16_t x) {
                res.kind = ControlResult::Kind::Ok;
                res.data.push_back(static_cast<std::uint8_t>(x));
                res.data.push_back(static_cast<std::uint8_t>(x >> 8));
            };
            if (isClock && cs == 0x01) { // CS_SAM_FREQ_CONTROL
                if (isCur && (bmRequestType & 0x80) == 0) { // SET CUR（OUT 数据 4B LE）
                    if (outData.size() < 4) return true;
                    st_.sampleRate = static_cast<std::uint32_t>(outData[0]) |
                                     (static_cast<std::uint32_t>(outData[1]) << 8) |
                                     (static_cast<std::uint32_t>(outData[2]) << 16) |
                                     (static_cast<std::uint32_t>(outData[3]) << 24);
                    res.kind = ControlResult::Kind::Ok;
                    return false;
                }
                if (isCur) { put32(st_.sampleRate); trunc(res.data); return false; }
                // RANGE：numSubRanges(2) + {MIN,MAX,RES}×n（离散档 MIN=MAX, RES=0）
                res.kind = ControlResult::Kind::Ok;
                res.data.assign({static_cast<std::uint8_t>(rates_.size()),
                                 static_cast<std::uint8_t>(rates_.size() >> 8)});
                for (auto r : rates_) { // 逐子范围 {MIN,MAX,RES}（离散档 MIN=MAX, RES=0）
                    for (int rep = 0; rep < 2; ++rep)
                        for (int i = 0; i < 4; ++i) res.data.push_back(static_cast<std::uint8_t>(r >> (8 * i)));
                    for (int i = 0; i < 4; ++i) res.data.push_back(0);
                }
                trunc(res.data);
                return false;
            }
            if (isClock && cs == 0x02) { // CS_CLOCK_VALID_CONTROL：CUR 恒 1
                if (isCur && (bmRequestType & 0x80) == 0) { res.kind = ControlResult::Kind::Ok; return false; }
                if (isCur) { res.kind = ControlResult::Kind::Ok; res.data = {1}; return false; }
                return true;
            }
            if (isFu && cs == 0x02) { // Volume
                if (isCur && (bmRequestType & 0x80) == 0) {
                    if (outData.size() < 2) return true;
                    st_.volCur[idx] = rd_le16(outData);
                    if (cn == 0) st_.volCur[1] = st_.volCur[0];
                    res.kind = ControlResult::Kind::Ok;
                    return false;
                }
                if (isCur) { put16v(st_.volCur[idx]); trunc(res.data); return false; }
                res.kind = ControlResult::Kind::Ok;
                res.data = {0x01, 0x00}; // 1 个子范围
                put16v(st_.volMin); put16v(st_.volMax); put16v(st_.volRes);
                trunc(res.data);
                return false;
            }
            if (isFu && cs == 0x01) { // Mute
                if (isCur && (bmRequestType & 0x80) == 0) {
                    if (outData.empty()) return true;
                    st_.mute[idx] = outData[0] != 0;
                    if (cn == 0) st_.mute[1] = st_.mute[0];
                    res.kind = ControlResult::Kind::Ok;
                    return false;
                }
                if (isCur) { res.kind = ControlResult::Kind::Ok; res.data = {st_.mute[idx]}; return false; }
                return true;
            }
            return true;
        }
        if (type == 0x01 && recipient == 0x01) { // class/interface → UAC1 音频控制
            const std::uint8_t entity = wIndex >> 8;
            const std::uint8_t cs = wValue >> 8;
            const std::uint8_t cn = wValue & 0xff; // 0=master, 1..n=声道
            const bool isFu = std::find(fuIds_.begin(), fuIds_.end(), entity) != fuIds_.end();
            if (!isFu) return true;
            const std::size_t idx = cn == 0 ? 0 : cn - 1;
            if (idx > 1) return true;
            if (cs == kCsVol) {
                switch (bRequest) {
                case kSetCur:
                    if (outData.size() < 2) return true;
                    st_.volCur[idx] = rd_le16(outData);
                    if (cn == 0) st_.volCur[1] = st_.volCur[0]; // master 双声道同写
                    res.kind = ControlResult::Kind::Ok;
                    return false;
                case kGetCur: res.kind = ControlResult::Kind::Ok;
                    res.data = {static_cast<std::uint8_t>(st_.volCur[idx] & 0xff),
                                static_cast<std::uint8_t>((st_.volCur[idx] >> 8) & 0xff)};
                    return false;
                case kGetMin: res.kind = ControlResult::Kind::Ok;
                    res.data = {static_cast<std::uint8_t>(st_.volMin & 0xff),
                                static_cast<std::uint8_t>((st_.volMin >> 8) & 0xff)};
                    return false;
                case kGetMax: res.kind = ControlResult::Kind::Ok;
                    res.data = {static_cast<std::uint8_t>(st_.volMax & 0xff),
                                static_cast<std::uint8_t>((st_.volMax >> 8) & 0xff)};
                    return false;
                case kGetRes: res.kind = ControlResult::Kind::Ok;
                    res.data = {static_cast<std::uint8_t>(st_.volRes & 0xff),
                                static_cast<std::uint8_t>((st_.volRes >> 8) & 0xff)};
                    return false;
                default: return true;
                }
            }
            if (cs == kCsMute) {
                switch (bRequest) {
                case kSetCur:
                    if (outData.empty()) return true;
                    st_.mute[idx] = outData[0] != 0;
                    if (cn == 0) st_.mute[1] = st_.mute[0];
                    res.kind = ControlResult::Kind::Ok;
                    return false;
                case kGetCur: res.kind = ControlResult::Kind::Ok; res.data = {st_.mute[idx]};
                    return false;
                default: return true;
                }
            }
            return true;
        }
        if (type == 0x01 && recipient == 0x02) { // class/endpoint → 采样率
            if ((wValue >> 8) != kCsSamFreq) return true;
            switch (bRequest) {
            case kSetCur:
                if (outData.size() < 3) return true;
                st_.sampleRate = rd_le24(outData);
                res.kind = ControlResult::Kind::Ok;
                return false;
            case kGetCur: {
                res.kind = ControlResult::Kind::Ok;
                res.data = {static_cast<std::uint8_t>(st_.sampleRate),
                            static_cast<std::uint8_t>(st_.sampleRate >> 8),
                            static_cast<std::uint8_t>(st_.sampleRate >> 16)};
                return false;
            }
            default: return true;
            }
        }
        return true;
    }();

    if (isStall) {
        res.kind = ControlResult::Kind::Stall;
        res.data.clear();
        log_req(bmRequestType, bRequest, wValue, wIndex, wLength, "stall", 0, outData.data(),
                outData.size());
    } else {
        log_req(bmRequestType, bRequest, wValue, wIndex, wLength, "ok", res.data.size(),
                outData.data(), outData.size());
    }
    return res;
}

} // namespace uac
