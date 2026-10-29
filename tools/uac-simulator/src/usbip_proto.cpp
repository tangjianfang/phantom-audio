// USB/IP v1.1.1 协议编解码实现（全大端，依据内核 usbip_protocol.rst）
#include <uac/usbip_proto.h>
#include <uac/endian.h>

namespace uac::usbip {

namespace {
void push_fixed_string(std::vector<u8>& out, const std::string& s, size_t width) {
    const size_t at = out.size();
    out.resize(at + width, 0);
    for (size_t i = 0; i < s.size() && i < width - 1; ++i) out[at + i] = static_cast<u8>(s[i]);
}

void push_usbip_device(std::vector<u8>& out, const UsbIpDevice& d, bool with_interfaces) {
    push_fixed_string(out, d.path, 256);
    push_fixed_string(out, d.busid, 32);
    push_be(out, d.busnum);
    push_be(out, d.devnum);
    push_be(out, d.speed);
    push_be(out, d.idVendor);
    push_be(out, d.idProduct);
    push_be(out, d.bcdDevice);
    const u8 tail[6] = {d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol,
                        d.bConfigurationValue, d.bNumConfigurations, d.bNumInterfaces};
    out.insert(out.end(), tail, tail + 6);
    if (with_interfaces) {
        for (auto&& ifc : d.interfaces) {
            const u8 rec[4] = {ifc.bInterfaceClass, ifc.bInterfaceSubClass, ifc.bInterfaceProtocol, 0};
            out.insert(out.end(), rec, rec + 4);
        }
    }
}

void push_common(std::vector<u8>& out, u16 code, u32 status) {
    const u8 ver[2] = {static_cast<u8>(kVersion >> 8), static_cast<u8>(kVersion & 0xff)};
    out.insert(out.end(), ver, ver + 2);
    const u8 cd[2] = {static_cast<u8>(code >> 8), static_cast<u8>(code & 0xff)};
    out.insert(out.end(), cd, cd + 2);
    push_be(out, status);
}
} // namespace

std::vector<u8> encode_devlist_reply(const std::vector<UsbIpDevice>& devs) {
    std::vector<u8> out;
    out.reserve(12 + devs.size() * (kUsbIpDeviceSize + 4));
    push_common(out, OP_REP_DEVLIST, 0);
    push_be<u32>(out, static_cast<u32>(devs.size()));
    for (auto&& d : devs) {
        // 设备清单里的 bNumInterfaces 以 interfaces 实际数量为准
        UsbIpDevice copy = d;
        copy.bNumInterfaces = static_cast<u8>(copy.interfaces.size());
        push_usbip_device(out, copy, /*with_interfaces=*/true);
    }
    return out;
}

std::string decode_import_busid(std::span<const u8> body32) {
    std::string s;
    for (auto b : body32.subspan(0, body32.size() < 32 ? body32.size() : 32)) {
        if (b == 0) break;
        s.push_back(static_cast<char>(b));
    }
    return s;
}

std::vector<u8> encode_import_reply(const UsbIpDevice& dev, u32 status) {
    std::vector<u8> out;
    push_common(out, OP_REP_IMPORT, status);
    if (status == 0) {
        UsbIpDevice copy = dev;
        copy.bNumInterfaces = static_cast<u8>(copy.interfaces.size());
        push_usbip_device(out, copy, /*with_interfaces=*/false); // 文档：IMPORT 回复不含接口记录
    }
    return out;
}

std::optional<UrbCommand> decode_urb_header(std::span<const u8> buf) {
    if (buf.size() < kUrbHeaderSize) return std::nullopt;
    UrbCommand c{};
    c.basic.command = rd_be<u32>(&buf[0]);
    c.basic.seqnum = rd_be<u32>(&buf[4]);
    c.basic.devid = rd_be<u32>(&buf[8]);
    c.basic.direction = rd_be<u32>(&buf[12]);
    c.basic.ep = rd_be<u32>(&buf[16]);
    if (c.basic.command == CMD_SUBMIT) {
        c.is_submit = true;
        c.submit.transfer_flags = rd_be<u32>(&buf[20]);
        c.submit.transfer_buffer_length = rd_be<u32>(&buf[24]);
        c.submit.start_frame = rd_be<i32>(&buf[28]);
        c.submit.number_of_packets = rd_be<u32>(&buf[32]);
        c.submit.interval = rd_be<i32>(&buf[36]);
        std::memcpy(c.submit.setup, &buf[40], 8);
    } else if (c.basic.command == CMD_UNLINK) {
        c.is_submit = false;
        c.unlink_seqnum = rd_be<u32>(&buf[20]);
    } else {
        return std::nullopt; // server 只接受 CMD_SUBMIT / CMD_UNLINK
    }
    return c;
}

namespace {
std::vector<u8> encode_urb(const BasicHeader& basic, const void* union28) {
    std::vector<u8> out(kUrbHeaderSize, 0);
    wr_be<u32>(out.data() + 0, basic.command);
    wr_be<u32>(out.data() + 4, basic.seqnum);
    wr_be<u32>(out.data() + 8, basic.devid); // server RET 恒 0，由调用方保证
    wr_be<u32>(out.data() + 12, basic.direction);
    wr_be<u32>(out.data() + 16, basic.ep);
    // union 28B 逐字段大端写入由特化函数完成，这里覆写 20..47 字节
    std::memcpy(out.data() + 20, union28, 28);
    return out;
}
} // namespace

std::vector<u8> encode_cmd_submit(const BasicHeader& b, const CmdSubmit& c) {
    std::vector<u8> u(28, 0);
    wr_be<u32>(&u[0], c.transfer_flags);
    wr_be<u32>(&u[4], c.transfer_buffer_length);
    wr_be<i32>(&u[8], c.start_frame);
    wr_be<u32>(&u[12], c.number_of_packets);
    wr_be<i32>(&u[16], c.interval);
    std::memcpy(&u[20], c.setup, 8);
    return encode_urb(b, u.data());
}

std::vector<u8> encode_ret_submit(const BasicHeader& req, const RetSubmit& r) {
    std::vector<u8> u(28, 0);
    wr_be<i32>(&u[0], r.status);
    wr_be<u32>(&u[4], r.actual_length);
    wr_be<i32>(&u[8], r.start_frame);
    wr_be<u32>(&u[12], r.number_of_packets);
    wr_be<u32>(&u[16], r.error_count);
    // u[20..27] padding 恒 0
    BasicHeader b{RET_SUBMIT, req.seqnum, 0, 0, 0};
    return encode_urb(b, u.data());
}

std::vector<u8> encode_ret_unlink(const BasicHeader& req, i32 status) {
    std::vector<u8> u(28, 0);
    wr_be<i32>(&u[0], status);
    BasicHeader b{RET_UNLINK, req.seqnum, 0, 0, 0};
    return encode_urb(b, u.data());
}

std::vector<u8> encode_iso_descs(const std::vector<IsoPacket>& v) {
    std::vector<u8> out;
    out.reserve(v.size() * kIsoDescSize);
    for (auto&& p : v) {
        push_be(out, p.offset);
        push_be(out, p.length);
        push_be(out, p.actual_length);
        push_be<i32>(out, p.status);
    }
    return out;
}

std::vector<IsoPacket> decode_iso_descs(std::span<const u8> buf) {
    std::vector<IsoPacket> out;
    if (buf.size() % kIsoDescSize != 0) return out;
    out.reserve(buf.size() / kIsoDescSize);
    for (size_t off = 0; off < buf.size(); off += kIsoDescSize) {
        IsoPacket p;
        p.offset = rd_be<u32>(&buf[off]);
        p.length = rd_be<u32>(&buf[off + 4]);
        p.actual_length = rd_be<u32>(&buf[off + 8]);
        p.status = rd_be<i32>(&buf[off + 12]);
        out.push_back(p);
    }
    return out;
}

size_t payload_len_after_header(const UrbCommand& c) {
    if (!c.is_submit) return 0;
    return c.basic.direction == DIR_OUT ? c.submit.transfer_buffer_length : 0;
}

} // namespace uac::usbip
