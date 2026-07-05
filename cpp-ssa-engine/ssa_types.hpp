#ifndef SSA_TYPES_HPP
#define SSA_TYPES_HPP

// ============================================================================
// ssa_types.hpp  —  Phase-2 Binary Wire Format
// ----------------------------------------------------------------------------
// 68-byte binary frame published via ZMQ PUB :5558 by the C++ DSP engine and
// consumed byte-for-byte by the Java HftRegimeDetection subscriber.
//
// STRICT INVARIANTS:
//   1. Total packed size MUST be exactly 68 bytes — Java pre-allocates
//      ByteBuffer.allocateDirect(68) and uses raw offset reads. Any size
//      drift would silently corrupt the subscriber / cause segfaults.
//   2. Field ORDER and SIZES are frozen for backward compatibility with the
//      existing Java reader — only their semantic meaning has been updated:
//        ssa_sideway  → ssa_macro_trend   (8-byte double at offset [36..43])
//        ssa_smoothed → micro_trend       (carries the Phase-2 FilterPipeline
//                                          micro output, retains old name for
//                                          wire compatibility — Java renames the
//                                          variable internally in Phase 3)
//   3. `#pragma pack(push, 1)` enforces byte alignment; the static_assert
//      below is the canonical check — a compile failure here means the wire
//      format has been broken and must NOT be deployed.
//
// All other code paths now live in dsp_math.hpp / dsp_pipeline.hpp; the
// ancillary CircularPriceBuffer / SsaResult classes from the previous
// architecture have been retired (the FilterPipeline owns its own ring buffer
// inside the dsp::MesaStrategyMEE member).
// ============================================================================

#include <cstdint>
#include <cstddef>

static constexpr uint32_t SSA_FRAME_MAGIC = 0x53534150; // "SSAP"
static constexpr size_t   SSA_FRAME_SIZE  = 68;

#pragma pack(push, 1)
struct SsaFrame {
    uint32_t magic;           // [0..3]    0x53534150 — wire integrity marker
    uint64_t timestamp_ns;    // [4..11]   tick timestamp in nanoseconds
    double   ssa_smoothed;    // [12..19]  micro_trend  (alias for wire compat)
    double   ssa_slope;       // [20..27]  micro_trend's 1st finite difference
    double   ssa_accel;       // [28..35]  micro_trend's 2nd finite difference
    double   ssa_macro_trend; // [36..43]  macro_trend  (dom_cycle × MACRO_RATIO)
    int32_t  L_fast;          // [44..47]  micro smoother period (diagnostic)
    int32_t  L_slow;          // [48..51]  macro smoother period (diagnostic)
    float    blend_weight;    // [52..55]  reserved diagnostic (0.0)
    float    evr_fast;        // [56..59]  reserved diagnostic (0.0)
    float    evr_slow;        // [60..63]  MEE spectral_concentration (diagnostic)
    float    eigen_gap;       // [64..67]  MEE power-ratio gap at peak (0.0)
};
#pragma pack(pop)

static_assert(sizeof(SsaFrame) == SSA_FRAME_SIZE,
              "SsaFrame packing mismatch — wire format corrupted! "
              "Bump Java's SSA_FRAME_SIZE to match, or contact the DSP team.");

#endif  // SSA_TYPES_HPP