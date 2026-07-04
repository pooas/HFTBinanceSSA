#ifndef ADAPTIVE_L_HPP
#define ADAPTIVE_L_HPP

#include <cmath>
#include <algorithm>

// ============================================================================
// AdaptiveL — Maps HMM regime probabilities to stable integer window lengths
//
// Three-stage pipeline (pure arithmetic, no allocations, no branching beyond
// the single hysteresis check):
//
//   Stage 1: Raw target from regime probabilities
//     L_raw = L_min + (L_max - L_min) * (P_calm + η * P_trend)
//
//   Stage 2: Exponential smoothing
//     L_ema = α * L_raw + (1 - α) * L_ema_prev
//
//   Stage 3: Hysteresis deadband + odd-enforcement
//     if |L_ema - L_current| > Δ_h: L_current = round_odd(L_ema)
//     else: L_current unchanged
//
// Derived windows:
//   L_slow = L_current
//   L_fast = max(L_min, round_odd(L_current * ρ))
//
// Thread safety: HMM probabilities are read via std::atomic<double>::load
// with memory_order_relaxed in the caller. This class itself is single-threaded
// (called only from the main processing loop).
// ============================================================================

struct AdaptiveLResult {
    int  L_slow;
    int  L_fast;
    bool changed;   // true if L actually updated (triggers pipeline resize)
};

class AdaptiveL {
public:
    // Default parameters tuned for 50ms tick polling
    static constexpr int    L_MIN     = 15;
    static constexpr int    L_MAX     = 121;
    static constexpr double ETA       = 0.6;    // trending weight factor
    static constexpr double ALPHA_L   = 0.03;   // EMA smoothing (~33-sample window)
    static constexpr int    DELTA_H   = 4;      // hysteresis deadband (samples)
    static constexpr double RHO       = 0.4;    // fast/slow ratio

    AdaptiveL()
        : L_ema_(static_cast<double>((L_MIN + L_MAX) / 2)),
          L_current_(round_odd_clamp(static_cast<double>((L_MIN + L_MAX) / 2)))
    {}

    AdaptiveL(int initial_L)
        : L_ema_(static_cast<double>(initial_L)),
          L_current_(round_odd_clamp(static_cast<double>(initial_L)))
    {}

    // Main update — called once per tick with relaxed-loaded HMM probabilities.
    // Pure arithmetic path: no allocation, no branching beyond hysteresis.
    AdaptiveLResult update(double p_calm, double p_trend, double p_crisis) {
        (void)p_crisis; // implicit via p_calm + p_trend (they sum to ~1)

        // Stage 1: Raw target from regime probabilities
        const double L_raw = static_cast<double>(L_MIN)
            + static_cast<double>(L_MAX - L_MIN) * (p_calm + ETA * p_trend);

        // Stage 2: Exponential smoothing
        L_ema_ = ALPHA_L * L_raw + (1.0 - ALPHA_L) * L_ema_;

        // Stage 3: Hysteresis deadband + odd enforcement
        AdaptiveLResult result;
        const double drift = L_ema_ - static_cast<double>(L_current_);

        if (std::abs(drift) > static_cast<double>(DELTA_H)) {
            L_current_ = round_odd_clamp(L_ema_);
            result.changed = true;
        } else {
            result.changed = false;
        }

        // Derived windows
        result.L_slow = L_current_;
        result.L_fast = std::max(L_MIN, round_odd_fast(
            static_cast<double>(L_current_) * RHO));

        return result;
    }

    int L_slow() const { return L_current_; }
    int L_fast() const { return std::max(L_MIN, round_odd_fast(
        static_cast<double>(L_current_) * RHO)); }
    double L_ema() const { return L_ema_; }

private:
    double L_ema_;      // smoothed continuous window estimate
    int    L_current_;  // committed integer window (odd)

    // Branchless round-to-odd: cast to int then OR with 1.
    // Guarantees odd result for any positive input.
    static int round_odd_fast(double x) {
        return static_cast<int>(x) | 1;
    }

    // Round to odd with clamping to [L_MIN, L_MAX].
    static int round_odd_clamp(double x) {
        int val = static_cast<int>(x) | 1;
        if (val < L_MIN) val = L_MIN;
        if (val > L_MAX) val = L_MAX;
        return val;
    }
};

#endif // ADAPTIVE_L_HPP
