// USB/IP server 传输层实现（Winsock2，仅 127.0.0.1，多设备并发通道）
#include <uac/usbip_server.h>
#include <uac/endian.h>
#include <uac/net_util.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace uac {

namespace { // anonymous
    using net::send_all;
    using net::recv_all;

std::once_flag g_wsa_once;
void wsa_init_once() {
    std::call_once(g_wsa_once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
}






} // namespace

struct UsbIpServer::Impl {
    struct Conn { // 每个 IMPORT 连接的独立状态
        SOCKET s = INVALID_SOCKET;
        UrbDispatcher* disp = nullptr; // owning device model's dispatcher
        std::mutex cancelMtx;
        std::set<uint32_t> cancelled;
        std::mutex sendMtx;
    };

    // UNLINK 抢先标记的上限。正常情况下标记在对应 RET_SUBMIT 被丢弃时就移除；
    // 只有"URB 早已完成才收到 UNLINK"的标记会残留，封顶避免长时间运行时无限增长
    // （seqnum 单调递增，set 的最小元素即最旧的标记）。
    static constexpr std::size_t kMaxCancelled = 4096;

    std::atomic<bool> stopping{false};
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<int> controlIdleMs{10000};
    std::thread acceptThread;

    // exported devices with their owning dispatchers (busid -> pair)
    std::vector<std::pair<usbip::UsbIpDevice, UrbDispatcher*>> devices;

    std::mutex devMtx; // busid → 通道（attached 即在表中）
    std::map<std::string, std::shared_ptr<Conn>> channels;

    // 通道线程：每次 attach 一个。线程结束后置 done，由 accept 线程在下次 IMPORT 前
    // 回收（join），避免反复 attach/detach 时 std::thread 对象无限累积；stop() 兜底全部 join。
    struct ChannelThread {
        std::thread t;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<ChannelThread> channelThreads;

    void reap_channel_threads() { // 仅 accept 线程调用（stop() 在其 join 之后才触碰该容器）
        for (auto it = channelThreads.begin(); it != channelThreads.end();) {
            if (it->done->load()) {
                if (it->t.joinable()) it->t.join();
                it = channelThreads.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool send_on(Conn& c, std::span<const unsigned char> a, std::span<const unsigned char> b = {},
                 std::span<const unsigned char> d = {}) { // uac-cov:thread
        std::lock_guard lk(c.sendMtx);
        if (c.s == INVALID_SOCKET) return false;
        return send_all(c.s, a) && (b.empty() || send_all(c.s, b)) &&
               (d.empty() || send_all(c.s, d));
    }

    CompletionFn wrap_completion(std::shared_ptr<Conn> c, uint32_t seqnum,
                                 CompletionFn&& complete) { // uac-cov:thread
        return [this, c = std::move(c), seqnum, cb = std::move(complete)](Completion&& cm) mutable {
            {
                std::lock_guard lk(c->cancelMtx);
                if (const auto it = c->cancelled.find(seqnum); it != c->cancelled.end()) {
                    c->cancelled.erase(it); // UNLINK 抢先：静默丢弃，标记用完即删
                    return;
                }
            }
            using namespace usbip;
            std::vector<unsigned char> wire(kUrbHeaderSize);
            uac::wr_be<uint32_t>(wire.data() + 0, RET_SUBMIT);
            uac::wr_be<uint32_t>(wire.data() + 4, seqnum);
            uac::wr_be<int32_t>(wire.data() + 20, cm.ret.status);
            uac::wr_be<uint32_t>(wire.data() + 24, cm.ret.actual_length);
            uac::wr_be<int32_t>(wire.data() + 28, cm.ret.start_frame);
            uac::wr_be<uint32_t>(wire.data() + 32, cm.ret.number_of_packets);
            uac::wr_be<uint32_t>(wire.data() + 36, cm.ret.error_count);
            auto iso = encode_iso_descs(cm.iso);
            send_on(*c, wire, cm.payload, iso);
            cb = {};
        };
    }

    UnlinkResultFn wrap_unlink(std::shared_ptr<Conn> c, uint32_t seqnum,
                               UnlinkResultFn&& done) { // uac-cov:thread
        return [this, c = std::move(c), seqnum, cb = std::move(done)](int32_t status) mutable {
            usbip::BasicHeader req{};
            req.seqnum = seqnum;
            send_on(*c, usbip::encode_ret_unlink(req, status));
            cb = {};
        };
    }

    // 控制连接（DEVLIST / IMPORT 握手）；IMPORT 成功则启动通道线程并返回
    void serve_control(SOCKET s) {
        using namespace usbip;
        // 250ms receive timeout + stopping recheck (tri-state read keeps
        // partial bytes across timeouts, so a throttled client splitting an
        // 8-byte header does not misalign the stream).
        net::set_recv_timeout(s, 250);
        // 空闲时限：自上一次完整消息（或连接建立）起超过 controlIdleMs 仍无进展就放弃。
        // 超时分支原先只在 stopping 时才退出，空闲不关的对端会永久占住串行的 accept 线程。
        using Clock = std::chrono::steady_clock;
        auto lastProgress = Clock::now();
        const auto idle_expired = [&] {
            return Clock::now() - lastProgress > std::chrono::milliseconds(controlIdleMs.load());
        };
        unsigned char common[8];
        std::size_t off = 0;
        for (;;) {
            switch (net::recv_until(s, common, sizeof(common), off)) {
            case net::RecvResult::Ok:
                off = 0;
                lastProgress = Clock::now();
                break;
            case net::RecvResult::Timeout:
                if (!stopping.load() && !idle_expired()) continue; // 仍在时限内
                closesocket(s);
                return;
            case net::RecvResult::Closed:
                closesocket(s);
                return;
            }
            if (uac::rd_be<uint16_t>(&common[0]) != kVersion) break;
            const auto code = uac::rd_be<uint16_t>(&common[2]);

            if (code == OP_REQ_DEVLIST) {
                std::vector<UsbIpDevice> list;
                list.reserve(devices.size());
                for (auto&& d : devices) list.push_back(d.first);
                if (!send_all(s, encode_devlist_reply(list))) break;
                continue;
            }
            if (code == OP_REQ_IMPORT) {
                unsigned char body[32];
                std::size_t boff = 0;
                bool bodyOk = false;
                for (;;) {
                    const auto rr = net::recv_until(s, body, sizeof(body), boff);
                    if (rr == net::RecvResult::Ok) { bodyOk = true; break; }
                    if (rr == net::RecvResult::Timeout && !stopping.load() && !idle_expired())
                        continue;
                    break;
                }
                if (!bodyOk) { closesocket(s); return; }
                const auto busid = decode_import_busid({body, 32});
                const std::pair<usbip::UsbIpDevice, UrbDispatcher*>* found = nullptr;
                for (auto&& d : devices)
                    if (d.first.busid == busid) { found = &d; break; }
                if (!found) {
                    send_all(s, encode_import_reply(devices.empty() ? UsbIpDevice{} : devices[0].first, 1));
                    break;
                }
                auto conn = std::make_shared<Conn>();
                conn->s = s;
                conn->disp = found->second;
                {
                    std::lock_guard lk(devMtx);
                    if (channels.count(busid)) { // 已占用
                        send_all(s, encode_import_reply(found->first, 1));
                        break;
                    }
                    if (!send_all(s, encode_import_reply(found->first, 0))) break;
                    channels[busid] = conn;
                }
                // The channel loop uses blocking reads: clear the serve-loop
                // receive timeout, or a quiet bus (>250ms) would look like a
                // dead peer and tear the channel down mid-stream.
                DWORD none = 0;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&none),
                           sizeof(none));
                conn->disp->on_import(found->first);
                UrbDispatcher* disp = conn->disp;
                reap_channel_threads(); // 回收此前已结束的通道线程
                auto done = std::make_shared<std::atomic<bool>>(false);
                channelThreads.push_back({std::thread([this, busid, conn, disp, done] { // uac-cov:thread
                                              urb_loop(conn);
                                              {
                                                  std::lock_guard lk(devMtx);
                                                  channels.erase(busid);
                                              }
                                              disp->on_detach();
                                              done->store(true);
                                          }),
                                          done});
                return; // 连接归通道线程
            }
            break;
        }
        closesocket(s);
    }

    // 直接持有 shared_ptr<Conn>：完成回调需要共享所有权（线程可能先于回调结束），
    // 此前靠 shared_from() 扫描 channels 反查，找不到时会返回空指针。
    void urb_loop(const std::shared_ptr<Conn>& connPtr) {
        using namespace usbip;
        Conn& c = *connPtr;
        std::vector<unsigned char> hdr(kUrbHeaderSize);
        std::vector<unsigned char> payload;
        std::vector<unsigned char> iso_raw;
        for (;;) {
            if (!recv_all(c.s, hdr.data(), hdr.size())) break;
            auto cmd = decode_urb_header(hdr);
            if (!cmd) break;
            if (cmd->is_submit) {
                const size_t plen = payload_len_after_header(*cmd);
                payload.assign(plen, 0);
                if (plen && !recv_all(c.s, payload.data(), plen)) break;
                std::vector<IsoPacket> iso;
                if (is_iso(cmd->submit.number_of_packets)) {
                    const size_t n = cmd->submit.number_of_packets * kIsoDescSize;
                    iso_raw.assign(n, 0);
                    if (!recv_all(c.s, iso_raw.data(), n)) break;
                    iso = decode_iso_descs(iso_raw);
                }
                c.disp->on_urb(*cmd, payload, iso,
                               wrap_completion(connPtr, cmd->basic.seqnum, CompletionFn{}));
            } else {
                {
                    std::lock_guard lk(c.cancelMtx);
                    c.cancelled.insert(cmd->unlink_seqnum);
                    while (c.cancelled.size() > kMaxCancelled)
                        c.cancelled.erase(c.cancelled.begin()); // 最旧的标记
                }
                c.disp->on_unlink(cmd->basic.seqnum, cmd->unlink_seqnum,
                                  wrap_unlink(connPtr, cmd->basic.seqnum, UnlinkResultFn{}));
            }
        }
        close_conn(c);
    }

    // Single-lock close for a channel socket: stop()/kick() and the urb_loop
    // exit path both go through devMtx, so the handle can never be closed
    // twice (a recycled handle would kill an unrelated live connection).
    void close_conn(Conn& c) {
        std::lock_guard lk(devMtx);
        if (c.s != INVALID_SOCKET) { shutdown(c.s, SD_BOTH); closesocket(c.s); c.s = INVALID_SOCKET; }
    }

};

UsbIpServer::UsbIpServer() : impl_(std::make_unique<Impl>()) {}
UsbIpServer::~UsbIpServer() { stop(); }

void UsbIpServer::set_control_idle_timeout_ms(int ms) { impl_->controlIdleMs.store(ms); }

bool UsbIpServer::start(uint16_t port, const std::vector<Export>& exportable) {
    wsa_init_once();
    impl_->devices = exportable;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR ||
        listen(ls, 8) == SOCKET_ERROR) {
        closesocket(ls);
        return false;
    }
    impl_->listener.store(ls);
    impl_->stopping.store(false);

    impl_->acceptThread = std::thread([this] { // uac-cov:thread
        for (;;) {
            if (impl_->stopping.load()) return;
            SOCKET ls = impl_->listener.load();
            if (ls == INVALID_SOCKET) return;
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(ls, &rf);
            timeval tv{0, 100 * 1000};
            if (select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET s = accept(ls, nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                // transient failure (e.g. WSAECONNRESET between select and
                // accept): keep serving; backoff prevents a busy loop
                if (impl_->stopping.load()) return;
                Sleep(50);
                continue;
            }
            if (impl_->stopping.load()) { closesocket(s); return; }
            impl_->serve_control(s);
        }
    });
    return true;
}

void UsbIpServer::stop() {
    if (impl_->stopping.exchange(true)) return;
    // 顺序：唤醒所有通道读线程与正在服务的控制连接 → join 全部 → 关 listener。
    // 不 shutdown 控制连接的话，accept 线程可能阻塞在无超时的 recv 上，
    // join 永不返回（attach/detach 每次都会走 stop，整进程随之卡死）。
    {
        std::lock_guard lk(impl_->devMtx);
        for (auto&& [busid, c] : impl_->channels)
            if (c->s != INVALID_SOCKET) { shutdown(c->s, SD_BOTH); closesocket(c->s); c->s = INVALID_SOCKET; }
    }
    if (impl_->acceptThread.joinable()) impl_->acceptThread.join();
    for (auto& ct : impl_->channelThreads)
        if (ct.t.joinable()) ct.t.join();
    impl_->channelThreads.clear();
    SOCKET ls = impl_->listener.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) { shutdown(ls, SD_BOTH); closesocket(ls); }
}

void UsbIpServer::kick() {
    std::lock_guard lk(impl_->devMtx);
    for (auto&& [busid, c] : impl_->channels)
        if (c->s != INVALID_SOCKET) { shutdown(c->s, SD_BOTH); closesocket(c->s); c->s = INVALID_SOCKET; }
}

bool UsbIpServer::start(uint16_t port, UrbDispatcher& dispatcher,
                        std::vector<usbip::UsbIpDevice> exportable) {
    std::vector<Export> list;
    list.reserve(exportable.size());
    for (auto& d : exportable) list.emplace_back(std::move(d), &dispatcher);
    return start(port, list);
}

bool UsbIpServer::attached() const {
    std::lock_guard lk(impl_->devMtx);
    return !impl_->channels.empty();
}

uint16_t UsbIpServer::port() const {
    SOCKET ls = impl_->listener.load();
    if (ls == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

} // namespace uac
