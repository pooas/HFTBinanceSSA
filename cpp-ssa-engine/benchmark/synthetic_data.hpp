#ifndef SYNTHETIC_DATA_HPP
#define SYNTHETIC_DATA_HPP

#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

// ============================================================================
// Synthetic Data Generators for SSA Benchmark
//
// Each generator returns a pair: {signal, driver}
//   signal = noise-corrupted observable (what the SSA engine sees)
//   driver = noise-free ground truth (what perfect reconstruction recovers)
//
// Deterministic PRNG for reproducible benchmarks (no std::random needed).
// ============================================================================

namespace synth {

// Deterministic LCG + Box-Muller for reproducible Gaussian noise
class DeterministicRng {
public:
    explicit DeterministicRng(uint64_t seed = 42) : state_(seed) {}

    double uniform() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state_ >> 11) / static_cast<double>(1ULL << 53);
    }

    double gaussian(double sigma) {
        // Box-Muller transform
        double u1 = uniform();
        double u2 = uniform();
        if (u1 < 1e-15) u1 = 1e-15;
        double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
        return z * sigma;
    }

private:
    uint64_t state_;
};

struct SyntheticSignal {
    std::vector<double> signal;  // noisy observation
    std::vector<double> driver;  // noise-free ground truth
};

// ============================================================================
// Linear trend + Gaussian noise
//
// driver[t] = base_price + slope * t
// signal[t] = driver[t] + N(0, noise_sigma)
// ============================================================================

inline SyntheticSignal generate_trending(int N, double slope, double noise_sigma,
                                         double base_price = 100.0, uint64_t seed = 1)
{
    SyntheticSignal out;
    out.signal.resize(N);
    out.driver.resize(N);

    DeterministicRng rng(seed);

    for (int t = 0; t < N; ++t) {
        double d = base_price + slope * t;
        out.driver[t] = d;
        out.signal[t] = d + rng.gaussian(noise_sigma);
    }

    return out;
}

// ============================================================================
// Regime switch: concatenation of segments with tanh-blended joins
//
// Each segment has: { slope, duration, noise_sigma }
// Transitions are smoothed over 20 samples using tanh blending.
// ============================================================================

struct RegimeSegment {
    double slope;
    int    duration;
    double noise_sigma;
};

inline SyntheticSignal generate_regime_switch(int N, const std::vector<RegimeSegment>& segments,
                                              double base_price = 100.0, uint64_t seed = 2)
{
    SyntheticSignal out;
    out.signal.resize(N);
    out.driver.resize(N);

    DeterministicRng rng(seed);
    static constexpr int BLEND_WIDTH = 20;

    // Build piecewise-linear driver without blending first
    std::vector<double> raw_driver(N);
    std::vector<double> noise_level(N);

    int pos = 0;
    double value = base_price;

    for (const auto& seg : segments) {
        for (int t = 0; t < seg.duration && pos < N; ++t, ++pos) {
            raw_driver[pos] = value;
            noise_level[pos] = seg.noise_sigma;
            value += seg.slope;
        }
    }
    // Fill remainder if segments don't cover N
    while (pos < N) {
        raw_driver[pos] = value;
        noise_level[pos] = (segments.empty() ? 0.1 : segments.back().noise_sigma);
        ++pos;
    }

    // Apply tanh blending at segment boundaries
    std::vector<int> boundaries;
    int b = 0;
    for (const auto& seg : segments) {
        b += seg.duration;
        if (b < N) boundaries.push_back(b);
    }

    // Copy raw driver to output driver, then smooth near boundaries
    out.driver = raw_driver;

    for (int bnd : boundaries) {
        int start = std::max(0, bnd - BLEND_WIDTH);
        int end   = std::min(N, bnd + BLEND_WIDTH);
        for (int t = start; t < end; ++t) {
            double x = static_cast<double>(t - bnd) / static_cast<double>(BLEND_WIDTH);
            double alpha = 0.5 + 0.5 * std::tanh(2.5 * x);
            // Blend between the raw value at t and a linearly interpolated midpoint
            double left  = (t > 0) ? raw_driver[t - 1] : raw_driver[t];
            double right = (t < N - 1) ? raw_driver[t + 1] : raw_driver[t];
            out.driver[t] = (1.0 - alpha) * left + alpha * right;
        }
    }

    // Recompute driver as cumulative to ensure continuity after blending
    // Actually, keep the tanh-smoothed version as-is (it's already continuous)

    // Add noise
    for (int t = 0; t < N; ++t) {
        out.signal[t] = out.driver[t] + rng.gaussian(noise_level[t]);
    }

    return out;
}

// ============================================================================
// Noise burst: clean trend with an injected high-noise episode
//
// driver[t] = base_price + slope * t  (continuous everywhere)
// signal[t] = driver[t] + N(0, sigma_normal) outside burst
// signal[t] = driver[t] + N(0, burst_sigma) inside burst
// ============================================================================

inline SyntheticSignal generate_noise_burst(int N, int burst_start, int burst_duration,
                                            double burst_sigma, double slope = 0.05,
                                            double normal_sigma = 0.1,
                                            double base_price = 100.0, uint64_t seed = 3)
{
    SyntheticSignal out;
    out.signal.resize(N);
    out.driver.resize(N);

    DeterministicRng rng(seed);
    const int burst_end = burst_start + burst_duration;

    for (int t = 0; t < N; ++t) {
        double d = base_price + slope * t;
        out.driver[t] = d;

        double sigma = (t >= burst_start && t < burst_end) ? burst_sigma : normal_sigma;
        out.signal[t] = d + rng.gaussian(sigma);
    }

    return out;
}

// ============================================================================
// Sideways (range-bound): oscillatory signal with noise
//
// driver[t] = base_price + amplitude * sin(2π * t / period)
// signal[t] = driver[t] + N(0, noise_sigma)
// ============================================================================

inline SyntheticSignal generate_sideways(int N, double amplitude, double period,
                                         double noise_sigma, double base_price = 100.0,
                                         uint64_t seed = 4)
{
    SyntheticSignal out;
    out.signal.resize(N);
    out.driver.resize(N);

    DeterministicRng rng(seed);

    for (int t = 0; t < N; ++t) {
        double d = base_price + amplitude * std::sin(2.0 * M_PI * t / period);
        out.driver[t] = d;
        out.signal[t] = d + rng.gaussian(noise_sigma);
    }

    return out;
}

} // namespace synth

#endif // SYNTHETIC_DATA_HPP
