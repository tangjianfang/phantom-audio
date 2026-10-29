// USB/IP 协议编解码测试 —— 黄金字节来自内核文档 usbip_protocol.rst 的真实抓包示例
#include <doctest/doctest.h>
#include <uac/usbip_proto.h>
#include <uac/endian.h>

using namespace uac::usbip;
using u8v = std::vector<u8>;

namespace {
// 内核文档 EXAMPLE: CmdIntrIN（48B 真实线上字节）
constexpr u8 GOLDEN_CMD_INTR_IN[48] = {
    0x00,0x00,0x00,0x01, 0x00,0x00,0x0d,0x05, 0x00,0x01,0x00,0x0f, 0x00,0x00,0x00,0x01,
    0x00,0x00,0x00,0x01, 0x00,0x00,0x02,0x00, 0x00,0x00,0x00,0x40, 0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x04, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
};
// 内核文档 EXAMPLE: RetIntrIn 头 48B
constexpr u8 GOLDEN_RET_INTR_IN[48] = {
    0x00,0x00,0x00,0x03, 0x00,0x00,0x0d,0x05, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x40, 0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
};

UsbIpDevice make_cmedia() {
    UsbIpDevice d;
    d.path = "/sys/devices/pci0000:00/0000:00:1d.1/usb3/3-2";
    d.busid = "3-2";
    d.busnum = 1; d.devnum = 15;
    d.speed = 3; // High-Speed —— UDE 适配要求
    d.idVendor = 0x0D8C; d.idProduct = 0x0103; d.bcdDevice = 0x0010;
    d.bDeviceClass = 0; d.bDeviceSubClass = 0; d.bDeviceProtocol = 0;
    d.bConfigurationValue = 1; d.bNumConfigurations = 1; d.bNumInterfaces = 2;
    d.interfaces = {{0x01,0x01,0x00},{0x01,0x02,0x00}};
    return d;
}
} // namespace

TEST_CASE("wire sizes") {
    CHECK(sizeof(BasicHeader) == 20);
    CHECK(sizeof(CmdSubmit) == 28);
    CHECK(sizeof(RetSubmit) == 28);
    CHECK(kUrbHeaderSize == 48);
}

TEST_CASE("decode golden CmdIntrIN from kernel doc") {
    auto cmd = decode_urb_header(GOLDEN_CMD_INTR_IN);
    REQUIRE(cmd.has_value());
    CHECK(cmd->is_submit);
    CHECK(cmd->basic.command == CMD_SUBMIT);
    CHECK(cmd->basic.seqnum == 0x0d05);
    CHECK(cmd->basic.devid == 0x0001000f); // (busnum 1 <<16)|devnum 15
    CHECK(cmd->basic.direction == DIR_IN);
    CHECK(cmd->basic.ep == 1);
    CHECK(cmd->submit.transfer_flags == 0x0200);
    CHECK(cmd->submit.transfer_buffer_length == 0x40);
    // 真实抓包：非 ISO 时 start_frame=0xffffffff、number_of_packets=0
    //（文档表格文字写反了两者，以示例抓包为准，两形态都须兼容）
    CHECK(cmd->submit.start_frame == -1);
    CHECK(cmd->submit.number_of_packets == 0);
    CHECK(cmd->submit.interval == 4);
    CHECK_FALSE(is_iso(cmd->submit.number_of_packets));
    CHECK_FALSE(is_iso(kNoPackets));
    CHECK(is_iso(10));
}

TEST_CASE("decode golden RetIntrIn as submit-frame is rejected, unlink ok") {
    // RetIntrIn 命令码是 RET_SUBMIT，server 侧 decode 应拒绝
    auto cmd = decode_urb_header(GOLDEN_RET_INTR_IN);
    CHECK_FALSE(cmd.has_value());
}

TEST_CASE("encode_cmd_submit roundtrips to golden bytes") {
    BasicHeader b{CMD_SUBMIT, 0x0d05, 0x0001000f, DIR_IN, 1};
    CmdSubmit c{0x0200, 0x40, -1, 0, 4, {0,0,0,0,0,0,0,0}};
    auto wire = encode_cmd_submit(b, c);
    REQUIRE(wire.size() == 48);
    CHECK(std::equal(wire.begin(), wire.end(), std::begin(GOLDEN_CMD_INTR_IN)));
}

TEST_CASE("encode_ret_submit zeroes server-side fields and pads") {
    BasicHeader b{RET_SUBMIT, 0x0d05, 0, 0, 0};
    // 黄金抓包：actual_length 回显 0x40，start_frame=ffffffff，npackets=0
    RetSubmit r{0, 0x40, -1, 0, 0, {0,0,0,0,0,0,0,0}};
    auto wire = encode_ret_submit(b, r);
    REQUIRE(wire.size() == 48);
    CHECK(std::equal(wire.begin(), wire.end(), std::begin(GOLDEN_RET_INTR_IN)));
}

TEST_CASE("devlist reply golden layout") {
    auto v = encode_devlist_reply({make_cmedia()});
    REQUIRE(v.size() == 12 + kUsbIpDeviceSize + 2 * 4); // 332
    // 版本 0x0111 / code 0x0005 / status 0 / ndev 1（全大端）
    CHECK(v[0] == 0x01); CHECK(v[1] == 0x11);
    CHECK(v[2] == 0x00); CHECK(v[3] == 0x05);
    CHECK(uac::rd_be<u32>(&v[4]) == 0);
    CHECK(uac::rd_be<u32>(&v[8]) == 1);
    // path @0x0C，busid @0x10C == "3-2"
    CHECK(v[0x10C] == '3'); CHECK(v[0x10D] == '-'); CHECK(v[0x10E] == '2'); CHECK(v[0x10F] == 0);
    // busnum @0x12C = 1, speed @0x134 = 3(HS), idVendor @0x138 = 0x0D8C
    CHECK(uac::rd_be<u32>(&v[0x12C]) == 1);
    CHECK(uac::rd_be<u32>(&v[0x134]) == 3);
    CHECK(uac::rd_be<u16>(&v[0x138]) == 0x0D8C);
    CHECK(uac::rd_be<u16>(&v[0x13A]) == 0x0103);
    // bNumInterfaces @0x143 = 2；其后 2 条 4B 接口记录（class 0x01）
    CHECK(v[0x143] == 2);
    CHECK(v[0x144] == 0x01); CHECK(v[0x145] == 0x01); CHECK(v[0x146] == 0x00); CHECK(v[0x147] == 0x00);
    CHECK(v[0x148] == 0x01); CHECK(v[0x149] == 0x02);
}

TEST_CASE("empty devlist ends after count field") {
    auto v = encode_devlist_reply({});
    REQUIRE(v.size() == 12);
    CHECK(uac::rd_be<u32>(&v[8]) == 0);
}

TEST_CASE("import busid decode strips zeros") {
    u8 body[32]{};
    body[0]='3'; body[1]='-'; body[2]='2';
    CHECK(decode_import_busid(body) == "3-2");
}

TEST_CASE("import reply layout ok / error case truncated") {
    auto ok = encode_import_reply(make_cmedia(), 0);
    REQUIRE(ok.size() == 8 + kUsbIpDeviceSize); // 320：ver2+code2+status4+device312（无 ndev 字段）
    CHECK(ok[2] == 0x00); CHECK(ok[3] == 0x03); // OP_REP_IMPORT
    CHECK(uac::rd_be<u32>(&ok[4]) == 0);
    CHECK(ok[8 + 256] == '3');                   // busid @0x108
    auto err = encode_import_reply(make_cmedia(), 1);
    CHECK(err.size() == 8);                      // status!=0：止于 status 字段
    CHECK(uac::rd_be<u32>(&err[4]) == 1);
}

TEST_CASE("iso descriptor array roundtrip") {
    std::vector<IsoPacket> in{{0,192,192,0},{192,192,192,-75}};
    auto wire = encode_iso_descs(in);
    REQUIRE(wire.size() == 2 * kIsoDescSize);
    CHECK(uac::rd_be<u32>(&wire[0]) == 0);
    CHECK(uac::rd_be<u32>(&wire[4]) == 192);
    CHECK(uac::rd_be<u32>(&wire[16+12]) == static_cast<u32>(-75)); // status 大端按位模式
    auto out = decode_iso_descs(wire);
    REQUIRE(out.size() == 2);
    CHECK(out[1].offset == 192);
    CHECK(out[1].status == -75);
}

TEST_CASE("unlink decode") {
    std::vector<u8> w(48, 0);
    w[3] = 0x02;                       // CMD_UNLINK
    w[7] = 0x77;                       // seqnum = 0x77
    w[22] = 0x22; w[23] = 0x33;        // unlink_seqnum @0x14 = 0x00002233（大端）
    auto c = decode_urb_header(w);
    REQUIRE(c.has_value());
    CHECK_FALSE(c->is_submit);
    CHECK(c->basic.seqnum == 0x77);
    CHECK(c->unlink_seqnum == 0x2233);
}

TEST_CASE("short buffer / garbage rejected") {
    u8v short_(10, 0);
    CHECK_FALSE(decode_urb_header(short_).has_value());
    u8v bad(48, 0); bad[3] = 0x09;     // 未知命令
    CHECK_FALSE(decode_urb_header(bad).has_value());
}

TEST_CASE("payload length rule") {
    BasicHeader b{CMD_SUBMIT, 1, 0x1000f, DIR_OUT, 1};
    CmdSubmit c{0, 1920, 0, 10, 1, {}};
    UrbCommand uc{b, c, 0, true};
    CHECK(payload_len_after_header(uc) == 1920); // OUT: 全量负载
    uc.basic.direction = DIR_IN;
    CHECK(payload_len_after_header(uc) == 0);    // IN: 负载在 RET 里
}
