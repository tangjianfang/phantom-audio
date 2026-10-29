// AudioControl 处理器实现：标准 EP0 + UAC1 类请求 + 全量 NDJSON 日志
#include <uac/ac_processor.h>

#include <chrono>

namespace uac {

namespace {
// bRequest 码
constexpr std::uint8_t kGetStatus = 0x00, kSetInterface = 0x0B, kGetInterface = 0x0A,
                        kSetConfiguration = 0x09, kGetConfiguration = 0x08,
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
                                  std::map<std::uint8_t, std::vector<std::uint8_t>> strings) {
    dev_ = std::move(device);
    cfg_ = std::move(config);
    strings_ = std::move(strings);
}

void AcProcessor::log_req(std::uint8_t bmRequestType, std::uint8_t bRequest, std::uint16_t wValue,
                          std::uint16_t wIndex, std::uint16_t wLength, const char* outcome,
                          std::size_t n) {
    log_.append("{\"ts\":" + std::to_string(now_ms()) +
                ",\"bmRequestType\":" + std::to_string(bmRequestType) +
                ",\"bRequest\":" + std::to_string(bRequest) +
                ",\"wValue\":" + std::to_string(wValue) +
                ",\"wIndex\":" + std::to_string(wIndex) +
                ",\"wLength\":" + std::to_string(wLength) +
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
                    auto it = strings_.find(wValue & 0xff);
                    if (it == strings_.end()) return true;
                    trunc(it->second);
                    return false;
                }
                default: return true; // qualifier 等：bcdUSB1.1 不支持
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
        log_req(bmRequestType, bRequest, wValue, wIndex, wLength, "stall", 0);
    } else {
        log_req(bmRequestType, bRequest, wValue, wIndex, wLength, "ok", res.data.size());
    }
    return res;
}

} // namespace uac
