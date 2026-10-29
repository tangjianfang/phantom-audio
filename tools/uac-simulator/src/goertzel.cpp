// Goertzel 实现
#include <uac/goertzel.h>
#include <cmath>

namespace uac {

double goertzel_power(const float* x, std::size_t n, double targetHz, double sampleRate) {
    if (n == 0 || sampleRate <= 0) return 0.0;
    const double k = std::round(n * targetHz / sampleRate);
    const double w = 2.0 * 3.14159265358979323846 * k / static_cast<double>(n);
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power / static_cast<double>(n * n) * 4.0; // 归一化到幅度平方
}

double goertzel_power(const std::vector<float>& x, double targetHz, double sampleRate) {
    return goertzel_power(x.data(), x.size(), targetHz, sampleRate);
}

std::vector<float> synth_sine(double hz, double amp, std::size_t n, double sampleRate) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979323846 * hz *
                                                static_cast<double>(i) / sampleRate));
    return v;
}

} // namespace uac
