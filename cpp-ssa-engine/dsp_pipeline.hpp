#ifndef DSP_PIPELINE_HPP
#define DSP_PIPELINE_HPP

// ============================================================================
// dsp_pipeline.hpp  —  Phase-2 C++ Pipeline (Sensor-Fused DSP Architecture)
// ----------------------------------------------------------------------------
// Wires Phase-1's dsp_math.hpp classes into a single FilterPipeline:
//
//   raw price
//      │
//      ▼
//   dsp::MesaStrategyMEE     → dom_cycle  ∈ [lower_bound, upper_bound]
//      │
//      ├──▶ dsp::DynamicUltimateSmoother (period = dom_cycle * MICRO_RATIO)
//      │     → micro_trend  (responsive, low-smooth trend line)
//      │
//      └──▶ dsp::DynamicUltimateSmoother (period = dom_cycle * MACRO_RATIO)
//            → macro_trend  (heavy-smooth long-horizon line — feeds the
//                             Phase-3 sensor-fusion lag-cancellation equation
//                             zero_lag_trend = macro_trend + K·sg_slope)
//
// Slope & acceleration are central finite differences on the micro_trend
// 3-sample history. The output PipelineResult is consumed 1:1 by main.cpp to
// populate the SsaFrame binary wire format.
//
// Hot-path cost: O(1) per tick (FilterPipeline::step is branch-free in steady
// state; MEE internally gates the ~30 µs DFT pass behind a 150-tick ring
// buffer flush). No heap allocations — all members are fixed-size PODs.
// ============================================================================

#include "dsp_math.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace dsp {

// ============================================================================
// PipelineResult — output struct of FilterPipeline::step
// ============================================================================
struct PipelineResult {
    double micro_trend;   // UltimateSmoother(dom_cycle × MICRO_RATIO)
    double macro_trend;   // UltimateSmoother(dom_cycle × MACRO_RATIO)
    double slope;         // Central 1st difference of micro_trend
    double accel;         // Central 2nd difference of micro_trend
    double dom_cycle;     // Current MEE dominant cycle (ticks)
    double spectral_conc; // MEE spectral concentration [0,1]
    bool   valid;         // false until warm-up complete
};

// ----------------------------------------------------------------------------
// FilterPipeline — Phase-2 wiring of MesaStrategyMEE + dual UltimateSmoothers.
// ----------------------------------------------------------------------------
class FilterPipeline {
public:
    static constexpr double MICRO_RATIO = 1.0;   // period_micro = dom_cycle
    static constexpr double MACRO_RATIO = 50.0;  // period_macro = dom_cycle × 50

    explicit FilterPipeline(double lower_bound = 8.0, double upper_bound = 330.0)
        : mee_(lower_bound, upper_bound),
          dom_cycle_(20.0),
          spectral_conc_(0.5),
          bar_count_(0),
          warmed_(false)
    {
        std::memset(micro_hist_, 0, sizeof(micro_hist_));
        std::memset(macro_hist_, 0, sizeof(macro_hist_));
    }

    // ------------------------------------------------------------------
    // Warm up the pipeline by replaying historical ticks. Same pattern as
    // the legacy DspPipelineController::warm_up — call once at boot, then
    // switch to step() for live ticks.
    // ------------------------------------------------------------------
    template <typename TickIter>
    void warm_up(TickIter begin, TickIter end) {
        for (auto it = begin; it != end; ++it) {
            step(static_cast<double>(*it));
        }
        warmed_ = (bar_count_ >= dsp::MesaStrategyMEE::HISTORY_LEN);
    }

    // ------------------------------------------------------------------
    // Per-tick step function — O(1) once the MEE buffer is full.
    // ------------------------------------------------------------------
    PipelineResult step(double price) {
        ++bar_count_;

        if (!std::isfinite(price)) {
            // Refuse pathological ticks; keep emitting the previous trend.
            return last_result_();
        }

        // 1) Dominant cycle extraction (MEE: AR(3) + Direct DFT).
        dom_cycle_ = mee_.update(price);
        spectral_conc_ = mee_.spectral_concentration();

        // 2) Micro trend — responsive, follows cyclical structure.
        const double period_micro = std::max(dom_cycle_ * MICRO_RATIO, 4.0);
        const double micro_trend  = micro_smoother_.update(price, period_micro);

        // 3) Macro trend — heavy smoothing for the big-picture regime.
        const double period_macro = std::max(dom_cycle_ * MACRO_RATIO, 4.0);
        const double macro_trend  = macro_smoother_.update(price, period_macro);

        // 4) Finite differences on the 3-sample micro history.
        //    [0]=n-2, [1]=n-1, [2]=n  → slope & accel of the *micro* trend.
        micro_hist_[0] = micro_hist_[1];
        micro_hist_[1] = micro_hist_[2];
        micro_hist_[2] = micro_trend;

        macro_hist_[0] = macro_hist_[1];
        macro_hist_[1] = macro_hist_[2];
        macro_hist_[2] = macro_trend;

        PipelineResult out{};
        out.dom_cycle     = dom_cycle_;
        out.spectral_conc = spectral_conc_;
        out.micro_trend   = micro_trend;
        out.macro_trend   = macro_trend;

        if (bar_count_ >= 3) {
            out.slope = (micro_hist_[2] - micro_hist_[0]) * 0.5;
            out.accel = micro_hist_[2] - 2.0 * micro_hist_[1] + micro_hist_[0];
        } else {
            out.slope = 0.0;
            out.accel = 0.0;
        }

        // Sanity guard — if any smoother ever explodes, collapse to price.
        if (!std::isfinite(out.micro_trend) ||
            std::abs(out.micro_trend - price) > 10.0 * std::max(std::abs(price), 1.0)) {
            out.micro_trend = price;
            out.slope       = 0.0;
            out.accel       = 0.0;
        }
        if (!std::isfinite(out.macro_trend) ||
            std::abs(out.macro_trend - price) > 10.0 * std::max(std::abs(price), 1.0)) {
            out.macro_trend = price;
        }

        out.valid = (bar_count_ >= dsp::MesaStrategyMEE::HISTORY_LEN);
        if (!warmed_ && out.valid) warmed_ = true;

        last_ = out;   // cache for pathological-tick fallbacks
        return out;
    }

    // ---------------- Diagnostics for main.cpp's logging ------------------
    bool   is_warmed()   const { return warmed_; }
    double dom_cycle()   const { return dom_cycle_; }
    double spectral_conc() const { return spectral_conc_; }
    int    fast_period() const { return static_cast<int>(dom_cycle_ * MICRO_RATIO); }
    int    slow_period() const { return static_cast<int>(dom_cycle_ * MACRO_RATIO); }

    void reset() {
        mee_.reset();
        micro_smoother_.reset();
        macro_smoother_.reset();
        std::memset(micro_hist_, 0, sizeof(micro_hist_));
        std::memset(macro_hist_, 0, sizeof(macro_hist_));
        dom_cycle_     = 20.0;
        spectral_conc_ = 0.5;
        bar_count_     = 0;
        warmed_        = false;
        last_          = PipelineResult{};
    }

private:
    dsp::MesaStrategyMEE       mee_;
    dsp::DynamicUltimateSmoother micro_smoother_;
    dsp::DynamicUltimateSmoother macro_smoother_;

    double dom_cycle_;
    double spectral_conc_;

    double micro_hist_[3];   // [n-2, n-1, n] of micro_trend
    double macro_hist_[3];   // [n-2, n-1, n] of macro_trend
    int    bar_count_;
    bool   warmed_;
    PipelineResult last_;    // last valid output (for NaN-input recoil)

    PipelineResult last_result_() const {
        PipelineResult out = last_;
        out.valid = warmed_;
        return out;
    }
};

}  // namespace dsp

#endif  // DSP_PIPELINE_HPP