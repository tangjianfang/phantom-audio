// Goertzel 数学测试
#include <doctest/doctest.h>
#include <uac/goertzel.h>

using namespace uac;

TEST_CASE("1kHz sine detected with expected amplitude") {
    auto x = synth_sine(1000.0, 0.5, 4800, 48000.0); // N=4800：1kHz 恰为第 100 个频 Bin
    const auto p = goertzel_power(x, 1000.0, 48000.0);
    // 幅度 0.5 → 归一化功率 ≈ 0.5²=0.25
    CHECK(p > 0.22);
    CHECK(p < 0.28);
}

TEST_CASE("off-frequency probe is rejected") {
    auto x = synth_sine(2000.0, 0.5, 4800, 48000.0); // 整 Bin：第 200
    const auto pWrong = goertzel_power(x, 1000.0, 48000.0);
    const auto pRight = goertzel_power(x, 2000.0, 48000.0);
    CHECK(pRight > 0.2);
    CHECK(pWrong < pRight * 0.01);
}

TEST_CASE("mixed signal: fundamental dominates") {
    auto a = synth_sine(440.0, 0.8, 48000, 48000.0); // 整 Bin：第 440
    auto b = synth_sine(3000.0, 0.1, 48000, 48000.0);
    std::vector<float> mix(48000);
    for (std::size_t i = 0; i < mix.size(); ++i) mix[i] = a[i] + b[i];
    const auto p440 = goertzel_power(mix, 440.0, 48000.0);
    const auto p3k = goertzel_power(mix, 3000.0, 48000.0);
    CHECK(p440 > 0.5);   // 0.8² = 0.64
    CHECK(p3k > 0.005);  // 0.1² = 0.01
    CHECK(p3k < p440);
}

TEST_CASE("silence and degenerate inputs") {
    std::vector<float> zeros(1024, 0.0f);
    CHECK(goertzel_power(zeros, 1000.0, 48000.0) < 1e-12);
    CHECK(goertzel_power(nullptr, 0, 1000.0, 48000.0) == 0.0);
    std::vector<float> empty;
    CHECK(goertzel_power(empty, 1000.0, 48000.0) == 0.0);
}

TEST_CASE("44.1k rate and odd sizes work") {
    auto x = synth_sine(997.0, 0.3, 1023, 44100.0);
    const auto p = goertzel_power(x, 997.0, 44100.0);
    CHECK(p > 0.06); // 0.09 容差放宽（非整周期泄漏）
}
