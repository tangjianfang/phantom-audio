// LogStore 实现
#include <uac/log_store.h>

namespace uac {

void LogStore::append(std::string_view line) {
    std::lock_guard lk(mtx_);
    ring_.emplace_back(line);
    if (ring_.size() > kMaxLines) ring_.pop_front();
    if (file_.is_open()) {
        file_ << line << '\n';
        if (ring_.size() % 64 == 0) file_.flush(); // 批量刷新
    }
}

std::vector<std::string> LogStore::tail(std::size_t n) const {
    std::lock_guard lk(mtx_);
    std::vector<std::string> out;
    auto begin = ring_.size() > n ? ring_.end() - static_cast<std::ptrdiff_t>(n) : ring_.begin();
    out.assign(begin, ring_.end());
    return out;
}

std::size_t LogStore::count() const {
    std::lock_guard lk(mtx_);
    return ring_.size();
}

void LogStore::clear() {
    std::lock_guard lk(mtx_);
    ring_.clear();
}

bool LogStore::set_file_sink(const std::string& path) {
    std::lock_guard lk(mtx_);
    if (file_.is_open()) file_.close(); // close 触发 flush；已开流上 open 会失败
    if (path.empty()) return false;
    file_.open(path, std::ios::app);
    return file_.is_open();
}

} // namespace uac
