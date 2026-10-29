// 事件注入器测试：黄金字节 + DeviceModel 挂起/唤醒语义
#include <doctest/doctest.h>
#include <uac/device_model.h>

using namespace uac;

TEST_CASE("uac2 status word golden bytes") {
    EventInjector inj; // 默认 6B UAC2
    inj.volume_changed(2 /*FU id*/, 0);
    auto w = inj.poll_interrupt_in();
    REQUIRE(w.has_value());
    REQUIRE(w->size() == 6);
    // ADC-2 6.1.2：{0x00, entity=2, notify=1, wValue=0x0001, 0x00}
    CHECK((*w)[0] == 0x00);
    CHECK((*w)[1] == 0x02);
    CHECK((*w)[2] == 0x01); // CUR_CHANGED
    CHECK((*w)[3] == 0x01); // wValue lo（CN=1）
    CHECK((*w)[4] == 0x00);
    CHECK((*w)[5] == 0x00);
    CHECK_FALSE(inj.poll_interrupt_in().has_value()); // 队列空
}

TEST_CASE("uac1 mode emits 2-byte status") {
    EventInjector inj;
    inj.set_uac1_mode();
    inj.mute_changed(5, 0);
    auto w = inj.poll_interrupt_in();
    REQUIRE(w.has_value());
    REQUIRE(w->size() == 2);
    CHECK((*w)[0] == 0x00);
    CHECK((*w)[1] == 0x05);
}

TEST_CASE("fifo order preserved") {
    EventInjector inj;
    inj.volume_changed(2);
    inj.mute_changed(2);
    auto a = inj.poll_interrupt_in();
    auto b = inj.poll_interrupt_in();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK((*a)[2] == 0x01);
    CHECK(inj.pending() == false);
    inj.clear();
}

TEST_CASE("device model: interrupt held until injection, then golden payload") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac2_default_snapshot(), std::move(clock));

    // 空队列：中断 IN 应挂起（无立即完成）
    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 77;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 1; // 0x81
    c.submit.transfer_buffer_length = 6;
    c.submit.interval = 4;
    c.submit.number_of_packets = 0;
    int got = 0;
    std::vector<unsigned char> payload;
    m.processor(); // no-op；确保可访问
    // 直接调用 on_urb（不经传输层）
    m.on_urb(c, {}, {}, [&](Completion&& cm) {
        payload = std::move(cm.payload);
        ++got;
    });
    CHECK(got == 0); // 挂起

    // 注入音量-（默认 0dB 已是最大值，先降再验）→ 挂起项被唤醒，携带 6B Status Word
    m.inject_volume_step(-1);
    REQUIRE(got == 1);
    REQUIRE(payload.size() == 6);
    CHECK(payload[1] == uac2::kFuSpeaker);
    CHECK(payload[2] == 0x01);
    // 音量状态已 +1dB（volRes=256 → +256）
    CHECK(m.ac_state().volCur[0] == -256); // -1dB

    // 再次提交中断 IN：队列空 → 再次挂起
    int got2 = 0;
    c.basic.seqnum = 78;
    m.on_urb(c, {}, {}, [&](Completion&&) { ++got2; });
    CHECK(got2 == 0);
    // 静音注入 → 唤醒
    m.inject_mute_toggle();
    CHECK(got2 == 1);
    CHECK(m.ac_state().mute[0] == true);
}

TEST_CASE("volume step clamps at range bounds") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac2_default_snapshot(), std::move(clock));
    for (int i = 0; i != 40; ++i) m.inject_volume_step(1);
    CHECK(m.ac_state().volCur[0] == 0); // 顶到 maxDb=0
    for (int i = 0; i != 80; ++i) m.inject_volume_step(-1);
    CHECK(m.ac_state().volCur[0] == -6144); // 底到 minDb
}
