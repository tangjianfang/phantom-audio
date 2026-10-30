// 名字/路径校验实现（纯字符串逻辑）
#include <uac/path_policy.h>
#include <cstddef>
#include <string_view>

namespace uac::policy {

namespace {

bool is_ascii_alpha(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool is_ascii_alnum(unsigned char c) { return is_ascii_alpha(c) || (c >= '0' && c <= '9'); }

char ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
    return true;
}

bool is_reserved_device(std::string_view stem) {
    for (const char* n : {"CON", "PRN", "AUX", "NUL"})
        if (iequals(stem, n)) return true;
    if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
        (iequals(stem.substr(0, 3), "COM") || iequals(stem.substr(0, 3), "LPT")))
        return true;
    return false;
}

bool fail(std::string* why, const char* reason) {
    if (why) *why = reason;
    return false;
}

} // namespace

bool valid_profile_name(const std::string& name) {
    std::string_view s = name;
    constexpr std::string_view kExt = ".json";
    if (s.size() > kExt.size() && s.substr(s.size() - kExt.size()) == kExt)
        s.remove_suffix(kExt.size());
    if (s.empty() || s.size() > 64) return false;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (!is_ascii_alnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

bool valid_media_path(const std::string& path, std::string* why) {
    if (path.empty()) return fail(why, "empty path");
    if (path.size() > 259) return fail(why, "path too long");
    for (const char ch : path) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == '"' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?')
            return fail(why, "illegal character in path");
    }
    if (path.starts_with("\\\\") || path.starts_with("//"))
        return fail(why, "UNC or device path not allowed");

    // ':' 只能是盘符冒号，且其后必须是分隔符（拒绝 "C:foo.wav" 与 "x.wav:stream"）
    for (std::size_t i = 0; i < path.size(); ++i)
        if (path[i] == ':' && i != 1) return fail(why, "':' only allowed as drive designator");
    if (path.size() >= 2 && path[1] == ':') {
        if (!is_ascii_alpha(static_cast<unsigned char>(path[0])))
            return fail(why, "bad drive letter");
        if (path.size() < 3 || (path[2] != '\\' && path[2] != '/'))
            return fail(why, "drive-relative path not allowed");
    }

    // 逐分量检查
    std::size_t pos = (path.size() >= 2 && path[1] == ':') ? 2 : 0;
    while (pos <= path.size()) {
        std::size_t end = path.find_first_of("\\/", pos);
        if (end == std::string::npos) end = path.size();
        const std::string_view comp(path.data() + pos, end - pos);
        if (comp == "..") return fail(why, "'..' component not allowed");
        const std::string_view stem = comp.substr(0, comp.find('.'));
        if (!stem.empty() && is_reserved_device(stem)) return fail(why, "reserved device name");
        pos = end + 1;
    }

    constexpr std::string_view kWav = ".wav";
    if (path.size() <= kWav.size() || !iequals(std::string_view(path).substr(path.size() - kWav.size()), kWav))
        return fail(why, "only .wav files are allowed");
    return true;
}

} // namespace uac::policy
