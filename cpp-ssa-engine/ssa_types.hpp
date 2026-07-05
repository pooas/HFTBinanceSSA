#ifndef SSA_TYPES_HPP
#define SSA_TYPES_HPP

#include <cstdint>
#include <cstring>

// ============================================================================
// SSA Frame — 68-byte binary wire format published via ZMQ PUB :5558
//
// Little-endian layout consumed by Java ByteBuffer.getDouble() directly.
// Zero-copy, zero-GC on the subscriber side.
// ============================================================================

static constexpr uint32_t SSA_FRAME_MAGIC = 0x53534150; // "SSAP"
static constexpr size_t   SSA_FRAME_SIZE  = 68;

#pragma pack(push, 1)
struct SsaFrame {
    uint32_t magic;           // [0..3]   0x53534150
    uint64_t timestamp_ns;    // [4..11]  tick timestamp in nanoseconds
    double   ssa_smoothed;    // [12..19] blended composite signal S(t)
    double   ssa_slope;       // [20..27] blended 1st derivative
    double   ssa_accel;       // [28..35] blended 2nd derivative
    double   ssa_macro_trend; // [36..43] macro Ehlers trend (dom_cycle × MACRO_RATIO)
    int32_t  L_fast;          // [44..47] current fast window length
    int32_t  L_slow;          // [48..51] current slow window length
    float    blend_weight;    // [52..55] w(t) for diagnostics
    float    evr_fast;        // [56..59] explained variance ratio (fast)
    float    evr_slow;        // [60..63] explained variance ratio (slow)
    float    eigen_gap;       // [64..67] 1 - sigma2/sigma1
};
#pragma pack(pop)

static_assert(sizeof(SsaFrame) == SSA_FRAME_SIZE, "SsaFrame packing mismatch");

// ============================================================================
// Circular Price Buffer — fixed-capacity ring, O(1) push, ordered readout
//
// Capacity 512: accommodates L_max(121) + K_min + T_embed with margin.
// No heap allocations after construction.
// ============================================================================

static constexpr int32_t SSA_BUFFER_CAPACITY = 512;

class CircularPriceBuffer {
public:
    CircularPriceBuffer() : head_(0), count_(0) {
        std::memset(buf_, 0, sizeof(buf_));
    }

    void push(double price) {
        buf_[head_] = price;
        head_ = (head_ + 1) & (SSA_BUFFER_CAPACITY - 1); // power-of-2 modulo
        if (count_ < SSA_BUFFER_CAPACITY) ++count_;
    }

    int32_t size() const { return count_; }

    // Logical index access: 0 = oldest, count_-1 = newest
    double operator[](int32_t i) const {
        int32_t idx = (head_ - count_ + i + SSA_BUFFER_CAPACITY) & (SSA_BUFFER_CAPACITY - 1);
        return buf_[idx];
    }

    // Copy last N elements into contiguous output (oldest first)
    void read_last(int32_t n, double* out) const {
        int32_t start = count_ - n;
        for (int32_t i = 0; i < n; ++i) {
            out[i] = (*this)[start + i];
        }
    }

    double newest() const {
        return buf_[(head_ - 1 + SSA_BUFFER_CAPACITY) & (SSA_BUFFER_CAPACITY - 1)];
    }

private:
    alignas(64) double buf_[SSA_BUFFER_CAPACITY];
    int32_t head_;
    int32_t count_;
};

// ============================================================================
// SSA Pipeline Result — output of a single pipeline (fast or slow)
// ============================================================================

struct SsaResult {
    double smoothed;
    double slope;
    double accel;
    double evr;
    double eigen_gap;
};

#endif // SSA_TYPES_HPP
