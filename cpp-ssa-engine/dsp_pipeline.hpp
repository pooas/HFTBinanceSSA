#ifndef DSP_PIPELINE_HPP
#define DSP_PIPELINE_HPP

#include "ssa_types.hpp"
#include "ehlers_filters.hpp"
#include "mee_cycle_estimator.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

// ============================================================================
// DspPipeline — Single Ehlers smoother channel
//
// Wraps a DynamicUltimateSmoother with a period ratio (0.5 for fast, 1.0 for
// slow). Computes smoothed value, slope (central finite difference), and
// acceleration (2nd finite difference) from a 3-sample history.
//
// Replaces SsaPipeline. O(1) per tick, zero allocations.
// ============================================================================

class DspPipeline {
public:
    DspPipeline() : period_ratio_(1.0), bar_(0) {
        std::memset(hist_, 0, sizeof(hist_));
    }

    void set_ratio(double ratio) { period_ratio_ = ratio; }

    void step(double price, double dom_cycle, double spectral_conc, SsaResult& out) {
        ++bar_;

        double period = dom_cycle * period_ratio_;
        period = std::max(period, 4.0);

        const double smoothed = smoother_.update(price, period);

        hist_[0] = hist_[1];
        hist_[1] = hist_[2];
        hist_[2] = smoothed;

        out.smoothed  = smoothed;
        out.evr       = spectral_conc;
        out.eigen_gap = 0.0;

        if (bar_ >= 3) {
            out.slope = (hist_[2] - hist_[0]) * 0.5;
            out.accel = hist_[2] - 2.0 * hist_[1] + hist_[0];
        } else {
            out.slope = 0.0;
            out.accel = 0.0;
        }
    }

    void reset() {
        smoother_.reset();
        std::memset(hist_, 0, sizeof(hist_));
        bar_ = 0;
    }

private:
    DynamicUltimateSmoother smoother_;
    double hist_[3];     // smoothed history: [n-2, n-1, n]
    double period_ratio_;
    int    bar_;
};

// ============================================================================
// PipelineOutput — Dual-channel result, identical to old SSA pipeline output
// ============================================================================

struct PipelineOutput {
    SsaResult fast;
    SsaResult slow;
    bool      valid;
};

// ============================================================================
// DspPipelineController — Dual Ehlers pipeline driven by MEE cycle estimation
//
// Architecture:
//   price → MEE → dom_cycle
//   price → UltimateSmoother(dom_cycle × 0.5) → fast channel
//   price → UltimateSmoother(dom_cycle × 1.0) → slow channel
//   SoftBlender fuses fast/slow using HMM regime probabilities (in main.cpp)
//
// The fast channel responds to half-cycle oscillations (more responsive in
// crisis). The slow channel tracks the full dominant cycle (smoother in calm
// regimes). This mirrors the old SSA dual-pipeline but with guaranteed-stable
// IIR filters instead of SVD.
//
// Warm-up: first ~155 ticks prime the filters (MEE needs 150 samples for
// autocorrelation, UltimateSmoother needs 4 bars). step() returns
// valid=false during this period.
//
// Replaces PipelineController + AdaptiveL. No explicit warm_up()/resize()
// needed — the pipeline self-primes through normal step() calls.
// ============================================================================

class DspPipelineController {
public:
    static constexpr double FAST_RATIO = 0.5;
    static constexpr double SLOW_RATIO = 1.0;
    static constexpr int    MIN_WARMUP = 155;

    DspPipelineController()
        : buf_(nullptr), warmed_(false), bar_count_(0)
    {
        fast_.set_ratio(FAST_RATIO);
        slow_.set_ratio(SLOW_RATIO);
    }

    void configure(const CircularPriceBuffer* buf) {
        buf_ = buf;
    }

    bool warm_up() {
        if (!buf_) return false;
        const int n = buf_->size();
        if (n < MIN_WARMUP) return false;

        SsaResult df, ds;
        for (int i = 0; i < n; ++i) {
            const double price = (*buf_)[i];  // 0=oldest, n-1=newest
            internal_step(price);

            const double dc = mee_.dom_cycle();
            const double sc = mee_.spectral_concentration();
            fast_.step(price, dc, sc, df);
            slow_.step(price, dc, sc, ds);
        }

        warmed_ = (bar_count_ >= MIN_WARMUP);
        return warmed_;
    }

    PipelineOutput step(double price) {
        internal_step(price);

        PipelineOutput out{};
        if (bar_count_ < MIN_WARMUP) {
            out.valid = false;
            return out;
        }

        if (!warmed_) warmed_ = true;

        const double dc  = mee_.dom_cycle();
        const double sc  = mee_.spectral_concentration();

        fast_.step(price, dc, sc, out.fast);
        slow_.step(price, dc, sc, out.slow);

        sanity_check(out.fast, price);
        sanity_check(out.slow, price);

        out.valid = true;
        return out;
    }

    bool   is_warmed()   const { return warmed_; }
    double dom_cycle()   const { return mee_.dom_cycle(); }
    int    fast_period() const { return static_cast<int>(mee_.dom_cycle() * FAST_RATIO); }
    int    slow_period() const { return static_cast<int>(mee_.dom_cycle() * SLOW_RATIO); }

private:
    const CircularPriceBuffer* buf_;
    MEECycleEstimator          mee_;
    DspPipeline                fast_;
    DspPipeline                slow_;
    bool                       warmed_;
    int                        bar_count_;

    void internal_step(double price) {
        ++bar_count_;
        mee_.update(price);
    }

    static void sanity_check(SsaResult& r, double price) {
        if (!std::isfinite(r.smoothed) ||
            std::abs(r.smoothed - price) > 10.0 * std::max(std::abs(price), 1.0)) {
            r.smoothed = price;
            r.slope    = 0.0;
            r.accel    = 0.0;
        }
    }
};

#endif // DSP_PIPELINE_HPP
