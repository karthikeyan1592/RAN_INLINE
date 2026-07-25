/* oi_p2_ldpc_decode.h — LDPC hookup: K6's cb_llr_out -> the existing, unmodified, bit-exact
 * BG1/BG2 OpenCL decoder (p0-rig-scaffold's prior work), for the CPU tail (P2-R9).
 *
 * Reuses p0-rig-scaffold/docker/gpu-phy/ldpc_suite/{ldpc_decode.cl,bg_tables.h} directly (relative
 * include/file-path reference, not a copy -- single source of truth, matching the reuse
 * convention already established for oi_p2_gold_init across p2b/p2d). Neither file is modified by
 * this hookup; per P0-R6/MODIFICATIONS.md, `ldpc_decode.cl` is our own unmodified prior work, and
 * `bg_tables.h` was regenerated 2026-07-23 from OCUDU's real BSD-3 API (see that file's own header
 * and MODIFICATIONS.md's "bg_tables.h provenance correction" for why and how).
 *
 * KNOWN, REQUIRED SIZE BRIDGE (not a P2-R9 violation -- see file header comment in
 * oi_p2_ldpc_decode.cpp): the decoder's `llr_input` argument is sized `n_vn_full*Z` (68*Z BG1 /
 * 52*Z BG2) -- it expects the 2*Z always-punctured VN0/VN1 columns to be present as neutral
 * (LLR=0) entries at the front. K6's `cb_llr_out` is sized `n_short*Z` (66*Z BG1 / 50*Z BG2) --
 * exactly the transmitted, non-punctured portion; it never carries those 2*Z entries at all (they
 * were never sent over the air). This hookup zero-pads that gap; P2-R9's "no re-quantization...
 * same int8 buffer, no copy-and-rescale" is about not touching the LLR *values* K6 produced, which
 * this preserves exactly -- the prepended zeros are new, protocol-mandated neutral entries for
 * bits that were never transmitted, not a rescaling of K6's real data.
 */
#ifndef OI_P2_LDPC_DECODE_H
#define OI_P2_LDPC_DECODE_H

#include <stdint.h>

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  cl_context ctx;
  cl_command_queue queue;
  cl_program program;
  cl_kernel kernel;
} oi_p2_ldpc_decoder;

/// Builds the (unmodified, prior-work) LDPC decode kernel once. `ldpc_decode_cl_path` is the
/// filesystem path to p0-rig-scaffold's ldpc_decode.cl (caller-supplied so this module makes no
/// assumption about relative working directory). Returns 0 on success.
int oi_p2_ldpc_decoder_init(oi_p2_ldpc_decoder* dec, cl_context ctx, cl_command_queue queue,
                            const char* ldpc_decode_cl_path);

void oi_p2_ldpc_decoder_destroy(oi_p2_ldpc_decoder* dec);

/// Decodes ONE codeblock. `cb_llr` is K6's raw output for this CB: `n_short * lifting_size` int8
/// LLRs (n_short = 66 for BG1, 50 for BG2) -- the transmitted-only length, NOT yet padded with the
/// 2*lifting_size always-punctured columns (this function does that internally). `out_bits` must
/// have room for ceil(n_vn_info*lifting_size / 8) bytes (n_vn_info = 22 for BG1, 10 for BG2),
/// packed MSB-first -- these are the decoded CB's info+CRC+filler bits (`segment_length` bits from
/// oi_p2_cb_segment.h), ready for oi_p2_cb_desegment(). Returns 0 on success, nonzero on an
/// OpenCL error (base_graph/lifting_size combinations outside TS 38.212's standard 8 sets are a
/// caller bug, not handled here).
int oi_p2_ldpc_decode_cb(oi_p2_ldpc_decoder* dec, const int8_t* cb_llr, uint32_t base_graph,
                         uint32_t lifting_size, uint32_t n_iter, uint8_t* out_bits);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* OI_P2_LDPC_DECODE_H */
