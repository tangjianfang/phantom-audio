#pragma once
// 请求日志（NDJSON 行，内存环形 + 可选文件 sink）
#include <cstddef>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace uac {

class LogStore {
public:
    void append(std::string_view ndjsonLine);
    std::vector<std::string> tail(std::size_t n) const;
    std::size_t count() const;
    void clear();
    bool set_file_sink(const std::string& path); // 追加写；失败返回 false

private:
    static constexpr std::size_t kMaxLines = 20000;
    mutable std::mutex mtx_;
    std::deque<std::string> ring_;
    std::ofstream file_;
};

} // namespace uac
