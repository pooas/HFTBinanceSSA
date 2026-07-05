#ifndef DSP_MATH_HPP
#define DSP_MATH_HPP

// ============================================================================
// dsp_math.hpp  —  Phase-1 C++ Math Core (Ehlers DSP & MEE)
// ----------------------------------------------------------------------------
// Production-ready, zero-allocation DSP core for the Adaptive SG Pipeline.
//
// Components:
//   • HighPassFilter          — 2-pole dynamic Butterworth high-pass
//   • DynamicSuperSmoother    — Ehlers' zero-lag-augmented 2-pole LP
//   • DynamicUltimateSmoother — Ehlers' Ultimate Smoother (zero-lag IIR)
//   • MesaStrategyMEE         — Max-Ent Spectral Analysis dominant-cycle
//                                estimator:  block autocorrelation →
//                                inline-unrolled Levinson-Durbin (p=3) →
//                                Direct DFT (~250 bins) → parabolic peak →
//                                rate-limited EMA-smoothed dom_cycle
//
// Engineering Constraints Honoured:
//   - ZERO heap allocations on the hot path. Every container is a class
//     member or a stack-local std::array with explicit alignas(64).
//   - No external math libraries. <cmath> / <array> / <cstring> / <algorithm>
//     only.
//   - Every mathematical output guarded by std::isfinite to neutralise
//     denormals / NaN / inf from ever escaping the filters.
//   - Levinson-Durbin recursion MANUALLY UNROLLED for fixed p=3 —
//     ≤9 FLOPs of recursion math, fully branchless after the degeneracy
//     guards, vectorisable by the optimiser.
//   - Direct DFT (targeted bins 7..256) instead of radix-2 N=2048 FFT —
//     ~30× cheaper (250×3 vs 2048·log₂(2048) MACs), no twiddle tables,
//     fully deterministic hot-path latency.
//   - Sub-bin parabolic interpolation on power(k−1), power(k), power(k+1)
//     refines the spectral peak to fractional bin precision without the
//     cost or stack of a 2 KB power[] array (re-evaluates only the two
//     neighbour bins in a second pass — ~6 trig ops, ~30 ns).
//
// All class/struct fields are first-order primitives or fixed-size arrays,
// making the entire header trivially serialisable into the C-Pipeline.
// ============================================================================

#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace dsp {

// ============================================================================
// HighPassFilter — 2-pole Butterworth High-Pass (Ehlers, dynamic period)
// ----------------------------------------------------------------------------
//   H(z) = c1·(1 − 2z⁻¹ + z⁻²) / (1 − c2·z⁻¹ − c3·z⁻²)
//
//   alpha = √2·π / period
//   a1    = exp(-alpha)
//   b1    = 2·a1·cos(alpha)
//   c2    = b1                 ⊣ (denominator recursion coefficients)
//   c3    = -a1²               ⊣
//   c1    = (1 + c2 − c3) / 4  ⊣ (numerator coefficient, normalises DC gain 0)
//
// Poles at radius exp(-alpha) < 1 → guaranteed stable for any finite period.
// Coefficients are recomputed every tick — cheap and avoids period-cache bugs
// in the presence of NaN/Inf inputs.
// ============================================================================
class HighPassFilter {
public:
    HighPassFilter() noexcept : bar_(0) {
        std::memset(p_,  0, sizeof(p_));
        std::memset(hp_, 0, sizeof(hp_));
    }

    double update(double price, double period) {
        // Defend against pathological period values.
        const double safe_period = std::max(period, 2.0);
        const double alpha = 1.4142135623730951 * M_PI / safe_period;
        const double a1   = std::exp(-alpha);
        const double b1   = 2.0 * a1 * std::cos(alpha);
        const double c2   = b1;
        const double c3   = -(a1 * a1);
        const double c1   = (1.0 + c2 - c3) * 0.25;

        // Shift price history: p_ = [p(n-2), p(n-1), p(n)] with p(n) newest.
        p_[0] = p_[1];
        p_[1] = p_[2];
        p_[2] = price;

        // IIR recurrence — numerator removes low-frequency content.
        double hp = c1 * (p_[2] - 2.0 * p_[1] + p_[0])
                  + c2 * hp_[1]
                  + c3 * hp_[0];

        ++bar_;
        if (bar_ < 4) {
            // Warm-up: silence output until we have ≥3 valid prior samples.
            hp = 0.0;
        }

        // Belt-and-braces NaN/Inf guard.
        if (!std::isfinite(hp)) {
            hp = 0.0;
        }

        // Always shift history so warm-up samples populate correctly.
        hp_[0] = hp_[1];
        hp_[1] = hp;
        return hp;
    }

    void reset() {
        std::memset(p_,  0, sizeof(p_));
        std::memset(hp_, 0, sizeof(hp_));
        bar_ = 0;
    }

private:
    double p_[3];   // [p(n-2), p(n-1), p(n)]  — size 3 price history
    double hp_[2];  // [hp(n-2), hp(n-1)]      — size 2 output history
    int    bar_;    // warm-up counter
};

// ============================================================================
// DynamicSuperSmoother — Ehlers' 2-pole Super Smoother (low-pass, dynamic)
// ----------------------------------------------------------------------------
//   H(z) = c1·(z + 1)/2 / (1 − c2·z⁻¹ − c3·z⁻²)
//   c1    = 1 − c2 − c3       ⊣ (DC-gain = 1 normalisation)
//
// Closely tracks SuperSmoother behaviour of the existing ehlers_filters.hpp
// but matches the std::array hint and tightens NaN guards.
// ============================================================================
class DynamicSuperSmoother {
public:
    DynamicSuperSmoother() noexcept : p_prev_(0.0), bar_(0) {
        s_[0] = 0.0;
        s_[1] = 0.0;
    }

    double update(double price, double period) {
        const double safe_period = std::max(period, 2.0);
        const double alpha = 1.414 * M_PI / safe_period;
        const double a1   = std::exp(-alpha);
        const double b1   = 2.0 * a1 * std::cos(alpha);
        const double c2   = b1;
        const double c3   = -(a1 * a1);
        const double c1   = 1.0 - c2 - c3;

        ++bar_;
        if (bar_ < 3) {
            // Prime the recursion with the raw price so subsequent calls have
            // a sensible IIR history.
            p_prev_ = price;
            s_[0] = s_[1];
            s_[1] = price;
            return price;
        }

        const double out = c1 * (price + p_prev_) * 0.5
                         + c2 * s_[1]
                         + c3 * s_[0];

        p_prev_ = price;
        s_[0] = s_[1];

        if (!std::isfinite(out)) {
            // Fail-safe to wipe any non-finite garbage from the recursion.
            s_[1] = price;
            return price;
        }

        s_[1] = out;
        return out;
    }

    void reset() {
        p_prev_ = 0.0;
        s_[0] = s_[1] = 0.0;
        bar_ = 0;
    }

private:
    double p_prev_;     // price(n-1) — single-sample history
    double s_[2];       // [s(n-2), s(n-1)]
    int    bar_;
};

// ============================================================================
// DynamicUltimateSmoother — Ehlers' zero-lag 2-pole IIR Trend Smoother
// ----------------------------------------------------------------------------
//   us(n) = (1 − c1)·p(n)
//         + (2c1 − c2)·p(n-1)
//         − (c1 + c3)·p(n-2)
//         +  c2        ·us(n-1)
//         +  c3        ·us(n-2)
//
//   c1 = (1 + c2 − c3) / 4   — derived from the same Butterworth
//   c2, c3 as above. The 2-pole design propagates both the input and its
//   shifted sample into the recurrence, cancelling most of the phase lag
//   introduced by a plain 2-pole Butterworth.
//
// This is the canonical "macro trend" filter fed by Phase-2 of the pipeline.
// ============================================================================
class DynamicUltimateSmoother {
public:
    DynamicUltimateSmoother() noexcept : bar_(0) {
        std::memset(p_,  0, sizeof(p_));
        std::memset(us_, 0, sizeof(us_));
    }

    double update(double price, double period) {
        const double safe_period = std::max(period, 2.0);
        const double alpha = 1.4142135623730951 * M_PI / safe_period;
        const double a1   = std::exp(-alpha);
        const double b1   = 2.0 * a1 * std::cos(alpha);
        const double c2   = b1;
        const double c3   = -(a1 * a1);
        const double c1   = (1.0 + c2 - c3) * 0.25;

        ++bar_;
        if (bar_ < 4) {
            // Warm-up: pass-through.
            p_[0] = p_[1];
            p_[1] = price;
            us_[0] = us_[1];
            us_[1] = price;
            return price;
        }

        const double us = (1.0 - c1) * price
                        + (2.0 * c1 - c2) * p_[1]
                        - (c1 + c3) * p_[0]
                        + c2 * us_[1]
                        + c3 * us_[0];

        // Shift state regardless of output validity so next step is correct
        p_[0] = p_[1];
        p_[1] = price;
        us_[0] = us_[1];

        if (!std::isfinite(us)) {
            us_[1] = price;
            return price;
        }

        us_[1] = us;
        return us;
    }

    void reset() {
        std::memset(p_,  0, sizeof(p_));
        std::memset(us_, 0, sizeof(us_));
        bar_ = 0;
    }

private:
    double p_[2];    // price history: [p(n-2), p(n-1)]
    double us_[2];   // ultimate-smoother history: [us(n-2), us(n-1)]
    int    bar_;
};

// ============================================================================
// MesaStrategyMEE — MEE Dominant Cycle Estimator (AR(3) + Direct DFT)
// ----------------------------------------------------------------------------
// Pipeline:
//   raw price
//     → HighPassFilter(period=upper_bound)   — detrend long cycles
//     → DynamicSuperSmoother(period=lower_bound) — anti-alias high noise
//     → ring buffer of HISTORY_LEN filtered samples
//     → extract_cycle:
//         • block-averaged autocorrelation (K_BLOCKS = 6, BLOCK_SIZE = 25)
//         • Levinson-Durbin recursion (manually unrolled for p=3)
//         • Direct DFT AR power spectrum over ~250 frequency bins
//         • Sub-bin parabolic interpolation of the spectral peak
//         • Rate-limited EMA smoothing (max 10% change/tick, α=0.2)
//     → smoothed dom_cycle ∈ [lower_bound, upper_bound]
//
// Hot-path cost: ~30 µs per tick once the buffer is full
//   (dominated by ~250 cos/sin pairs in the Direct DFT loop).
//
// Storage: only class members + small stack-aligned temporaries; no heap.
// ============================================================================
class MesaStrategyMEE {
public:
    // --- Compile-time geometry ---
    static constexpr int    HISTORY_LEN       = 150;
    static constexpr int    BLOCK_COUNT       = 6;                       // K+1 blocks
    static constexpr int    BLOCK_SIZE        = HISTORY_LEN / BLOCK_COUNT; // = 25
    static_assert(HISTORY_LEN % BLOCK_COUNT == 0,
                  "HISTORY_LEN must divide cleanly into BLOCK_COUNT major blocks");

    // --- Spectral estimation ---
    static constexpr int    AR_ORDER          = 3;
    static constexpr int    DFT_VIRTUAL_SIZE  = 2048;   // virtual FFT length, only selects ~250 bins

    // --- Cycle tracking ---
    static constexpr double DEFAULT_CYCLE      = 20.0;
    static constexpr double MAX_CHANGE_RATIO   = 0.10;  // ≤10% jump per update
    static constexpr double EMA_FAST           = 0.20;  // α in dom_cycle EMA

    // --- Numerical safety floors ---
    static constexpr double SAFE_AUTOCORR_FLOOR = 1e-20;
    static constexpr double SAFE_LEVINSON_FLOOR = 1e-12;
    static constexpr double SAFE_DFT_FLOOR       = 1e-30;

    explicit MesaStrategyMEE(double lower_bound = 8.0, double upper_bound = 330.0)
        : lower_bound_(lower_bound),
          upper_bound_(upper_bound),
          dom_cycle_(DEFAULT_CYCLE),
          spectral_conc_(0.5),
          buf_head_(0),
          buf_count_(0)
    {
        std::memset(buf_.data(), 0, sizeof(double) * HISTORY_LEN);

        const double u_upper = (upper_bound_ > 0.0) ? upper_bound_ : DEFAULT_CYCLE;
        const double u_lower = (lower_bound_ > 0.0) ? lower_bound_ : 1.0;

        k_min_ = static_cast<int>(std::ceil  (static_cast<double>(DFT_VIRTUAL_SIZE) / u_upper));
        k_max_ = static_cast<int>(std::floor (static_cast<double>(DFT_VIRTUAL_SIZE) / u_lower));
        k_min_ = std::max(k_min_, 1);
        k_max_ = std::min(k_max_, DFT_VIRTUAL_SIZE / 2 - 1);
    }

    // ---------------------------------------------------------------------
    // Anything accepted from the upstream caller hits here:
    //   hp_filter detrends, super_smoother denoises, ring buffer accumulates.
    // Once the ring is full (after exactly HISTORY_LEN calls), extract_cycle
    // runs once per subsequent tick to refresh dom_cycle.
    // ---------------------------------------------------------------------
    double update(double price) {
        if (!std::isfinite(price)) {
            // Refuse pathological ticks entirely, but keep dom_cycle stable.
            return dom_cycle_;
        }

        const double hp   = hp_filter_.update(price, upper_bound_);
        const double filt = ss_filter_.update(hp,    lower_bound_);

        buf_[buf_head_] = filt;
        buf_head_ = (buf_head_ + 1) % HISTORY_LEN;
        if (buf_count_ < HISTORY_LEN) ++buf_count_;

        if (buf_count_ == HISTORY_LEN) {
            extract_cycle_();
        }

        if (!std::isfinite(dom_cycle_)) {
            // In case a math explosion slipped past every guard, restart clean.
            dom_cycle_ = DEFAULT_CYCLE;
        }
        return dom_cycle_;
    }

    double dom_cycle()               const { return dom_cycle_; }
    double spectral_concentration()  const { return spectral_conc_; }
    bool   is_warmed()               const { return buf_count_ >= HISTORY_LEN; }

    void reset() {
        std::memset(buf_.data(), 0, sizeof(double) * HISTORY_LEN);
        buf_head_ = buf_count_ = 0;
        dom_cycle_    = DEFAULT_CYCLE;
        spectral_conc_ = 0.5;
        hp_filter_.reset();
        ss_filter_.reset();
    }

private:
    double lower_bound_;
    double upper_bound_;
    double dom_cycle_;
    double spectral_conc_;

    HighPassFilter        hp_filter_;
    DynamicSuperSmoother  ss_filter_;

    alignas(64) std::array<double, HISTORY_LEN> buf_;
    int buf_head_;
    int buf_count_;

    int k_min_;   // low-k bound (highest period of interest)
    int k_max_;   // high-k bound (lowest period of interest)

    // -----------------------------------------------------------------
    // Copy the ring buffer into a temporally ordered stack array.
    // (Defensive + cache-friendly — the SSA inner loop will iterate
    // over the contiguous data buffer with `alignas(64)` stream regularity.)
    // -----------------------------------------------------------------
    void unwind_buffer_(double* out) const {
        const int tail = HISTORY_LEN - buf_head_;
        std::memcpy(out,                buf_.data() + buf_head_, tail * sizeof(double));
        std::memcpy(out + tail,         buf_.data(),             buf_head_ * sizeof(double));
    }

    // -----------------------------------------------------------------
    // Heavy MEE math — runs once per tick AFTER warm-up.
    // -----------------------------------------------------------------
    void extract_cycle_() {
        alignas(64) std::array<double, HISTORY_LEN> data;
        unwind_buffer_(data.data());

        // ----- 1) Block-averaged autocorrelation (variance normalised) -----
        std::array<double, AR_ORDER + 1> autocorr;
        autocorr.fill(0.0);

        for (int b = 0; b < BLOCK_COUNT; ++b) {
            const double* block = data.data() + b * BLOCK_SIZE;

            double mean = 0.0;
            for (int i = 0; i < BLOCK_SIZE; ++i) mean += block[i];
            mean /= static_cast<double>(BLOCK_SIZE);

            alignas(64) std::array<double, BLOCK_SIZE> centered;
            for (int i = 0; i < BLOCK_SIZE; ++i) centered[i] = block[i] - mean;

            for (int lag = 0; lag <= AR_ORDER; ++lag) {
                double sum = 0.0;
                const int n = BLOCK_SIZE - lag;
                for (int i = 0; i < n; ++i) {
                    sum += centered[i + lag] * centered[i];
                }
                autocorr[lag] += sum;
            }
        }

        for (int k = 0; k <= AR_ORDER; ++k) {
            autocorr[k] /= static_cast<double>(BLOCK_COUNT);
        }

        if (autocorr[0] < SAFE_AUTOCORR_FLOOR || !std::isfinite(autocorr[0])) {
            return;  // zero-energy signal — leave dom_cycle unchanged
        }

        // Normalise by autocorr[0] so r[0] = 1 (Levinson-Durbin convention).
        const double inv_r0 = 1.0 / autocorr[0];
        for (int k = 0; k <= AR_ORDER; ++k) {
            autocorr[k] *= inv_r0;
        }

        // ----- 2) Levinson-Durbin recursion (p=3) (manually unrolled) -----
        std::array<double, AR_ORDER> ar;
        if (!levinson_durbin_(autocorr, ar)) {
            // Degenerate Toeplitz matrix — defer to previous dom_cycle.
            return;
        }
        if (!std::isfinite(ar[0]) || !std::isfinite(ar[1]) || !std::isfinite(ar[2])) {
            return;
        }

        // ----- 3,4) AR power spectrum, peak + parabolic refinement -----
        double raw_cycle = find_spectral_peak_(ar);

        // ----- 5) Rate-limit step (≤ MAX_CHANGE_RATIO per update) + EMA smoothing -----
        const double max_change = std::fabs(dom_cycle_) * MAX_CHANGE_RATIO;
        double       change     = raw_cycle - dom_cycle_;
        if (change >  max_change) raw_cycle = dom_cycle_ + max_change;
        if (change < -max_change) raw_cycle = dom_cycle_ - max_change;

        dom_cycle_ = dom_cycle_ * (1.0 - EMA_FAST) + raw_cycle * EMA_FAST;

        // ----- 6) Final clamp -----
        dom_cycle_ = std::max(dom_cycle_, lower_bound_);
        dom_cycle_ = std::min(dom_cycle_, upper_bound_);
    }

    // -----------------------------------------------------------------
    // Levinson-Durbin recursion — inline unrolled for fixed p = 3.
    //
    // Solves the symmetric Toeplitz system T(r[0..3])·a = r[1..3] where
    // a = [a1, a2, a3] are the AR(3) forward predictor coefficients.
    // Mathematically: the same recursion that Yule-Walker would solve,
    // but here obtained directly from the autocorrelation sequence with
    // < 9 multiplies and absolutely no inner loop.
    //
    // Returns false if T becomes degenerate (E < SAFE_LEVINSON_FLOOR) — the
    // caller must then leave dom_cycle unchanged.
    // -----------------------------------------------------------------
    static bool levinson_durbin_(const std::array<double, AR_ORDER + 1>& r,
                                 std::array<double, AR_ORDER>&            a) {
        // AR(1)
        const double k1 = r[1];            // reflection coeff 1
        a[0] = k1;
        double E = 1.0 - k1 * k1;          // r[0] = 1.0 after normalisation
        if (E < SAFE_LEVINSON_FLOOR) return false;

        // AR(2) (downreverse j = 1 only — mirror coeff at 0)
        const double k2 = (r[2] - a[0] * r[1]) / E;
        const double a1_prev = a[0];
        a[0] = a1_prev - k2 * a1_prev;     // j = 1 reversal term a_{2-1=1}^{1} = a[0]
        a[1] = k2;
        E   *= (1.0 - k2 * k2);
        if (E < SAFE_LEVINSON_FLOOR) return false;

        // AR(3)
        const double k3 = (r[3] - a[0] * r[2] - a[1] * r[1]) / E;
        const double a1_prev2 = a[0];
        const double a2_prev2 = a[1];
        a[0] = a1_prev2 - k3 * a2_prev2;   // j = 1 mirror: a_{3-1=2}^{2} = a[1]
        a[1] = a2_prev2 - k3 * a1_prev2;   // j = 2 mirror: a_{3-2=1}^{2} = a[0]
        a[2] = k3;

        return true;
    }

    // -----------------------------------------------------------------
    // Compute AR power spectrum at frequency bin index k.
    // Direct DFT — evaluates the AR transfer function directly
    //
    //    P(k) = 1 / |H(k)|²
    //
    // where H(k) = 1 − a1·z^-1 − a2·z^-2 − a3·z^-3 and z = exp(j·2π·k/N).
    // (k = absolute bin number from [k_min_, k_max_]; N = DFT_VIRTUAL_SIZE.)
    // These 6 trig evaluations per bin are the dominant cost on the hot path
    // — a single-call arithmetic load of ~30 ns, perfectly vectorisable.
    // -----------------------------------------------------------------
    static inline double ar_power_(const std::array<double, AR_ORDER>& ar, int k) {
        const double w = (2.0 * M_PI / static_cast<double>(DFT_VIRTUAL_SIZE)) * k;

        const double h_re = 1.0
            - ar[0] * std::cos(w)
            - ar[1] * std::cos(2.0 * w)
            - ar[2] * std::cos(3.0 * w);
        const double h_im =
              ar[0] * std::sin(w)
            + ar[1] * std::sin(2.0 * w)
            + ar[2] * std::sin(3.0 * w);

        const double mag_sq = h_re * h_re + h_im * h_im;
        if (mag_sq < SAFE_DFT_FLOOR || !std::isfinite(mag_sq)) return 0.0;
        return 1.0 / mag_sq;
    }

    // -----------------------------------------------------------------
    // Pass 1: linear sweep over the target bins to find the peak bin and
    //         total power. Pass 2: re-evaluate power at peak ± 1 to obtain
    //         neighbours for sub-bin parabolic interpolation.
    // -----------------------------------------------------------------
    double find_spectral_peak_(const std::array<double, AR_ORDER>& ar) {
        double max_power = -1.0;
        int    peak_bin  = -1;
        double peak_p    = 0.0;
        double total_power = 0.0;

        for (int k = k_min_; k <= k_max_; ++k) {
            const double p = ar_power_(ar, k);
            if (!std::isfinite(p)) continue;

            total_power += p;
            if (p > max_power) {
                max_power = p;
                peak_bin  = k;
                peak_p    = p;
            }
        }

        // ----- Spectral concentration ratio (peak / total) -----
        // Downstream Phase-2 pipeline may use this as a confidential signal
        // of how tightly identifiable the dominant cycle is.
        if (total_power < SAFE_DFT_FLOOR || !std::isfinite(total_power)) {
            spectral_conc_ = 0.5;    // uncertain → fall back to neutral a priori
        } else {
            spectral_conc_ = std::max(0.0, std::min(1.0, peak_p / total_power));
        }

        if (peak_bin <= 0 || !std::isfinite(max_power)) {
            // No convincing peak — leave dom_cycle alone.
            return dom_cycle_;
        }

        // ----- Parabolic sub-bin refit -----
        // 3-point parabola through {P(k-1), P(k), P(k+1)}. δ ∈ [-0.5, +0.5]
        //   δ = 0.5·(P_{k-1} − P_{k+1}) / (P_{k-1} − 2·P_k + P_{k+1})
        // Carefully reverted to 0 if either denominator is degenerate or the
        // boundary bin was the peak (clamp linearly).
        const double pL = (peak_bin > k_min_) ? ar_power_(ar, peak_bin - 1) : peak_p;
        const double pR = (peak_bin < k_max_) ? ar_power_(ar, peak_bin + 1) : peak_p;

        const double denom = pL - 2.0 * peak_p + pR;
        double delta = 0.0;
        if (std::fabs(denom) > SAFE_DFT_FLOOR) {
            delta = 0.5 * (pL - pR) / denom;
        }
        if (!std::isfinite(delta)) delta = 0.0;
        delta = std::max(-0.5, std::min(0.5, delta));

        const double peak_freq = (static_cast<double>(peak_bin) + delta)
                               / static_cast<double>(DFT_VIRTUAL_SIZE);
        if (peak_freq < 1e-12 || !std::isfinite(peak_freq)) {
            return dom_cycle_;
        }

        double raw_cycle = 1.0 / peak_freq;
        return std::max(lower_bound_, std::min(upper_bound_, raw_cycle));
    }
};

}  // namespace dsp

#endif  // DSP_MATH_HPP