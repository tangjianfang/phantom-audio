#pragma once
// 设备模型：把 USB/IP 传输层的 URB 流映射到 AC 处理器 / AS 引擎 /（M2）事件注入器。
// 维护：接口 alt 状态、端点→引擎路由、UNLINK 取消。
#include <uac/ac_processor.h>
#include <uac/as_engine.h>
#include <uac/desc_uac1.h>
#include <uac/desc_uac2.h>
#include <uac/desc_hid.h>
#include <uac/urb_dispatcher.h>
#include <uac/event_injector.h>
#include <uac/fault_injector.h>
#include <functional>
#include <thread>
#include <uac/usbip_proto.h>
#include <atomic>
#include <map>

namespace uac {

// 一次成型的设备快照（Profile 加载或内置 preset 产出）
struct DeviceSnapshot {
    Protocol protocol = Protocol::Uac1;
    desc::DeviceDesc dev;
    std::vector<std::uint8_t> config;   // 已过 UDE 适配
    std::vector<std::uint8_t> qualifier;
    std::map<std::uint8_t, std::vector<std::uint8_t>> strings;
    AcState ac;
    StreamConfig stream;
    std::vector<std::uint8_t> featureUnitIds;
    std::uint8_t clockId = uac2::kClock; // UAC2 用
    std::vector<std::uint32_t> rates{48000};
    bool hasSpeakerOut = true;  // EP 0x06
    bool hasMicIn = true;       // EP 0x86
    bool hidConsumerVolume = false; // EP 0x82 + 报告描述符
    std::vector<std::uint8_t> hidReportDesc;
    std::string name = "uac1-sim";
};

// 内置 preset：UAC1 C-Media 风格（黄金拓扑）/ UAC2 全双工
DeviceSnapshot make_uac1_default_snapshot();
DeviceSnapshot make_uac2_default_snapshot();
DeviceSnapshot make_uac1_hid_snapshot(); // UAC1 + HID 音量键复合

class DeviceModel final : public UrbDispatcher {
public:
    // clock 为空时使用 WallClock；测试注入 ManualClock 获得确定性 pacing
    explicit DeviceModel(DeviceSnapshot snap,
                         std::unique_ptr<class MicroframeClock> clock = nullptr);
    ~DeviceModel();

    // UrbDispatcher
    void on_import(const usbip::UsbIpDevice& dev) override;
    void on_urb(const usbip::UrbCommand& cmd, std::span<const unsigned char> payload,
                std::span<const usbip::IsoPacket> isoDescs, CompletionFn complete) override;
    void on_unlink(std::uint32_t seqnum, std::uint32_t unlink_seqnum,
                   UnlinkResultFn done) override;
    void on_detach() override;

    // R6 回退通道：HID 音量键（按住-释放语义）
    void inject_hid_keys(std::uint8_t keys); // hid::kVolUp/kVolDown/kMute 位或；0=释放
    bool has_hid() const { return snap_.hidConsumerVolume; }

    // R6 事件注入（先改状态再入队 Status Word）
    void inject_volume_step(int steps); // ±resDb 步进（正=音量+，负=音量-）
    void inject_mute_toggle();
    EventInjector& injector() { return injector_; }
    FaultInjector& faults() { return fault_; }
    void set_disconnect_hook(std::function<void()> h) { disconnectHook_ = std::move(h); }

    usbip::UsbIpDevice usbip_device() const; // 导出给 server 的设备清单
    std::uint8_t interface_alt(std::uint8_t iface) const;
    AcState ac_state() const;
    AsEngine& engine() { return engine_; }
    AcProcessor& processor() { return ac_; }
    LogStore& log() { return log_; }
    MicroframeClock& clock() { return *clock_; }
    bool attached() const { return attached_.load(); }
    void pump() { engine_.pump(); } // ManualClock 测试用

private:
    void handle_ep0(const usbip::UrbCommand& cmd, std::span<const unsigned char> payload,
                     CompletionFn complete);
    mutable std::mutex mtx_;
    DeviceSnapshot snap_;
    LogStore log_;
    std::unique_ptr<MicroframeClock> clock_;
    AcProcessor ac_;
    AsEngine engine_;
    std::map<std::uint8_t, std::uint8_t> ifaceAlt_;
    EventInjector injector_;
    std::uint32_t lastRate_ = 0;              // host-driven rate sync into the engine
    std::vector<std::thread> delayThreads_;   // fault-delay completions (joined in dtor)
    std::mutex delayMtx_;
    FaultInjector fault_;
    std::function<void()> disconnectHook_;
    std::vector<std::uint8_t> hidQueue_; // 待发送报告（每项 2B：id+keys）
    struct HeldHid { std::uint32_t seqnum; CompletionFn complete; };
    std::vector<HeldHid> heldHid_;
    void flush_held_hid();
    struct HeldInt { std::uint32_t seqnum; CompletionFn complete; };
    std::vector<HeldInt> heldInterrupts_;
    void flush_held_interrupts();
    std::atomic<bool> attached_{false};
    std::uint64_t imported_ = 0;
};

} // namespace uac
