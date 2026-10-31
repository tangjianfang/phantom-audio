// DeviceModel 实现
#include <uac/device_model.h>
#include <chrono>
#include <memory>
#include <thread>
#include <uac/ude_adapter.h>

namespace uac {

DeviceSnapshot make_uac1_default_snapshot() {
    DeviceSnapshot s;
    uac1::Uac1Profile p;
    s.dev = uac1::build_device_desc(p);
    s.config = uac1::build_config_desc(p);
    ude::adapt_config_for_ude(s.config); // HS + bInterval>=4
    s.strings = uac1::build_string_descs(p);
    s.stream = StreamConfig{};
    s.featureUnitIds = {uac1::kFuSpeaker, uac1::kFuMic};
    s.name = "uac1-cmedia-sim";
    return s;
}

DeviceSnapshot make_uac1_hid_snapshot() {
    DeviceSnapshot s;
    uac1::Uac1Profile p;
    s.dev = uac1::build_device_desc(p);
    s.config = hid::append_hid_interface(uac1::build_config_desc(p), 3);
    ude::adapt_config_for_ude(s.config);
    s.strings = uac1::build_string_descs(p);
    s.stream = StreamConfig{};
    s.featureUnitIds = {uac1::kFuSpeaker, uac1::kFuMic};
    s.hidConsumerVolume = true;
    s.hidReportDesc = hid::build_report_desc();
    s.name = "uac1-hid-sim";
    return s;
}

DeviceSnapshot make_uac2_default_snapshot() {
    DeviceSnapshot s;
    uac2::Uac2Profile p;
    s.protocol = Protocol::Uac2;
    s.dev = uac2::build_device_desc(p);
    s.config = uac2::build_config_desc(p);
    ude::adapt_config_for_ude(s.config);
    s.qualifier = uac2::build_qualifier_desc(p);
    s.strings = uac2::build_string_descs(p);
    s.stream = StreamConfig{48000, 2, 4, 1000, 0}; // 32bit 槽
    s.featureUnitIds = {uac2::kFuSpeaker, uac2::kFuMic};
    s.clockId = uac2::kClock;
    s.rates = p.rates;
    s.name = "uac2-sim";
    return s;
}

DeviceModel::~DeviceModel() {
    // Invariant: destroy only after all in-flight URB dispatch returned (the
    // owning server must be stopped first - SimHost::on_detach guarantees it).
    // The flag below narrows the delay-push race; it cannot make concurrent
    // handle_ep0 on a destroyed object defined behavior.
    // Mark stopped under the mutex FIRST: any racer that acquires the lock
    // afterwards completes inline instead of pushing into the dying vector.
    std::vector<std::thread> pending;
    {
        std::lock_guard lk(delayMtx_);
        delayStopped_ = true;
        pending.swap(delayThreads_);
    }
    for (auto& t : pending)
        if (t.joinable()) t.join();
}

DeviceModel::DeviceModel(DeviceSnapshot snap, std::unique_ptr<MicroframeClock> clock)
    : snap_(std::move(snap)), clock_(clock ? std::move(clock) : std::make_unique<WallClock>()),
      ac_(snap_.ac, log_), engine_(*clock_, log_, snap_.stream),
      injector_(snap_.protocol == Protocol::Uac2 ? 6 : 2) {
    ac_.set_descriptors(desc::encode_device_desc(snap_.dev), snap_.config, snap_.strings,
                        snap_.qualifier);
    if (snap_.msosVendorCode) {
        ac_.set_msos(snap_.bos, snap_.msosSet, snap_.msosVendorCode);
        msosSet_ = snap_.msosSet;
        msosVendorCode_ = snap_.msosVendorCode;
    }
    ac_.set_feature_unit_ids(snap_.featureUnitIds);
    ac_.set_protocol(snap_.protocol);
    ac_.set_rates(snap_.rates);
    ac_.set_clock_id(snap_.clockId);
    // 注入 ManualClock 时关闭自动泵（测试手动 pump）；WallClock 用自动泵
    engine_.set_auto_pump(dynamic_cast<WallClock*>(clock_.get()) != nullptr);
}

usbip::UsbIpDevice DeviceModel::usbip_device() const {
    usbip::UsbIpDevice d;
    d.path = "/uacsim/" + snap_.name;
    d.busid = "1-1";
    d.busnum = 1;
    d.devnum = 1;
    d.speed = 3; // High-Speed —— UDE 适配要求（issue #35）
    d.idVendor = snap_.dev.idVendor;
    d.idProduct = snap_.dev.idProduct;
    d.bcdDevice = snap_.dev.bcdDevice;
    // 接口清单与配置描述符一致：HID 复合快照多出第 4（HID）接口
    const std::uint8_t proto =
        snap_.protocol == Protocol::Uac2 ? std::uint8_t{0x20} : std::uint8_t{0x00};
    d.interfaces = {{0x01, 0x01, proto}, {0x01, 0x02, proto}, {0x01, 0x02, proto}};
    if (snap_.hidConsumerVolume) d.interfaces.push_back({0x03, 0x00, 0x00});
    d.bNumInterfaces = static_cast<std::uint8_t>(d.interfaces.size());
    return d;
}

void DeviceModel::on_import(const usbip::UsbIpDevice&) {
    std::lock_guard lk(mtx_);
    attached_.store(true);
    ++imported_;
    ifaceAlt_.clear();
}

void DeviceModel::on_detach() {
    std::lock_guard lk(mtx_);
    attached_.store(false);
    ifaceAlt_.clear();
}

void DeviceModel::handle_ep0(const usbip::UrbCommand& cmd, std::span<const unsigned char> payload,
                             CompletionFn complete) {
    const auto& setup = cmd.submit.setup;
    const bool isOut = (setup[0] & 0x80) == 0;
    const std::span<const unsigned char> outData = isOut ? payload : std::span<const unsigned char>{};

    // MS OS 2.0 vendor-specific descriptor-set request (bRequest = vendorCode,
    // wIndex=0x07 per MS OS 2.0 spec; bmRequestType 0xC0 device-to-host)
    if (msosSet_ && !msosSet_->empty() && setup[0] == 0xC0 && setup[1] == msosVendorCode_ &&
        (setup[4] | (setup[5] << 8)) == 0x0007) {
        Completion cm;
        cm.ret.actual_length = static_cast<std::uint32_t>(msosSet_->size());
        cm.payload = *msosSet_;
        const auto wLen = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
        if (cm.payload.size() > wLen) {
            cm.payload.resize(wLen);
            cm.ret.actual_length = wLen;
        }
        complete(std::move(cm));
        return;
    }

    // HID 报告描述符请求：GET_DESCRIPTOR(Report, wValue=0x22xx, wIndex=interface)
    if (snap_.hidConsumerVolume && (setup[0] & 0x1f) == 0x01 && setup[1] == 0x06 &&
        setup[3] == 0x22) {
        Completion cm;
        cm.ret.actual_length = static_cast<std::uint32_t>(snap_.hidReportDesc.size());
        cm.payload = snap_.hidReportDesc;
        const auto wLen = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
        if (cm.payload.size() > wLen) {
            cm.payload.resize(wLen);
            cm.ret.actual_length = wLen;
        }
        complete(std::move(cm));
        return;
    }

    // SET_INTERFACE / GET_INTERFACE 在此处理（维护 alt 状态）
    const std::uint8_t type = (setup[0] >> 5) & 0x03;
    const std::uint8_t recipient = setup[0] & 0x1f;
    if (type == 0x00 && recipient == 0x01 && setup[1] == 0x0B) { // SET_INTERFACE
        const std::uint8_t iface = setup[4], alt = setup[2];
        {
            std::lock_guard lk(mtx_);
            ifaceAlt_[iface] = alt;
            log_.append(std::string("{\"event\":\"set_interface\",\"iface\":") +
                        std::to_string(iface) + ",\"alt\":" + std::to_string(alt) + "}");
        }
        if (iface == uac1::kInterfaceMic && alt != 0) engine_.set_rate(snap_.ac.sampleRate);
        Completion cm; // 零长度 Ok
        complete(std::move(cm));
        return;
    }
    if (type == 0x00 && recipient == 0x01 && setup[1] == 0x0A) { // GET_INTERFACE
        std::lock_guard lk(mtx_);
        log_.append(std::string("{\"event\":\"get_interface\",\"iface\":") +
                    std::to_string(setup[4]) + "}");
        Completion cm;
        cm.ret.actual_length = 1;
        cm.payload = {static_cast<unsigned char>(ifaceAlt_.count(setup[4]) ? ifaceAlt_[setup[4]] : 0)};
        complete(std::move(cm));
        return;
    }

    fault_.on_ep0_request();
    // AcState is shared with the HTTP-thread injectors: serialize the
    // control-transfer read-modify-write against them.
    std::lock_guard<std::mutex> acLock(mtx_);
    if (fault_.should_disconnect()) {
        log_.append("{\"event\":\"fault_disconnect\"}");
        if (disconnectHook_) disconnectHook_();
        Completion cm; // 静默 STALL：连接已被 kick
        cm.ret.status = -19; // -ENODEV
        complete(std::move(cm));
        return;
    }
    if (fault_.should_stall(setup[0], setup[1])) {
        log_.append("{\"event\":\"fault_stall\"}");
        ++cStall_;
        Completion cm;
        cm.ret.status = -32; // -EPIPE
        complete(std::move(cm));
        return;
    }
    const int delayMs = fault_.delay_for(setup[0], setup[1]);
    auto r = ac_.on_control(setup[0], setup[1],
                            static_cast<std::uint16_t>(setup[2] | (setup[3] << 8)),
                            static_cast<std::uint16_t>(setup[4] | (setup[5] << 8)),
                            static_cast<std::uint16_t>(setup[6] | (setup[7] << 8)), outData);
    // 配置描述符损坏注入（GET_DESCRIPTOR config）
    if ((setup[1] == 0x06) && (setup[3] == 0x02) && r.kind == ControlResult::Kind::Ok)
        fault_.mutate_config_response(r.data); // 仅损坏注入；断连由 should_disconnect 一次性触发
    // UAC2 clock SET_CUR (and UAC1 endpoint SET_CUR) update st_.sampleRate
    // inside AcProcessor; mirror it into the streaming engine so IN packets
    // are paced and sized at the negotiated rate.
    if (snap_.ac.sampleRate != lastRate_) {
        lastRate_ = snap_.ac.sampleRate;
        engine_.set_rate(lastRate_);
    }
    Completion cm;
    if (r.kind == ControlResult::Kind::Stall) {
        ++cStall_;
        cm.ret.status = -32; // -EPIPE（STALL 的 Linux errno）
    } else {
        ++cOk_;
        // 控制写（OUT 带数据阶段，如 SET_CUR）：URB 完成须上报数据阶段实际字节数。
        // 早期这里对 SET 恒报 0——usbaudio2 把它当"没写成"无限重发 SET_CUR(采样率)，
        // 设备被选为默认/引擎介入时稳定复现 ~400 次/秒的控制风暴（2026-11-01 真机实测）。
        const bool controlOut = (setup[0] & 0x80) == 0 && !payload.empty();
        cm.ret.actual_length = controlOut ? static_cast<std::uint32_t>(payload.size())
                                          : static_cast<std::uint32_t>(r.data.size());
        cm.payload = std::move(r.data);
    }
    if (delayMs > 0) { // 延迟注入：计时后异步完成
        auto delayed = std::make_shared<std::vector<unsigned char>>(std::move(cm.payload));
        auto status = cm.ret.status;
        auto len = cm.ret.actual_length;
        std::thread th([this, delayMs, delayed, status, len,
                        complete = std::move(complete)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            Completion d;
            d.ret.status = status;
            d.ret.actual_length = len;
            d.payload = std::move(*delayed);
            complete(std::move(d));
        });
        bool pushed = false;
        {
            std::lock_guard lk(delayMtx_);
            if (!delayStopped_) {
                delayThreads_.push_back(std::move(th));
                pushed = true;
            }
        }
        if (!pushed) th.join(); // racing destruction: run the delay to completion here
        return;
    }
    complete(std::move(cm));
}

std::string DeviceModel::urb_stats_line() {
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    auto d = [](std::atomic<uint64_t>& c) { return c.exchange(0); };
    const uint64_t ep0 = d(cEp0_), ok = d(cOk_), stall = d(cStall_), int81 = d(cInt81_),
                   hid82 = d(cHid82_), fb84 = d(cFb84_), iso86 = d(cIso86_), iso06 = d(cIso06_),
                   unk = d(cUnk_), unl = d(cUnlink_);
    if (!ep0 && !ok && !stall && !int81 && !hid82 && !fb84 && !iso86 && !iso06 && !unk && !unl)
        return {}; // idle second: no line, so the GUI log tail stays readable
    std::size_t heldInt, heldHid;
    {
        std::lock_guard lk(mtx_);
        heldInt = heldInterrupts_.size();
        heldHid = heldHid_.size();
    }
    return "{\"ts\":" + std::to_string(ts) + ",\"event\":\"urb_stats\""
           + ",\"ep0\":" + std::to_string(ep0)
           + ",\"ok\":" + std::to_string(ok)
           + ",\"stall\":" + std::to_string(stall)
           + ",\"int81\":" + std::to_string(int81)
           + ",\"hid82\":" + std::to_string(hid82)
           + ",\"fb84\":" + std::to_string(fb84)
           + ",\"iso86\":" + std::to_string(iso86)
           + ",\"iso06\":" + std::to_string(iso06)
           + ",\"unk\":" + std::to_string(unk)
           + ",\"unl\":" + std::to_string(unl)
           + ",\"heldInt\":" + std::to_string(heldInt)
           + ",\"heldHid\":" + std::to_string(heldHid)
           + "}";
}

void DeviceModel::on_urb(const usbip::UrbCommand& cmd, std::span<const unsigned char> payload,
                         std::span<const usbip::IsoPacket> isoDescs, CompletionFn complete) {
    if (cmd.basic.ep == 0) {
        ++cEp0_;
        handle_ep0(cmd, payload, std::move(complete));
        return;
    }
    const std::uint8_t epAddr = static_cast<std::uint8_t>(
        (cmd.basic.direction == usbip::DIR_IN ? 0x80 : 0x00) | cmd.basic.ep);
    if (epAddr == 0x84) { // UAC2 explicit feedback IN: current Q16.16 value (High-Speed)
        ++cFb84_;
        const auto fb = AsEngine::fb_value_16_16(snap_.ac.sampleRate);
        Completion cm;
        cm.ret.actual_length = 4;
        cm.payload = {static_cast<unsigned char>(fb), static_cast<unsigned char>(fb >> 8),
                      static_cast<unsigned char>(fb >> 16), static_cast<unsigned char>(fb >> 24)};
        complete(std::move(cm));
        return;
    }
    if (epAddr == 0x86) { // 麦克风 iso IN：包一层 iso error_count 注入
        ++cIso86_;
        auto wrapped = [this, complete = std::move(complete)](Completion&& cm) mutable {
            if (auto ec = fault_.iso_error_count()) cm.ret.error_count = ec;
            complete(std::move(cm));
        };
        engine_.on_iso_in(cmd, std::move(wrapped));
        return;
    }
    if (epAddr == 0x82 && snap_.hidConsumerVolume) { // HID 中断 IN：按键报告
        ++cHid82_;
        std::vector<std::uint8_t> report;
        {
            std::lock_guard lk(mtx_);
            if (hidQueue_.size() >= 2) {
                report = {hidQueue_.front(), hidQueue_[1]};
                hidQueue_.erase(hidQueue_.begin(), hidQueue_.begin() + 2);
            }
        }
        if (!report.empty()) {
            Completion cm;
            cm.ret.actual_length = 2;
            cm.payload = std::move(report);
            complete(std::move(cm));
        } else {
            std::lock_guard lk(mtx_);
            heldHid_.push_back({cmd.basic.seqnum, std::move(complete)});
        }
        return;
    }
    if (epAddr == 0x81) { // AC 中断 IN（Status Word）：有挂起通知立即回，否则挂起
        ++cInt81_;
        if (auto w = injector_.poll_interrupt_in()) {
            Completion cm;
            cm.ret.actual_length = static_cast<std::uint32_t>(w->size());
            cm.payload = std::move(*w);
            complete(std::move(cm));
        } else {
            std::lock_guard lk(mtx_);
            heldInterrupts_.push_back({cmd.basic.seqnum, std::move(complete)});
        }
        return;
    }
    if (epAddr == 0x06) { // 扬声器 iso OUT：立即确认 + 数据交引擎
        ++cIso06_;
        engine_.on_iso_out(payload, isoDescs);
        Completion cm;
        cm.ret.status = 0;
        cm.ret.actual_length = cmd.submit.transfer_buffer_length;
        if (!isoDescs.empty()) {
            cm.ret.number_of_packets = static_cast<std::uint32_t>(isoDescs.size());
            cm.iso.assign(isoDescs.begin(), isoDescs.end());
            std::uint32_t sum = 0;
            for (auto&& p : cm.iso) sum += p.actual_length;
            cm.ret.actual_length = sum;
        }
        complete(std::move(cm));
        return;
    }
    // 未知端点：STALL
    ++cUnk_;
    Completion cm;
    cm.ret.status = -32;
    complete(std::move(cm));
}

void DeviceModel::on_unlink(std::uint32_t /*seqnum*/, std::uint32_t unlink_seqnum,
                            UnlinkResultFn done) {
    ++cUnlink_;
    { // 先取消挂起的中断提交（AC Status Word 0x81 与 HID 0x82）
        std::lock_guard lk(mtx_);
        for (auto it = heldInterrupts_.begin(); it != heldInterrupts_.end(); ++it)
            if (it->seqnum == unlink_seqnum) {
                heldInterrupts_.erase(it);
                done(-104);
                return;
            }
        for (auto it = heldHid_.begin(); it != heldHid_.end(); ++it)
            if (it->seqnum == unlink_seqnum) {
                heldHid_.erase(it);
                done(-104);
                return;
            }
    }
    if (engine_.cancel(unlink_seqnum)) done(-104); // -ECONNRESET
    else done(0); // 已完成
}

void DeviceModel::inject_volume_step(int steps) {
    std::int16_t cur = 0;
    {
        std::lock_guard lk(mtx_);
        const int res = snap_.ac.volRes == 0 ? 256 : snap_.ac.volRes;
        // 在 int 域内累加并夹紧，再窄化——volMin/volMax 本身是 int16，结果必在范围内
        int v = snap_.ac.volCur[0] + res * steps;
        if (v < snap_.ac.volMin) v = snap_.ac.volMin;
        if (v > snap_.ac.volMax) v = snap_.ac.volMax;
        cur = static_cast<std::int16_t>(v);
        snap_.ac.volCur[0] = snap_.ac.volCur[1] = cur;
    }
    injector_.volume_changed(snap_.featureUnitIds.empty() ? 2 : snap_.featureUnitIds[0]);
    flush_held_interrupts();
    // 日志里的 volCur 取自锁内快照：EP0 线程可能同时 SET_CUR，锁外读 snap_.ac 是数据竞争
    log_.append(std::string("{\"event\":\"inject_volume\",\"steps\":") + std::to_string(steps) +
                ",\"volCur\":" + std::to_string(cur) + "}");
}

void DeviceModel::inject_mute_toggle() {
    bool muted = false;
    {
        std::lock_guard lk(mtx_);
        snap_.ac.mute[0] = !snap_.ac.mute[0];
        snap_.ac.mute[1] = snap_.ac.mute[0];
        muted = snap_.ac.mute[0];
    }
    injector_.mute_changed(snap_.featureUnitIds.empty() ? 2 : snap_.featureUnitIds[0]);
    flush_held_interrupts();
    log_.append(std::string("{\"event\":\"inject_mute\",\"mute\":") + std::to_string(muted ? 1 : 0) +
                "}");
}

void DeviceModel::inject_hid_keys(std::uint8_t keys) {
    {
        std::lock_guard lk(mtx_);
        hidQueue_.push_back(0x01);
        hidQueue_.push_back(keys);
    }
    flush_held_hid();
    log_.append(std::string("{\"event\":\"inject_hid\",\"keys\":") + std::to_string(keys) + "}");
}

void DeviceModel::flush_held_hid() {
    std::vector<HeldHid> toFire;
    {
        std::lock_guard lk(mtx_);
        toFire = std::move(heldHid_);
        heldHid_.clear();
    }
    for (auto& h : toFire) {
        std::vector<std::uint8_t> rep;
        {
            std::lock_guard lk(mtx_);
            if (hidQueue_.size() >= 2) {
                rep = {hidQueue_.front(), hidQueue_[1]};
                hidQueue_.erase(hidQueue_.begin(), hidQueue_.begin() + 2);
            }
        }
        if (rep.empty()) { // 无报告：重新挂起
            std::lock_guard lk(mtx_);
            heldHid_.push_back(std::move(h));
            continue;
        }
        Completion cm;
        cm.ret.actual_length = 2;
        cm.payload = std::move(rep);
        h.complete(std::move(cm));
    }
}

void DeviceModel::flush_held_interrupts() {
    std::vector<HeldInt> toFire;
    {
        std::lock_guard lk(mtx_);
        toFire = std::move(heldInterrupts_);
        heldInterrupts_.clear();
    }
    for (auto& h : toFire) {
        auto w = injector_.poll_interrupt_in();
        if (!w) { // 通知已耗尽：重新挂起
            std::lock_guard lk(mtx_);
            heldInterrupts_.push_back(std::move(h));
            continue;
        }
        Completion cm;
        cm.ret.actual_length = static_cast<std::uint32_t>(w->size());
        cm.payload = std::move(*w);
        h.complete(std::move(cm));
    }
}

std::uint8_t DeviceModel::interface_alt(std::uint8_t iface) const {
    std::lock_guard lk(mtx_);
    auto it = ifaceAlt_.find(iface);
    return it == ifaceAlt_.end() ? 0 : it->second;
}

AcState DeviceModel::ac_state() const { return snap_.ac; }

} // namespace uac
