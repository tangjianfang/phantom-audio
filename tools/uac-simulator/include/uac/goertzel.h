#pragma once
// Goertzel 单频检测（host-probe 采集正弦校验用）
#include <cstddef>
#include <vector>

namespace uac {

// 返回 targetHz 频点的归一化功率（0..1 量级，正弦幅度 a 时 ≈ a²/2）
double goertzel_power(const float* x, std::size_t n, double targetHz, double sampleRate);

// 便捷：vector 版
double goertzel_power(const std::vector<float>& x, double targetHz, double sampleRate);

// 合成测试信号（供单测/自检）
std::vector<float> synth_sine(double hz, double amp, std::size_t n, double sampleRate);

} // namespace uac
