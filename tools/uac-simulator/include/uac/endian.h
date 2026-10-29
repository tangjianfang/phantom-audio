#pragma once
// 显式大端读写助手 —— USB/IP 全协议为 network byte order
// （依据内核文档 Documentation/usb/usbip_protocol.rst：全字段大端）。
// 不依赖主机字节序；仅支持 1/2/4/8 字节整型。
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace uac {

template <class T>
concept BeScalar = std::unsigned_integral<T> || (std::signed_integral<T> && sizeof(T) > 1);

template <BeScalar T>
inline T rd_be(const void* p) {
    std::make_unsigned_t<T> u{};
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < sizeof(T); ++i) u = static_cast<decltype(u)>((u << 8) | b[i]);
    T v;
    std::memcpy(&v, &u, sizeof(T));
    return v;
}

template <BeScalar T>
inline void wr_be(void* p, T v) {
    std::make_unsigned_t<T> u;
    std::memcpy(&u, &v, sizeof(T));
    auto* b = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < sizeof(T); ++i)
        b[sizeof(T) - 1 - i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
}

// 便捷写：向 vector 末尾追加一个大端标量
template <BeScalar T>
inline void push_be(std::vector<unsigned char>& out, T v) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    wr_be(out.data() + at, v);
}

} // namespace uac
