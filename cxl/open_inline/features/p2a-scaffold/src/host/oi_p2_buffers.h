/* oi_p2_buffers.h — buffer pool: RE grid x2, chan-est, eq-out, LLR, CB-LLR (HLD §5).
 *
 * Rule (HLD §5): zero device allocations after setup. Every buffer is sized once, at
 * oi_p2_buffers_create time, from the MVP config's fixed dimensions (LLD §4.2-§4.6) — no
 * per-slot resize. Sizes here use the MVP config's maximum dimensions (e.g. Qm=6 for the LLR
 * buffer) so any of the three MVP MCS indices {4,13,21} fit without reallocation.
 */
#ifndef OI_P2_BUFFERS_H
#define OI_P2_BUFFERS_H

// Portability floor is OpenCL 1.2-class (HLD §9, D-rejected "OpenCL 2.x/3.0 features") — pin the
// target version explicitly so no 2.x/3.0-only API is reached for accidentally via a newer
// default (PoCL/vendor ICD header default varies; don't rely on it).
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstddef>

namespace oi_p2 {

// MVP-fixed dimensions (SPEC "Fixed MVP configuration"; LLD §4).
constexpr int kNofSymbols = 14;
constexpr int kNofSubcarriers = 612;         // 51 PRB * 12
constexpr int kNofDataSymbols = 11;          // 14 - 3 DMRS symbols {2,7,11}
constexpr int kNofDataRe = kNofDataSymbols * kNofSubcarriers;  // 6732 (LLD §4.3)
constexpr int kMaxQm = 6;                     // 64QAM, MCS index 21 (SPEC MCS set)
constexpr int kMaxLiftingSize = 384;          // largest 5G NR lifting size (TS 38.212 Table 5.3.2-1)
constexpr int kMaxCbLlrLen = 66 * kMaxLiftingSize;  // BG1's 66*Z_c > BG2's 50*Z_c (LLD §4.6)

// K2's per-DMRS-symbol intermediates (p2d-k2-k3's K2a/K2b split -- see that slice's
// VERIFICATION.md). One of these per DMRS symbol {2, 7, 11}; rewritten (not reallocated) each
// launch since the DMRS reference sequence depends on slot_id.
constexpr int kNofPilots = 306;  // 51 PRB * 6 pilots/PRB (DMRS type1, comb-2)

struct K2aBufferSet {
  cl_mem ref_seq = nullptr;         // input: float2[306], host-generated per slot (oi_dmrs_ref_seq)
  cl_mem fd_est = nullptr;          // output: float2[612], this symbol's full-band FD estimate
  cl_mem filtered_pilot = nullptr;  // output: float2[306], pre-frequency-interpolation (K2b needs this)
  cl_mem epre_partial = nullptr;    // output: float, this symbol's partial EPRE sum
};

// Double-buffered per slot (HLD §5 "RE grid x2"); index by (slot_id % 2).
struct SlotBufferSet {
  cl_mem re_grid = nullptr;         // I2: float2[14][612], per HLD §4.2 row-major symbol-then-SC
  cl_mem symbol_bitmap = nullptr;   // I2: uint32, 14-bit completeness mask
  K2aBufferSet k2a[3];               // one per DMRS symbol {2, 7, 11}, in that order
  cl_mem ch_est = nullptr;          // I3: float2[N_data_re]
  cl_mem noise_var = nullptr;       // I3: float, per-slot scalar
  cl_mem epre = nullptr;            // I3: float, per-slot scalar
  cl_mem eq_symbols = nullptr;      // I4: float2[N_data_re]
  cl_mem eq_noise_var = nullptr;    // I4: float[N_data_re]
  cl_mem llr = nullptr;             // I5: int8[N_data_re * kMaxQm] (sized for worst-case Qm)
  cl_mem cb_llr = nullptr;          // I6: int8[kMaxCbLlrLen * kMaxSegments], one full_length-sized
                                    // slice per codeblock (worst case: BG1/Z_c=384, 4 segments)
};

constexpr int kMaxSegments = 4;  // this MVP's max nof_segments across MCS {4,13,21} (see STATUS.md)

struct BufferPool {
  cl_context ctx = nullptr;
  SlotBufferSet slots[2];  // double-buffered (HLD §6 "overlap is an allowed optimization")
  // Packet arena + descriptor ring (I1) — owned by ingest_backend in the full architecture; p2a
  // allocates a host-visible placeholder here so the API is end-to-end testable standalone
  // (real SIM/PHYSICAL ingest_backend integration is p3/p6's job, not this slice's).
  cl_mem arena = nullptr;
  size_t arena_bytes = 0;
  cl_mem desc_ring = nullptr;
  size_t desc_ring_capacity = 0;  // number of oi_frame_desc-sized slots
};

/// Allocates every buffer in pool at its MVP-maximum size. Returns false (and leaves pool
/// zero-initialized) on any clCreateBuffer failure — caller propagates OI_P2_ERR_DEVICE.
bool oi_p2_buffers_create(cl_context ctx, size_t arena_bytes, size_t desc_ring_capacity,
                           BufferPool* out_pool);

void oi_p2_buffers_destroy(BufferPool* pool);

}  // namespace oi_p2

#endif /* OI_P2_BUFFERS_H */
