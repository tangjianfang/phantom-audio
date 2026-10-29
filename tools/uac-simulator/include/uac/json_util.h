#pragma once
// Shared JSON string escaping (single implementation for GUI/CLI/test suite).
#include <string>

namespace uac {

// Escape a string for embedding inside a JSON string literal: Windows path
// backslashes and quotes would otherwise produce invalid JSON.
inline std::string JsonEscape(const std::string& in) {
    constexpr char kBs = 0x5C; // backslash
    std::string out;
    for (char c : in) {
        if (c == kBs || c == '"') out += kBs;
        out += c;
    }
    return out;
}

} // namespace uac
