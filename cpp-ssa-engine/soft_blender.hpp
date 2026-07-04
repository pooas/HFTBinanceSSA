#ifndef SOFT_BLENDER_HPP
#define SOFT_BLENDER_HPP

#include "ssa_types.hpp"
#include <cmath>

// ============================================================================
// SoftBlender — Regime-adaptive fusion of dual SSA pipelines
//
// Blends R_fast(t) and R_slow(t) into a composite signal with smooth,
// continuous transitions driven by HMM regime probabilities and EVR metrics.
//
// Pure function: identical output for identical inputs regardless of call
// frequency. No hidden state.
//
// Math:
//   φ(t) = P_calm + P_trend - P_crisis           ∈ [-1, +1]
//   ΔEVR  = EVR_slow - EVR_fast
//   w(t)  = 0.5 + 0.5 · tanh(γ·φ + β·ΔEVR)
//
//   S(t)       = w·R_slow + (1-w)·R_fast
//   Ṡ(t)       = w·slope_slow + (1-w)·slope_fast
//   S̈(t)       = w·accel_slow + (1-w)·accel_fast
//   sideway(t) = 1 - (w·EVR_slow + (1-w)·EVR_fast)
//
// Behavior:
//   Calm/trending regime (φ → +1) AND slow pipeline explains more (ΔEVR > 0):
//     w → 1, output follows R_slow (smoother, more stable)
//   Crisis regime (φ → -1) OR fast pipeline captures more structure:
//     w → 0, output follows R_fast (more responsive)
// ============================================================================

struct BlendedOutput {
    double smoothed;
    double slope;
    double accel;
    double sideway;
    float  blend_weight;  // w(t), for diagnostics
    float  evr_fast;
    float  evr_slow;
    float  eigen_gap;
};

class SoftBlender {
public:
    // Compile-time constants (tunable via recompile)
    static constexpr double GAMMA = 2.0;  // regime-driven blend sharpness
    static constexpr double BETA  = 1.0;  // EVR contribution modulator

    // Pure function: blend two pipeline results with regime probabilities.
    static BlendedOutput blend(const SsaResult& fast, const SsaResult& slow,
                               double p_calm, double p_trend, double p_crisis)
    {
        // Regime stability score: φ ∈ [-1, +1]
        const double phi = p_calm + p_trend - p_crisis;

        // EVR advantage of slow pipeline over fast
        const double d_evr = slow.evr - fast.evr;

        // Blend weight via tanh (smooth, no discontinuities)
        const double w = 0.5 + 0.5 * std::tanh(GAMMA * phi + BETA * d_evr);

        // One-minus-w (computed once for efficiency)
        const double w_inv = 1.0 - w;

        BlendedOutput out;
        out.smoothed     = w * slow.smoothed + w_inv * fast.smoothed;
        out.slope        = w * slow.slope    + w_inv * fast.slope;
        out.accel        = w * slow.accel    + w_inv * fast.accel;
        out.sideway      = 1.0 - (w * slow.evr + w_inv * fast.evr);
        out.blend_weight = static_cast<float>(w);
        out.evr_fast     = static_cast<float>(fast.evr);
        out.evr_slow     = static_cast<float>(slow.evr);
        out.eigen_gap    = static_cast<float>(
            w * slow.eigen_gap + w_inv * fast.eigen_gap);

        return out;
    }
};

#endif // SOFT_BLENDER_HPP
