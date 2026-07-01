/**
 * Adaptive Savitzky-Golay Filter for HFT Streaming Data
 *
 * Architecture:
 *   - Circular buffer ingestion: O(1) per tick
 *   - Precomputed causal SG coefficients for window sizes [7..127]
 *   - Simultaneous 0th/1st/2nd derivative output from single dot-product pass
 *   - Adaptive window: shrinks in trending regimes, expands in noisy/sideways
 *   - Zero-lag via right-edge (causal) polynomial evaluation
 *   - No repainting: each output is final once emitted
 *
 * The causal SG evaluates a polynomial fitted to the last N points at x=0
 * (the newest sample). This eliminates the group delay inherent in symmetric
 * SG while preserving the polynomial smoothing properties.
 *
 * Coefficient derivation:
 *   V[i][j] = x_i^j,  x_i = i - (N-1),  i=0..N-1,  j=0..P
 *   J = (V^T V)^{-1} V^T
 *   Row k of J = convolution weights for k-th derivative at x=0
 *
 * Baseline reference: https://github.com/Tugbars/Savitzky-Golay-Filter
 * Adapted for causal streaming with precomputed coefficient tables.
 *
 * License: MIT
 */

#ifndef ADAPTIVE_SG_FILTER_HPP
#define ADAPTIVE_SG_FILTER_HPP

#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>
#include <stdexcept>

namespace sg {

static constexpr int32_t SG_MIN_WINDOW   = 7;
static constexpr int32_t SG_MAX_WINDOW   = 127;
static constexpr int32_t SG_MAX_POLY     = 5;
static constexpr int32_t SG_BUFFER_CAP   = 256;

// ============================================================================
// SG Output — value + derivatives from a single filter pass
// ============================================================================

struct SgResult {
    double smoothed;     // 0th derivative: SG-filtered value
    double slope;        // 1st derivative: instantaneous rate of change
    double acceleration; // 2nd derivative: curvature
    int32_t window;      // current adaptive window size used
    int32_t poly_order;  // current polynomial order used
};

// ============================================================================
// Precomputed Coefficient Set for one (window, poly_order) pair
// ============================================================================

struct SgCoeffSet {
    int32_t window;
    int32_t poly_order;
    std::vector<double> c0;  // smoothing (0th deriv) coefficients [window]
    std::vector<double> c1;  // 1st derivative coefficients [window]
    std::vector<double> c2;  // 2nd derivative coefficients [window]
};

// ============================================================================
// Small-matrix LU solver for coefficient computation (P <= 6, so size <= 6x6)
// ============================================================================

namespace detail {

inline bool lu_solve(std::vector<double>& A, std::vector<double>& B,
                     int32_t n, int32_t nrhs)
{
    std::vector<int32_t> piv(n);
    for (int32_t i = 0; i < n; ++i) piv[i] = i;

    for (int32_t k = 0; k < n; ++k) {
        int32_t max_row = k;
        double max_val = std::abs(A[k * n + k]);
        for (int32_t i = k + 1; i < n; ++i) {
            double v = std::abs(A[i * n + k]);
            if (v > max_val) { max_val = v; max_row = i; }
        }
        if (max_val < 1e-14) return false;

        if (max_row != k) {
            std::swap(piv[k], piv[max_row]);
            for (int32_t j = 0; j < n; ++j)
                std::swap(A[k * n + j], A[max_row * n + j]);
            for (int32_t j = 0; j < nrhs; ++j)
                std::swap(B[k * nrhs + j], B[max_row * nrhs + j]);
        }

        for (int32_t i = k + 1; i < n; ++i) {
            double factor = A[i * n + k] / A[k * n + k];
            A[i * n + k] = factor;
            for (int32_t j = k + 1; j < n; ++j)
                A[i * n + j] -= factor * A[k * n + j];
            for (int32_t j = 0; j < nrhs; ++j)
                B[i * nrhs + j] -= factor * B[k * nrhs + j];
        }
    }

    for (int32_t k = n - 1; k >= 0; --k) {
        for (int32_t j = 0; j < nrhs; ++j) {
            for (int32_t i = k + 1; i < n; ++i)
                B[k * nrhs + j] -= A[k * n + i] * B[i * nrhs + j];
            B[k * nrhs + j] /= A[k * n + k];
        }
    }
    return true;
}

// Compute causal SG coefficients for a given window size and polynomial order.
// Causal: x_i = i - (N-1) for i=0..N-1, so newest sample is at x=0.
// Returns the k-th derivative coefficients (row k of (V^T V)^{-1} V^T).
inline SgCoeffSet compute_causal_coefficients(int32_t N, int32_t P)
{
    SgCoeffSet cs;
    cs.window = N;
    cs.poly_order = P;
    cs.c0.resize(N, 0.0);
    cs.c1.resize(N, 0.0);
    cs.c2.resize(N, 0.0);

    int32_t terms = P + 1;

    // Build V^T V (terms x terms) and V^T (terms x N)
    std::vector<double> VtV(terms * terms, 0.0);
    std::vector<double> Vt(terms * N, 0.0);

    for (int32_t i = 0; i < N; ++i) {
        double x = static_cast<double>(i - (N - 1));
        double xj = 1.0;
        for (int32_t j = 0; j < terms; ++j) {
            Vt[j * N + i] = xj;
            double xk = 1.0;
            for (int32_t k = 0; k < terms; ++k) {
                VtV[j * terms + k] += xj * xk;
                xk *= x;
            }
            xj *= x;
        }
    }

    // Solve VtV * J = Vt  →  J = (VtV)^{-1} Vt  (terms x N)
    // J is stored in Vt after solving
    if (!lu_solve(VtV, Vt, terms, N)) {
        // Fallback: identity coefficients (no filtering)
        cs.c0[N - 1] = 1.0;
        return cs;
    }

    // Row 0 of J = smoothing (0th derivative at x=0)
    // Row 1 of J = 1st derivative at x=0
    // Row 2 of J = 2nd derivative at x=0 (×2, since d²/dx² of a_2*x² = 2*a_2)
    for (int32_t i = 0; i < N; ++i) {
        cs.c0[i] = Vt[0 * N + i];
        if (terms > 1) cs.c1[i] = Vt[1 * N + i];
        if (terms > 2) cs.c2[i] = 2.0 * Vt[2 * N + i];
    }

    return cs;
}

} // namespace detail

// ============================================================================
// Coefficient Cache — precomputed for all supported (window, poly_order) pairs
// ============================================================================

class SgCoefficientCache {
public:
    SgCoefficientCache() {
        for (int32_t p = 2; p <= SG_MAX_POLY; ++p) {
            for (int32_t w = SG_MIN_WINDOW; w <= SG_MAX_WINDOW; w += 2) {
                if (w <= p + 1) continue;
                auto cs = detail::compute_causal_coefficients(w, p);
                int32_t key = encode_key(w, p);
                cache_[key] = std::move(cs);
            }
        }
    }

    const SgCoeffSet& get(int32_t window, int32_t poly_order) const {
        int32_t w = clamp_window(window);
        int32_t p = std::max(2, std::min(poly_order, SG_MAX_POLY));
        if (w <= p + 1) w = p + 3;
        w = clamp_window(w);

        int32_t key = encode_key(w, p);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;

        // Nearest available window
        for (int32_t dw = 2; dw <= SG_MAX_WINDOW; dw += 2) {
            int32_t w_up = clamp_window(w + dw);
            key = encode_key(w_up, p);
            it = cache_.find(key);
            if (it != cache_.end()) return it->second;

            int32_t w_dn = clamp_window(w - dw);
            key = encode_key(w_dn, p);
            it = cache_.find(key);
            if (it != cache_.end()) return it->second;
        }

        // Should never reach here; fallback to smallest valid
        key = encode_key(SG_MIN_WINDOW, 2);
        return cache_.at(key);
    }

private:
    static int32_t clamp_window(int32_t w) {
        w = std::max(SG_MIN_WINDOW, std::min(w, SG_MAX_WINDOW));
        if (w % 2 == 0) w += 1;
        return w;
    }

    static int32_t encode_key(int32_t window, int32_t poly) {
        return window * 16 + poly;
    }

    std::unordered_map<int32_t, SgCoeffSet> cache_;
};

// ============================================================================
// Circular Buffer — O(1) ingestion, ordered readout for dot product
// ============================================================================

class CircularBuffer {
public:
    CircularBuffer() : head_(0), count_(0) {
        std::memset(buf_, 0, sizeof(buf_));
    }

    void push(double value) {
        buf_[head_] = value;
        head_ = (head_ + 1) % SG_BUFFER_CAP;
        if (count_ < SG_BUFFER_CAP) ++count_;
    }

    int32_t size() const { return count_; }

    // Read element at logical index i (0 = oldest available, count-1 = newest)
    double operator[](int32_t i) const {
        int32_t idx = (head_ - count_ + i + SG_BUFFER_CAP) % SG_BUFFER_CAP;
        return buf_[idx];
    }

    // Read the last N elements into a contiguous output buffer (oldest first)
    void read_last_n(double* out, int32_t n) const {
        int32_t start = count_ - n;
        for (int32_t i = 0; i < n; ++i) {
            out[i] = (*this)[start + i];
        }
    }

    double newest() const {
        return buf_[(head_ - 1 + SG_BUFFER_CAP) % SG_BUFFER_CAP];
    }

private:
    double buf_[SG_BUFFER_CAP];
    int32_t head_;
    int32_t count_;
};

// ============================================================================
// Adaptive Savitzky-Golay Filter — main streaming processor
// ============================================================================

class AdaptiveSgFilter {
public:
    explicit AdaptiveSgFilter(const SgCoefficientCache& cache)
        : cache_(cache),
          current_window_(21),
          current_poly_(3),
          smoothed_window_(21.0),
          sideway_residual_var_(0.0),
          signal_var_(0.0)
    {}

    // Ingest a new price tick — O(1)
    void ingest(double price) {
        price_buf_.push(price);
    }

    // Compute the SG-filtered output using current adaptive parameters.
    // Call after ingest(). Returns false if insufficient data.
    bool compute(SgResult& out) {
        int32_t avail = price_buf_.size();
        if (avail < SG_MIN_WINDOW) return false;

        int32_t win = std::min(current_window_, avail);
        if (win % 2 == 0) win -= 1;
        win = std::max(SG_MIN_WINDOW, win);

        const SgCoeffSet& cs = cache_.get(win, current_poly_);
        int32_t N = cs.window;
        if (avail < N) {
            N = avail;
            if (N % 2 == 0) N -= 1;
            if (N < SG_MIN_WINDOW) return false;
        }

        // Dot product of coefficients with the last N samples from the circular buffer
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        int32_t buf_offset = avail - N;

        const double* c0 = cs.c0.data();
        const double* c1 = cs.c1.data();
        const double* c2 = cs.c2.data();

        for (int32_t i = 0; i < N; ++i) {
            double val = price_buf_[buf_offset + i];
            s0 += c0[i] * val;
            s1 += c1[i] * val;
            s2 += c2[i] * val;
        }

        // Update residual variance (for sideway score computation)
        double residual = price_buf_.newest() - s0;
        double alpha_var = 2.0 / (std::min(N, 50) + 1.0);
        sideway_residual_var_ = alpha_var * (residual * residual) + (1.0 - alpha_var) * sideway_residual_var_;

        double price_val = price_buf_.newest();
        double price_dev = price_val - s0;
        signal_var_ = alpha_var * (price_dev * price_dev) + (1.0 - alpha_var) * signal_var_;

        out.smoothed     = s0;
        out.slope        = s1;
        out.acceleration = s2;
        out.window       = N;
        out.poly_order   = current_poly_;

        return true;
    }

    // Compute the sideways score from SG residual analysis.
    // High residual variance relative to signal variance → sideways market.
    double compute_sideway_score() const {
        if (signal_var_ < 1e-12) return 1.0;
        double ratio = sideway_residual_var_ / signal_var_;
        return std::max(0.0, std::min(1.0, ratio));
    }

    // Adapt window size and polynomial order based on market regime signals.
    //   dom_cycle:    dominant market cycle period (from MESA/MEE)
    //   noise_ratio:  vress / price_scale — higher = noisier
    //   crisis_prob:  HMM crisis probability ∈ [0,1]
    //   sideway:      sideway score ∈ [0,1]
    void adapt(double dom_cycle, double noise_ratio, double crisis_prob, double sideway) {
        // Base window from dominant cycle: half-cycle captures one trend leg
        double base_window = std::max(7.0, dom_cycle * 0.5);

        // Widen in noisy/crisis/sideways conditions for more smoothing
        double noise_expand  = 1.0 + 2.0 * std::min(1.0, noise_ratio);
        double crisis_expand = 1.0 + 1.5 * crisis_prob;
        double side_expand   = 1.0 + 1.0 * sideway;

        double target_window = base_window * noise_expand * crisis_expand * side_expand;
        target_window = std::max(static_cast<double>(SG_MIN_WINDOW),
                        std::min(target_window, static_cast<double>(SG_MAX_WINDOW)));

        // Smooth the window adaptation to avoid oscillation
        smoothed_window_ = 0.05 * target_window + 0.95 * smoothed_window_;
        current_window_ = static_cast<int32_t>(std::round(smoothed_window_));
        if (current_window_ % 2 == 0) current_window_ += 1;

        // Polynomial order: lower in noisy/crisis for stability, higher in clean trends
        double quality = (1.0 - noise_ratio) * (1.0 - crisis_prob) * (1.0 - sideway);
        if (quality > 0.7)       current_poly_ = 4;
        else if (quality > 0.4)  current_poly_ = 3;
        else                     current_poly_ = 2;
    }

    int32_t window() const { return current_window_; }
    int32_t poly_order() const { return current_poly_; }

private:
    const SgCoefficientCache& cache_;
    CircularBuffer price_buf_;

    int32_t current_window_;
    int32_t current_poly_;
    double smoothed_window_;
    double sideway_residual_var_;
    double signal_var_;
};

} // namespace sg

#endif // ADAPTIVE_SG_FILTER_HPP
