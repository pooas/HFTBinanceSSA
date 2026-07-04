#ifndef SSA_PIPELINE_HPP
#define SSA_PIPELINE_HPP

#include "ssa_types.hpp"
#include "ssa_core.hpp"
#include "recursive_svd.hpp"

// ============================================================================
// SsaPipeline — One SSA processing channel (fast or slow)
//
// Owns a TrajectoryMatrix + RecursiveSVD pair. On each tick:
//   shift() → update() → diagonal_average_tail() → finite differences
//
// Hot-path step() is O(Lr + Kr + r^3) — sub-microsecond for r=2, L≤121.
// ============================================================================

class SsaPipeline {
public:
    SsaPipeline()
        : buf_(nullptr), L_(0), r_(0), warmed_(false) {}

    void configure(const CircularPriceBuffer* buf, int L, int r) {
        buf_ = buf;
        L_ = L;
        r_ = r;
        warmed_ = false;
    }

    // Cold-start: initialize trajectory matrix and SVD from buffered data.
    // Requires at least 2*L samples in the buffer for a meaningful decomposition.
    // Returns true if warm-up succeeded.
    bool warm_up() {
        if (!buf_) return false;

        const int avail = buf_->size();
        const int N_needed = 2 * L_;  // K >= L for meaningful SSA
        if (avail < N_needed) return false;

        const int N = avail;
        traj_.init(buf_, L_, N);
        svd_.init(traj_, r_);
        warmed_ = true;
        return true;
    }

    // Hot-path: process one new tick. Call after price_buffer.push(price).
    // Returns true if result is valid.
    bool step(SsaResult& out) {
        if (!warmed_) return false;

        // Shift trajectory matrix (drop oldest column, append newest)
        ShiftResult sr = traj_.shift();

        // Incremental SVD update
        svd_.update(sr.col_removed, sr.col_added);

        // Reconstruct last 5 points via diagonal averaging
        diagonal_average_tail(
            svd_.U(), svd_.V(), svd_.sigma(),
            svd_.L(), svd_.K(), svd_.r(),
            TAIL_LEN, tail_buf_);

        // Finite differences for slope and acceleration
        // tail_buf_[4] = newest, tail_buf_[0] = oldest of the 5
        out.smoothed = tail_buf_[4];
        out.slope    = (tail_buf_[4] - tail_buf_[2]) * 0.5;         // central difference
        out.accel    = tail_buf_[4] - 2.0 * tail_buf_[3] + tail_buf_[2]; // 2nd order backward
        out.evr      = svd_.evr();
        out.eigen_gap = svd_.eigen_gap();

        return true;
    }

    // Resize when AdaptiveL produces a new window length.
    // Expensive (~100 µs) — only called when hysteresis triggers (rare).
    void resize(int new_L) {
        if (!buf_) return;

        const int avail = buf_->size();
        const int N_needed = 2 * new_L;

        // Clamp to available data
        if (avail < N_needed) {
            new_L = avail / 2;
            if (new_L < 15) return; // L_min guard
        }

        L_ = new_L;
        const int N = avail;
        traj_.resize(new_L, N);
        svd_.recompute(traj_);
    }

    int L() const { return L_; }
    bool is_warmed() const { return warmed_; }

private:
    static constexpr int TAIL_LEN = 5;

    const CircularPriceBuffer* buf_;
    int L_;
    int r_;
    bool warmed_;

    TrajectoryMatrix traj_;
    RecursiveSVD     svd_;

    alignas(64) double tail_buf_[TAIL_LEN];
};

// ============================================================================
// PipelineController — Owns dual (fast + slow) SSA pipelines
//
// Drives both pipelines from the same shared CircularPriceBuffer on the same
// thread. No synchronization needed. Sequential execution guarantees
// deterministic output.
// ============================================================================

struct PipelineOutput {
    SsaResult fast;
    SsaResult slow;
    bool      valid;
};

class PipelineController {
public:
    PipelineController() : warmed_(false) {}

    // Configure both pipelines. Called once at startup after buffer is populated.
    void configure(const CircularPriceBuffer* buf, int L_fast, int L_slow, int rank) {
        pipeline_fast_.configure(buf, L_fast, rank);
        pipeline_slow_.configure(buf, L_slow, rank);
    }

    // Warm up both pipelines from historical data.
    bool warm_up() {
        bool ok_fast = pipeline_fast_.warm_up();
        bool ok_slow = pipeline_slow_.warm_up();
        warmed_ = ok_fast && ok_slow;
        return warmed_;
    }

    // Process one tick through both pipelines. Call after buffer.push(price).
    PipelineOutput step() {
        PipelineOutput out{};
        if (!warmed_) {
            out.valid = false;
            return out;
        }

        bool ok_fast = pipeline_fast_.step(out.fast);
        bool ok_slow = pipeline_slow_.step(out.slow);
        out.valid = ok_fast && ok_slow;

        return out;
    }

    // Resize both pipelines when AdaptiveL produces new window lengths.
    // Called rarely (hysteresis-gated, ~1-2x per minute max).
    void resize(int L_fast, int L_slow) {
        pipeline_fast_.resize(L_fast);
        pipeline_slow_.resize(L_slow);
    }

    bool is_warmed() const { return warmed_; }
    int L_fast() const { return pipeline_fast_.L(); }
    int L_slow() const { return pipeline_slow_.L(); }

private:
    SsaPipeline pipeline_fast_;
    SsaPipeline pipeline_slow_;
    bool warmed_;
};

#endif // SSA_PIPELINE_HPP
