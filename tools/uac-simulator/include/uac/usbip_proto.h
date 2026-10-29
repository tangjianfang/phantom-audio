#pragma once
// USB/IP v1.1.1 协议层 —— 线上格式编解码（server 侧视角）
// 权威依据：内核 Documentation/usb/usbip_protocol.rst（全字段大端）
//   - URB 头固定 48 字节（basic 20B + submit/unlink/ret 联合 28B）
//   - OP_REQ_DEVLIST=0x8005 / OP_REP_DEVLIST=0x0005
//     OP_REQ_IMPORT=0x8003 / OP_REP_IMPORT=0x0003
//     CMD_SUBMIT=0x00000001 / CMD_UNLINK=0x00000002
//     RET_SUBMIT=0x00000003 / RET_UNLINK=0x00000004
//   - 非 ISO 传输 number_of_packets = 0xffffffff
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uac::usbip {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;

// ---- 操作码 ----
constexpr u16 kVersion = 0x0111;
constexpr u16 OP_REQ_DEVLIST = 0x8005;
constexpr u16 OP_REP_DEVLIST = 0x0005;
constexpr u16 OP_REQ_IMPORT = 0x8003;
constexpr u16 OP_REP_IMPORT = 0x0003;
constexpr u32 CMD_SUBMIT = 0x00000001;
constexpr u32 CMD_UNLINK = 0x00000002;
constexpr u32 RET_SUBMIT = 0x00000003;
constexpr u32 RET_UNLINK = 0x00000004;

// URB 方向（basic.direction）
constexpr u32 DIR_OUT = 0;
constexpr u32 DIR_IN = 1;

// 非 ISO 的 number_of_packets 哨兵（文档文字声称 0xffffffff；内核示例抓包实为 0）
constexpr u32 kNoPackets = 0xffffffff;
// 两种线上形态都视为非 ISO
constexpr bool is_iso(u32 number_of_packets) { return number_of_packets != 0 && number_of_packets != kNoPackets; }

constexpr size_t kUrbHeaderSize = 0x30; // 48
constexpr size_t kBasicSize = 20;
constexpr size_t kIsoDescSize = 16; // 每个 iso_packet_descriptor

// ---- 结构体（内存表示；线上编解码见函数族） ----
#pragma pack(push, 1)
struct BasicHeader {
    u32 command;
    u32 seqnum;
    u32 devid;     // client: (busnum<<16)|devnum; server RET 恒 0
    u32 direction; // 0=OUT 1=IN（仅 client 有意义）
    u32 ep;        // 端点号（仅 client 有意义；UNLINK 恒 0）
};
struct CmdSubmit {
    u32 transfer_flags;
    u32 transfer_buffer_length;
    i32 start_frame;
    u32 number_of_packets; // 0xffffffff = 非 ISO
    i32 interval;
    u8 setup[8]; // USB setup 数据包（控制传输）
};
struct RetSubmit {
    i32 status;
    u32 actual_length;
    i32 start_frame;
    u32 number_of_packets;
    u32 error_count;
    u8 padding[8]; // 文档：0x28 处 8 字节 padding 恒 0
};
struct CmdUnlink {
    u32 unlink_seqnum;
    u8 padding[24];
};
struct RetUnlink {
    i32 status; // 成功取消 = -ECONNRESET(-104)；已完成后收到 = 0
    u8 padding[24];
};
#pragma pack(pop)

static_assert(sizeof(BasicHeader) == 20);
static_assert(sizeof(CmdSubmit) == 28);
static_assert(sizeof(RetSubmit) == 28);
static_assert(sizeof(CmdUnlink) == 28);
static_assert(sizeof(RetUnlink) == 28);

struct IsoPacket {          // iso_packet_descriptor（大端，16B）
    u32 offset = 0;
    u32 length = 0;
    u32 actual_length = 0;
    i32 status = 0;
};

// ---- 设备清单（usbip_usb_device，312B/台 + 每接口 4B） ----
struct UsbIpInterface {
    u8 bInterfaceClass = 0, bInterfaceSubClass = 0, bInterfaceProtocol = 0;
};
struct UsbIpDevice {
    std::string path;   // 如 "/sys/devices/.../usb3/3-2"（仿真器自造）
    std::string busid;  // 如 "3-2"（IMPORT 请求按它匹配）
    u32 busnum = 1, devnum = 2;
    u32 speed = 3; // 1=1.5M 2=12M(FS) 3=480M(HS) 4=5G 5=10G 6=20G —— UDE 需报 HS(3)
    u16 idVendor = 0, idProduct = 0, bcdDevice = 0;
    u8 bDeviceClass = 0, bDeviceSubClass = 0, bDeviceProtocol = 0;
    u8 bConfigurationValue = 1, bNumConfigurations = 1, bNumInterfaces = 1;
    std::vector<UsbIpInterface> interfaces;
};

constexpr size_t kUsbIpDeviceSize = 312; // 256+32+4*3+2*3+1*6

// ---- 控制面编解码 ----

// OP_REP_DEVLIST 完整回复（含 12B 公共头）
std::vector<u8> encode_devlist_reply(const std::vector<UsbIpDevice>& devs);

// OP_REQ_IMPORT 载荷解码：busid（去零截断）；输入为 8 字节公共头之后的 32B busid
std::string decode_import_busid(std::span<const u8> body32);

// OP_REP_IMPORT：status=0 附带设备结构；status!=0 仅 12B
std::vector<u8> encode_import_reply(const UsbIpDevice& dev, u32 status);

// ---- URB 面编解码 ----

// 解析 48B URB 头（CMD_SUBMIT / CMD_UNLINK）。返回 false：长度不足或命令未知。
struct UrbCommand {
    BasicHeader basic;
    CmdSubmit submit;
    u32 unlink_seqnum; // 仅 CMD_UNLINK
    bool is_submit;
};
std::optional<UrbCommand> decode_urb_header(std::span<const u8> buf);

// RET_SUBMIT 头（48B：basic(20) + ret(28)），devid/direction/ep 归零
std::vector<u8> encode_ret_submit(const BasicHeader& req, const RetSubmit& ret);

// RET_UNLINK 头（48B）
std::vector<u8> encode_ret_unlink(const BasicHeader& req, i32 status);

// CMD_SUBMIT 头（测试/回环客户端用）
std::vector<u8> encode_cmd_submit(const BasicHeader& b, const CmdSubmit& c);

// iso 描述符数组编解码（RET_SUBMIT 尾部 / CMD_SUBMIT(IN 时无,OUT 时附于 payload 后)）
std::vector<u8> encode_iso_descs(const std::vector<IsoPacket>& v);
std::vector<IsoPacket> decode_iso_descs(std::span<const u8> buf);

// URB 头之后的负载长度：OUT 方向 = transfer_buffer_length；IN 方向 = 0；
// ISO OUT 的 iso 描述符数组跟随 payload 之后（由调用方按 number_of_packets 续读）。
size_t payload_len_after_header(const UrbCommand& c);

} // namespace uac::usbip
