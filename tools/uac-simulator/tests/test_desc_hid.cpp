// HID 复合接口测试
#include <doctest/doctest.h>
#include <uac/desc_hid.h>
#include <uac/desc_uac1.h>
#include <uac/ude_adapter.h>
#include <uac/device_model.h>
#include <algorithm>

using namespace uac;
using uac::usbip::CmdSubmit;

TEST_CASE("report descriptor golden: consumer page + 3 keys + report id 1") {
    auto r = hid::build_report_desc();
    CHECK(r.size() == 31);
    CHECK(r[0] == 0x05); CHECK(r[1] == 0x0C);  // Usage Page Consumer
    CHECK(r[6] == 0x85); CHECK(r[7] == 0x01);  // Report ID 1
    // 三个 Usage：E9(vol+)/EA(vol-)/E2(mute)
    CHECK(r[16] == 0x09); CHECK(r[17] == 0xE9);
    CHECK(r[18] == 0x09); CHECK(r[19] == 0xEA);
    CHECK(r[20] == 0x09); CHECK(r[21] == 0xE2);
    CHECK(r.back() == 0xC0);                   // End Collection
    // 类描述符长度字段 = 26
    auto d = hid::build_hid_descs();
    REQUIRE(d.hid_class_desc.size() == 9);
    CHECK(d.hid_class_desc[1] == 0x21);
    CHECK(d.hid_class_desc[6] == 0x22);
    CHECK(d.hid_class_desc[7] == 31);          // wDescriptorLength lo
    CHECK(d.hid_class_desc[8] == 0);
}

TEST_CASE("append hid interface keeps uac1 config parseable") {
    uac1::Uac1Profile p;
    auto cfg = uac1::build_config_desc(p);
    const auto before = cfg.size();
    auto combined = hid::append_hid_interface(cfg, 3);
    CHECK(combined.size() == before + 9 + 9 + 7); // 报告描述符不嵌入（GET_DESCRIPTOR 获取）
    auto v = desc::parse_config(combined);
    REQUIRE(v.has_value());
    CHECK(v->bNumInterfaces == 4);
    CHECK(v->interfaces.size() == 6); // 原 5 + HID alt0
    CHECK(v->interfaces.back().cls == 0x03); // HID 类
    CHECK(v->endpoints.size() == 3);
    bool hidEp = false;
    for (auto&& e : v->endpoints)
        if (e.address == 0x82) {
            hidEp = true;
            CHECK((e.attributes & 0x03) == 0x03);
            CHECK(e.maxPacketSize == 2);
        }
    CHECK(hidEp);
    CHECK(ude::lint_common(*v).empty()); // 音频规则不破坏
}

TEST_CASE("hid report encoding") {
    auto up = hid::encode_report(hid::kVolUp);
    CHECK(up == std::vector<std::uint8_t>{0x01, 0x01});
    auto both = hid::encode_report(hid::kVolUp | hid::kMute);
    CHECK(both == std::vector<std::uint8_t>{0x01, 0x05});
    auto none = hid::encode_report(0);
    CHECK(none == std::vector<std::uint8_t>{0x01, 0x00});
}

TEST_CASE("ude adapter bumps hid interrupt interval too") {
    uac1::Uac1Profile p;
    auto cfg = hid::append_hid_interface(uac1::build_config_desc(p), 3);
    const int changed = ude::adapt_config_for_ude(cfg);
    CHECK(changed == 3); // 原 2 个 iso EP(interval=1→4) + HID int EP(1→4)
    auto v = desc::parse_config(cfg);
    for (auto&& e : v->endpoints) CHECK(e.interval >= 4);
}

TEST_CASE("device model: hid key press/release via interrupt endpoint") {
    auto clock = std::make_unique<ManualClock>();
    DeviceModel m(make_uac1_hid_snapshot(), std::move(clock));
    REQUIRE(m.has_hid());

    usbip::UrbCommand c{};
    c.is_submit = true;
    c.basic.command = usbip::CMD_SUBMIT;
    c.basic.seqnum = 90;
    c.basic.direction = usbip::DIR_IN;
    c.basic.ep = 2; // 0x82
    c.submit.transfer_buffer_length = 2;

    int got = 0;
    std::vector<std::uint8_t> last;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { last = std::move(cm.payload); ++got; });
    CHECK(got == 0); // 空队列挂起

    m.inject_hid_keys(hid::kVolUp);
    REQUIRE(got == 1);
    CHECK(last == std::vector<std::uint8_t>{0x01, 0x01});

    c.basic.seqnum = 91;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { last = std::move(cm.payload); ++got; });
    CHECK(got == 1); // 释放报告未注入 → 挂起
    m.inject_hid_keys(0); // 释放
    REQUIRE(got == 2);
    CHECK(last == std::vector<std::uint8_t>{0x01, 0x00});

    // 报告描述符经 ep0 GET_DESCRIPTOR(0x22)
    c.basic.seqnum = 92;
    c.basic.ep = 0;
    c.basic.direction = usbip::DIR_IN;
    CmdSubmit cs{0, 31, -1, 0, 0, {}};
    const std::uint8_t setup[8] = {0x81, 0x06, 0x00, 0x22, 0x03, 0x00, 31, 0};
    std::copy(setup, setup + 8, cs.setup);
    c.submit = cs;
    std::vector<std::uint8_t> report;
    m.on_urb(c, {}, {}, [&](Completion&& cm) { report = std::move(cm.payload); });
    REQUIRE(report.size() == 31);
    CHECK(report[1] == 0x0C);
}
