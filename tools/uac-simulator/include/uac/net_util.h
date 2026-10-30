#pragma once
// Shared network utilities for the two loopback servers (usbip + control API).
// One tri-state receive replaces per-server copies of send_all/recv_all plus
// the WSAGetLastError() side-channel that produced a stale-error spin bug.
#include <winsock2.h>
#include <cstddef>
#include <span>

namespace uac::net {

enum class RecvResult { Ok, Timeout, Closed };

// Receive timeout on an accepted socket (ms): makes serve loops interruptible
// by a stopping flag without mirroring the socket handle anywhere.
inline void set_recv_timeout(SOCKET s, int ms) {
    DWORD v = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
}

// Read exactly n bytes into p. `off` carries partial progress: a Timeout
// return keeps already-consumed bytes so a retry continues instead of
// misaligning the stream. Ok = all n bytes read; Closed = peer closed/error.
inline RecvResult recv_until(SOCKET s, unsigned char* p, std::size_t n, std::size_t& off) {
    while (off < n) {
        int r = ::recv(s, reinterpret_cast<char*>(p + off),
                       static_cast<int>(n - off), 0);
        if (r > 0) {
            off += static_cast<std::size_t>(r);
            continue;
        }
        if (r == 0) return RecvResult::Closed; // clean close, no last-error
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) return RecvResult::Timeout;
        return RecvResult::Closed; // reset/abort
    }
    return RecvResult::Ok;
}

// Blocking full read (no timeout): connection-close aware.
inline bool recv_all(SOCKET s, unsigned char* p, std::size_t n) {
    std::size_t off = 0;
    return recv_until(s, p, n, off) == RecvResult::Ok;
}

inline bool send_all(SOCKET s, std::span<const unsigned char> b) {
    std::size_t off = 0;
    while (off < b.size()) {
        int n = ::send(s, reinterpret_cast<const char*>(b.data() + off),
                       static_cast<int>(b.size() - off), 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace uac::net
