#ifndef SSA_CORE_HPP
#define SSA_CORE_HPP

#include "ssa_types.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

// ============================================================================
// Dimension Constants
// ============================================================================

static constexpr int SSA_MAX_L = 128;   // >= L_max(121), power of 2
static constexpr int SSA_MAX_K = 512;   // >= buffer_capacity - L_min + 1
static constexpr int SSA_MAX_R = 4;     // maximum SVD rank

// ============================================================================
// TrajectoryMatrix — Virtual L×K Hankel matrix over CircularPriceBuffer
//
// No explicit matrix storage. Column j is samples [offset+j .. offset+j+L-1]
// from the circular buffer. O(1) logical shift when a new price arrives.
// ============================================================================

struct ShiftResult {
    const double* col_removed;  // pointer to internal buffer (length L)
    const double* col_added;    // pointer to internal buffer (length L)
};

class TrajectoryMatrix {
public:
    TrajectoryMatrix() : buf_(nullptr), L_(0), K_(0), offset_(0) {
        std::memset(col_removed_, 0, sizeof(col_removed_));
        std::memset(col_added_, 0, sizeof(col_added_));
    }

    void init(const CircularPriceBuffer* buf, int L, int N) {
        buf_ = buf;
        L_ = L;
        K_ = N - L + 1;
        offset_ = buf->size() - N;
    }

    // Extract column j into caller-provided buffer (length >= L)
    void column(int j, double* out) const {
        const int base = offset_ + j;
        for (int i = 0; i < L_; ++i) {
            out[i] = (*buf_)[base + i];
        }
    }

    // Shift the window forward by one sample.
    // Precondition: a new price has already been pushed to the buffer.
    // The buffer's circular indexing shifts automatically with each push,
    // so offset_ stays fixed. The removed column (old column 0) is at
    // offset_-1 in post-push indexing; the added column (new last column)
    // is at offset_+K-1.
    ShiftResult shift() {
        // Removed column: old column 0, now at offset_-1 after buffer shift
        const int rm_base = offset_ - 1;
        for (int i = 0; i < L_; ++i) {
            col_removed_[i] = (*buf_)[rm_base + i];
        }

        // Added column: new rightmost column at offset_+K-1
        const int add_base = offset_ + K_ - 1;
        for (int i = 0; i < L_; ++i) {
            col_added_[i] = (*buf_)[add_base + i];
        }

        return { col_removed_, col_added_ };
    }

    // Rebuild dimensions when L changes. Called rarely (hysteresis-gated).
    void resize(int new_L, int N) {
        L_ = new_L;
        K_ = N - new_L + 1;
        offset_ = buf_->size() - N;
    }

    int L() const { return L_; }
    int K() const { return K_; }
    int N() const { return L_ + K_ - 1; }
    int offset() const { return offset_; }
    const CircularPriceBuffer* buffer() const { return buf_; }

private:
    const CircularPriceBuffer* buf_;
    int L_;
    int K_;
    int offset_;

    alignas(64) double col_removed_[SSA_MAX_L];
    alignas(64) double col_added_[SSA_MAX_L];
};

// ============================================================================
// Diagonal Averaging (Hankelization) — Tail Variant (hot path)
//
// Reconstructs only the last `tail_len` points of the signal from the rank-r
// approximation X_r = U * diag(sigma) * V^T, via anti-diagonal averaging.
//
// For the rightmost anti-diagonals, d_t is small (1,2,3,...), making this
// O(tail_len^2 * r) — trivially fast for tail_len <= 5.
//
// U stored column-major: U[k*max_L + i], V stored column-major: V[k*max_K + j]
// ============================================================================

inline void diagonal_average_tail(
    const double* U, const double* V, const double* sigma,
    int L, int K, int r, int tail_len, double* out)
{
    const int N = L + K - 1;

    for (int m = 0; m < tail_len; ++m) {
        const int t = N - tail_len + m;  // anti-diagonal index
        const int i_min = (t >= K) ? (t - K + 1) : 0;
        const int i_max = (t < L - 1) ? t : (L - 1);
        const int d_t = i_max - i_min + 1;

        double sum = 0.0;
        for (int i = i_min; i <= i_max; ++i) {
            const int j = t - i;
            double elem = 0.0;
            for (int k = 0; k < r; ++k) {
                elem += sigma[k] * U[k * SSA_MAX_L + i] * V[k * SSA_MAX_K + j];
            }
            sum += elem;
        }
        out[m] = std::isfinite(sum) ? (sum / static_cast<double>(d_t)) : 0.0;
    }
}

// ============================================================================
// Diagonal Averaging — Full reconstruction (cold path: warm-up, benchmarks)
//
// Reconstructs the entire N-point signal. Complexity O(L*K*r).
// ============================================================================

inline void diagonal_average_full(
    const double* U, const double* V, const double* sigma,
    int L, int K, int r, double* out)
{
    const int N = L + K - 1;

    for (int t = 0; t < N; ++t) {
        const int i_min = (t >= K) ? (t - K + 1) : 0;
        const int i_max = (t < L - 1) ? t : (L - 1);
        const int d_t = i_max - i_min + 1;

        double sum = 0.0;
        for (int i = i_min; i <= i_max; ++i) {
            const int j = t - i;
            for (int k = 0; k < r; ++k) {
                sum += sigma[k] * U[k * SSA_MAX_L + i] * V[k * SSA_MAX_K + j];
            }
        }
        out[t] = sum / static_cast<double>(d_t);
    }
}

#endif // SSA_CORE_HPP
