// USB/IP server 传输层实现（Winsock2，仅 127.0.0.1）
#include <uac/usbip_server.h>
#include <uac/endian.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace uac {

namespace {

std::once_flag g_wsa_once;
void wsa_init_once() {
    std::call_once(g_wsa_once, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        // 故意不 WSACleanup：进程级生命周期，避免与测试/多实例引用计数纠缠
    });
}

bool send_all(SOCKET s, std::span<const unsigned char> b) {
    size_t off = 0;
    while (off < b.size()) {
        int n = ::send(s, reinterpret_cast<const char*>(b.data() + off),
                       static_cast<int>(b.size() - off), 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// 阻塞读满 n 字节；返回 false = 连接关闭/出错
bool recv_all(SOCKET s, unsigned char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = ::recv(s, reinterpret_cast<char*>(p + off), static_cast<int>(n - off), 0);
        if (r <= 0) return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

} // namespace

struct UsbIpServer::Impl {
    std::atomic<bool> stopping{false};
    std::atomic<bool> attached{false};
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<SOCKET> data_sock{INVALID_SOCKET}; // IMPORT 后的 URB 通道
    std::thread accept_thread;

    UrbDispatcher* disp = nullptr;
    std::vector<usbip::UsbIpDevice> devices;

    std::mutex send_mtx;                 // 串行化 data_sock 上的发送
    std::mutex cancel_mtx;               // 已取消（UNLINK）的 seqnum 集合
    std::set<uint32_t> cancelled;
    std::vector<unsigned char> io_buf;   // 复用缓冲

    bool send_on_data(std::span<const unsigned char> a, std::span<const unsigned char> b = {},
                      std::span<const unsigned char> c = {}) {
        std::lock_guard lk(send_mtx);
        SOCKET s = data_sock.load();
        if (s == INVALID_SOCKET) return false;
        return send_all(s, a) && (b.empty() || send_all(s, b)) && (c.empty() || send_all(s, c));
    }

    CompletionFn wrap_completion(uint32_t seqnum, CompletionFn&& complete) {
        // UNLINK 抢先后静默丢弃 completion（协议要求：成功 RET_UNLINK 后不再有该 RET_SUBMIT）
        return [this, seqnum, cb = std::move(complete)](Completion&& cm) mutable {
            {
                std::lock_guard lk(cancel_mtx);
                if (cancelled.count(seqnum)) return;
            }
            using namespace usbip;
            std::vector<unsigned char> wire;
            wire.resize(kUrbHeaderSize);
            uac::wr_be<uint32_t>(wire.data() + 0, RET_SUBMIT);
            uac::wr_be<uint32_t>(wire.data() + 4, seqnum);
            // ret 段 20..47
            uac::wr_be<int32_t>(wire.data() + 20, cm.ret.status);
            uac::wr_be<uint32_t>(wire.data() + 24, cm.ret.actual_length);
            uac::wr_be<int32_t>(wire.data() + 28, cm.ret.start_frame);
            uac::wr_be<uint32_t>(wire.data() + 32, cm.ret.number_of_packets);
            uac::wr_be<uint32_t>(wire.data() + 36, cm.ret.error_count);
            auto iso = encode_iso_descs(cm.iso);
            send_on_data(wire, cm.payload, iso);
            cb = {}; // 释放
        };
    }

    UnlinkResultFn wrap_unlink(uint32_t seqnum, UnlinkResultFn&& done) {
        return [this, seqnum, cb = std::move(done)](int32_t status) mutable {
            std::vector<unsigned char> wire(usbip::kUrbHeaderSize, 0);
            uac::wr_be<uint32_t>(wire.data() + 0, usbip::RET_UNLINK);
            uac::wr_be<uint32_t>(wire.data() + 4, seqnum);
            uac::wr_be<int32_t>(wire.data() + 20, status);
            send_on_data(wire);
            cb = {};
        };
    }

    // 连接内主循环：返回前关闭套接字
    void serve_connection(SOCKET s) {
        using namespace usbip;
        bool is_data = false;
        for (;;) {
            unsigned char common[8];
            if (!recv_all(s, common, sizeof(common))) break;
            const auto ver = uac::rd_be<uint16_t>(&common[0]);
            const auto code = uac::rd_be<uint16_t>(&common[2]);
            if (ver != kVersion) break;

            if (code == OP_REQ_DEVLIST) {
                uac::rd_be<uint32_t>(&common[4]); // status 未用
                if (!send_all(s, encode_devlist_reply(devices))) break;
                continue; // 等客户端关闭或下一请求
            }
            if (code == OP_REQ_IMPORT) {
                unsigned char body[32]; // busid（status 已含于 8B 公共头）
                if (!recv_all(s, body, sizeof(body))) break;
                const auto busid = decode_import_busid({body, 32});
                const UsbIpDevice* found = nullptr;
                for (auto&& d : devices)
                    if (d.busid == busid) { found = &d; break; }
                if (!found) {
                    send_all(s, encode_import_reply(devices.empty() ? UsbIpDevice{} : devices[0], 1));
                    break; // 错误：关连接
                }
                if (attached.load()) { // 已占用：单设备单连接
                    send_all(s, encode_import_reply(*found, 1));
                    break;
                }
                if (!send_all(s, encode_import_reply(*found, 0))) break;
                data_sock.store(s);
                attached.store(true);
                is_data = true;
                disp->on_import(*found);
                urb_loop(s); // 直至连接关闭
                attached.store(false);
                data_sock.store(INVALID_SOCKET);
                {
                    std::lock_guard lk(cancel_mtx);
                    cancelled.clear();
                }
                disp->on_detach();
                break;
            }
            break; // 未知操作码
        }
        if (s != INVALID_SOCKET) closesocket(s);
        (void)is_data;
    }

    void urb_loop(SOCKET s) {
        using namespace usbip;
        std::vector<unsigned char> hdr(kUrbHeaderSize);
        std::vector<unsigned char> payload;
        std::vector<unsigned char> iso_raw;
        for (;;) {
            if (!recv_all(s, hdr.data(), hdr.size())) return;
            auto cmd = decode_urb_header(hdr);
            if (!cmd) return; // 协议错误：断开
            if (cmd->is_submit) {
                const size_t plen = payload_len_after_header(*cmd);
                payload.assign(plen, 0);
                if (plen && !recv_all(s, payload.data(), plen)) return;
                std::vector<IsoPacket> iso;
                if (is_iso(cmd->submit.number_of_packets)) {
                    const size_t n = cmd->submit.number_of_packets * kIsoDescSize;
                    iso_raw.assign(n, 0);
                    if (!recv_all(s, iso_raw.data(), n)) return;
                    iso = decode_iso_descs(iso_raw);
                }
                disp->on_urb(*cmd, payload, iso, wrap_completion(cmd->basic.seqnum, CompletionFn{}));
            } else {
                {
                    std::lock_guard lk(cancel_mtx);
                    cancelled.insert(cmd->unlink_seqnum);
                }
                disp->on_unlink(cmd->basic.seqnum, cmd->unlink_seqnum,
                                wrap_unlink(cmd->basic.seqnum, UnlinkResultFn{}));
            }
        }
    }
};

UsbIpServer::UsbIpServer() : impl_(std::make_unique<Impl>()) {}
UsbIpServer::~UsbIpServer() { stop(); }

bool UsbIpServer::start(uint16_t port, UrbDispatcher& dispatcher,
                        std::vector<usbip::UsbIpDevice> exportable) {
    wsa_init_once();
    impl_->disp = &dispatcher;
    impl_->devices = std::move(exportable);

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001); // 仅回环
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        fprintf(stderr, "bind(%u) wsaerr=%d\n", port, WSAGetLastError());
        closesocket(ls);
        return false;
    }
    if (listen(ls, 4) == SOCKET_ERROR) {
        closesocket(ls);
        return false;
    }
    impl_->listener.store(ls);
    impl_->stopping.store(false); // 支持复用（stop→start 循环）

    impl_->accept_thread = std::thread([this] {
        // Windows 上 closesocket 无法解除阻塞中的 accept()，用 select+短超时轮询退出标志
        for (;;) {
            if (impl_->stopping.load()) return;
            SOCKET ls = impl_->listener.load();
            if (ls == INVALID_SOCKET) return;
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(ls, &readfds);
            timeval tv{0, 200 * 1000}; // 200ms
            int r = select(0, &readfds, nullptr, nullptr, &tv);
            if (r <= 0) continue; // 超时/信号：回头检查 stopping
            SOCKET s = accept(ls, nullptr, nullptr);
            if (s == INVALID_SOCKET) return;
            if (impl_->stopping.load()) { closesocket(s); return; }
            impl_->serve_connection(s);
        }
    });
    return true;
}

void UsbIpServer::stop() {
    if (impl_->stopping.exchange(true)) return;
    // 顺序关键：① shutdown 数据套接字唤醒阻塞中的 urb_loop/recv（线程因此可退出）
    //           ② join 线程（accept 线程 100ms select 轮询会看到 stopping）
    //           ③ 线程退出后再关 listener —— 并发 select 中的 closesocket 在 Windows
    //              上不保证立即释放端口（此前热切换 10048 的根因）
    SOCKET ds = impl_->data_sock.exchange(INVALID_SOCKET);
    if (ds != INVALID_SOCKET) { shutdown(ds, SD_BOTH); closesocket(ds); }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    SOCKET ls = impl_->listener.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) { shutdown(ls, SD_BOTH); closesocket(ls); }
    impl_->attached.store(false);
}

void UsbIpServer::kick() {
    SOCKET ds = impl_->data_sock.exchange(INVALID_SOCKET);
    if (ds != INVALID_SOCKET) { shutdown(ds, SD_BOTH); closesocket(ds); }
}

bool UsbIpServer::attached() const { return impl_->attached.load(); }
uint16_t UsbIpServer::port() const {
    SOCKET ls = impl_->listener.load();
    if (ls == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

} // namespace uac
