// SpeakerMonitor 实现：winmm waveOut 双缓冲播放（测试台够用；不用 WASAPI 的复杂事件模型）。
#include "speaker_monitor.h"

#include <algorithm>
#include <cstring>
#include <windows.h>
#include <mmsystem.h>

namespace uac {

namespace {
constexpr int kHeaders = 4;
constexpr int kHeaderMs = 100; // 每块 100ms → 预备 400ms，延迟可接受且不抖

void CALLBACK wave_out_proc(HWAVEOUT, UINT msg, DWORD_PTR instance, DWORD_PTR, DWORD_PTR) {
    if (msg != WOM_DONE) return;
    // 不在这里 drain（回调线程约束）：主路径由 feed 驱动，这里只标记头已还（WAVEHDR.whdrDone 由系统置位）。
    (void)instance;
}
} // namespace

bool SpeakerMonitor::start(const std::string& excludeName) {
    {
        std::lock_guard lk(mtx_);
        stopLocked();
    }
    exclude_ = excludeName;
    // 选设备：第一个名字不含排除串的波形输出设备。WAVE_MAPPER（默认）不行——虚拟设备
    // 被设为默认后它就是默认，会把声音播回虚拟自己。
    UINT chosen = WAVE_MAPPER;
    const UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        const std::wstring name(caps.szPname);
        if (exclude_.empty() ||
            std::search(name.begin(), name.end(), exclude_.begin(), exclude_.end()) ==
                name.end()) {
            chosen = i;
            break;
        }
    }
    if (chosen == WAVE_MAPPER && !exclude_.empty()) {
        status_ = "no real output device found (only the simulator itself)";
        chosenValid_ = false;
        return false;
    }
    // 打开动作延迟到首帧（那时才知道虚拟流的 rate/channels）。
    status_ = "ready";
    chosen_ = chosen;
    chosenValid_ = true;
    return true;
}

void SpeakerMonitor::stop() {
    std::lock_guard lk(mtx_);
    stopLocked();
}

// 调用方必须已持有 mtx_
void SpeakerMonitor::stopLocked() {
    if (waveOut_) {
        waveOutReset(static_cast<HWAVEOUT>(waveOut_));
        for (int i = 0; i < kHeaders; ++i)
            if (headers_ && static_cast<WAVEHDR*>(headers_)[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(static_cast<HWAVEOUT>(waveOut_),
                                       &static_cast<WAVEHDR*>(headers_)[i], sizeof(WAVEHDR));
        waveOutClose(static_cast<HWAVEOUT>(waveOut_));
        waveOut_ = nullptr;
    }
    delete[] static_cast<WAVEHDR*>(headers_);
    headers_ = nullptr;
    buffers_.clear();
    queue_.clear(); // 调用方已持 mtx_
    rate_ = 0;
    channels_ = 0;
}

void SpeakerMonitor::reopenIfNeeded(std::uint32_t rate, std::uint8_t channels) {
    if (waveOut_ && rate == rate_ && channels == channels_) return;
    stopLocked();
    rate_ = rate;
    channels_ = channels;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = channels;
    fmt.nSamplesPerSec = rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = channels * 2;
    fmt.nAvgBytesPerSec = rate * fmt.nBlockAlign;
    HWAVEOUT out = nullptr;
    if (waveOutOpen(&out, chosen_, &fmt, reinterpret_cast<DWORD_PTR>(&wave_out_proc), 0,
                    CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        status_ = "waveOutOpen failed";
        rate_ = 0;
        return;
    }
    waveOut_ = out;
    auto* hdrs = new WAVEHDR[kHeaders]{};
    headers_ = hdrs;
    buffers_.assign(kHeaders, std::vector<unsigned char>(rate / (1000 / kHeaderMs) * channels * 2 + 4));
    for (int i = 0; i < kHeaders; ++i) {
        hdrs[i].lpData = reinterpret_cast<LPSTR>(buffers_[i].data());
        hdrs[i].dwBufferLength = static_cast<DWORD>(buffers_[i].size());
    }
    status_ = "playing to real output device";
}

void SpeakerMonitor::feed(const std::int16_t* pcm, std::size_t count, std::uint32_t rate,
                          std::uint8_t channels) {
    if (!pcm || !count || !rate || !channels) return;
    std::lock_guard lk(mtx_);
    if (!chosenValid_) return;
    reopenIfNeeded(rate, channels);
    if (!waveOut_) return;
    // 队列满就丢新数据（听感上跳一下，好过无限延迟）
    if (queue_.size() / channels >= kMaxQueueFrames) queue_.clear();
    queue_.insert(queue_.end(), pcm, pcm + count);
    drainQueue();
}

void SpeakerMonitor::drainQueue() {
    auto* hdrs = static_cast<WAVEHDR*>(headers_);
    for (int i = 0; i < kHeaders && !queue_.empty(); ++i) {
        if (hdrs[i].dwFlags & WHDR_INQUEUE) continue;
        const std::size_t capFrames = buffers_[i].size() / (channels_ * 2);
        const std::size_t frames = std::min(capFrames, queue_.size() / channels_);
        if (!frames) break;
        // 不足整块的尾部留到下一包（避免末尾噪声），本块只写整帧
        const std::size_t samples = frames * channels_;
        std::copy(queue_.begin(), queue_.begin() + samples,
                  reinterpret_cast<std::int16_t*>(buffers_[i].data()));
        queue_.erase(queue_.begin(), queue_.begin() + samples);
        hdrs[i].dwBufferLength = static_cast<DWORD>(samples * 2);
        hdrs[i].dwFlags = 0;
        waveOutPrepareHeader(static_cast<HWAVEOUT>(waveOut_), &hdrs[i], sizeof(WAVEHDR));
        waveOutWrite(static_cast<HWAVEOUT>(waveOut_), &hdrs[i], sizeof(WAVEHDR));
    }
}

} // namespace uac
