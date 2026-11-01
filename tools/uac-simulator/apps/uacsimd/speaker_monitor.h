// SpeakerMonitor —— 把虚拟扬声器收到的音频从真实输出设备放出来（像真实声卡出声）。
// 设备选择：winmm 枚举里第一个名字不含排除串（模拟器自己的产品名）的输出设备，
// 避免把声音播回虚拟设备自己（那会经 Windows 混音再次灌进虚拟扬声器）。
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace uac {

class SpeakerMonitor {
public:
    // excludeName：排除的设备名子串（模拟器产品名，如 "UAC2 Simulator"）。返回 false = 没有可用设备。
    bool start(const std::string& excludeName);
    void stop();
    bool running() const { return waveOut_ != nullptr; }
    // 已选定真实输出设备（打开延迟到首帧音频）：有声音进来就会播
    bool ready() const { return chosenValid_; }
    const std::string& status() const { return status_; } // 给 /status 与日志的说明

    // 引擎线程调用：16 位交错 PCM。内部只入队（锁 + 拷贝），不阻塞。
    void feed(const std::int16_t* pcm, std::size_t count, std::uint32_t rate, std::uint8_t channels);

private:
    void reopenIfNeeded(std::uint32_t rate, std::uint8_t channels);
    void stopLocked(); // 调用方已持 mtx_
    void drainQueue(); // 把队列灌进空闲的 WAVEHDR（调用方已持 mtx_）

    void* waveOut_ = nullptr;      // HWAVEOUT（避免头文件依赖 mmsystem）
    void* headers_ = nullptr;      // WAVEHDR[4]
    std::vector<std::vector<unsigned char>> buffers_; // 每个 WAVEHDR 的数据缓冲
    unsigned chosen_ = 0xFFFFFFFF; // winmm 设备号（0xFFFFFFFF = WAVE_MAPPER 哨兵：不可用）
    bool chosenValid_ = false;
    std::uint32_t rate_ = 0;
    std::uint8_t channels_ = 0;
    std::string exclude_, status_;
    std::mutex mtx_;
    std::deque<std::int16_t> queue_;
    static constexpr std::size_t kMaxQueueFrames = 48000; // 1s 上限：消费不动就丢（不无限积压）
};

} // namespace uac
