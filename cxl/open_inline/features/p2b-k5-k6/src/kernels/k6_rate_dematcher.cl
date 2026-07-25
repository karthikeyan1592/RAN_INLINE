/* k6_rate_dematcher.cl — K6, rate-dematcher, single-shot (T3 port, P2-R8).
 *
 * Port grounding: lib/phy/upper/channel_coding/ldpc/ldpc_rate_dematcher_impl.{h,cpp} — the
 * GENERIC class only, never the avx2/avx512/neon variants (D3). third_party/ocudu, BSD-3,
 * release_26_04.
 *
 * MVP simplification (matches SPEC scope exactly, not an invented shortcut): the real
 * ldpc_rate_dematcher_impl::allot_llrs() supports HARQ soft-combining across retransmissions and
 * an arbitrary redundancy version (rv 0-3). This feature's scope is single-shot, new_data=true
 * always, rv=0 always (P2-R8) -- under those two conditions the real algorithm's control flow
 * collapses to exactly one thing: shift_k0 is always 0 (shift_factor_bg{1,2}[0] == 0, TS38.212
 * Table 5.4.2.1-2), "is_copy_mode" is always true (no combine_softbits branch ever executes), and
 * the HARQ circular-buffer wraparound never triggers (Nref capping is out of scope, HARQ
 * deferred, so buffer_length == full_length always, and one systematic+filler+parity pass
 * consumes exactly rm_length input bits). shift_k0 is kept as a kernel argument (matching the
 * parent LLD's committed prototype) even though its value is always 0 in this MVP.
 *
 * Deinterleave (TS38.212 Table 5.4.2.1-2's bit-interleaver, reverted here) is fused into the same
 * pass rather than materializing a separate intermediate buffer (OCUDU's real code writes an
 * "aux" buffer first, then allot_llrs reads from it) -- deinterleave_bits_Qm's forward mapping
 * out[K*j+i] = in[i*Qm+j] (K = E/Qm) is invertible per output position (given a virtual index q
 * into the deinterleaved sequence, i = q%K, j = q/K, source index = i*Qm+j), so each output
 * position's source byte in llr_in can be computed directly with no scratch memory needed.
 *
 * Work-item = one whole CB (same granularity as k5_descrambler.cl's "one LLR block"; the parent
 * LLD's "work-item = one output CB position" is read here as "the position of this CB in the CB
 * sequence," not "one bit position" -- flagged for confirmation alongside the LLD's other open
 * items, not asserted as unambiguous).
 *
 * KNOWN SCOPE LIMIT (found empirically in tests/k6_test.cpp, not yet confirmed against real MVP
 * TBS/RE-mapping numbers): this single pass only recovers rm_length <= full_length -
 * nof_filler_bits. The real ldpc_rate_dematcher_impl handles rm_length beyond that by wrapping
 * around the circular buffer and combine_softbits()-summing repeated positions -- that path is
 * unimplemented here (HARQ/repetition explicitly out of MVP scope, P2-R8). Whether the MVP's
 * fixed config (51 PRB, MCS {4,13,21}, rv=0) can ever actually produce an E that large for a
 * single CB has NOT been checked against the real TBS/RE-mapping arithmetic -- open item, to
 * be confirmed in p2f-integration when real TB sizes are wired end-to-end. If it turns out it
 * can, this kernel needs the wraparound pass added before p2f can claim P2-R8 met.
 */
#include "oi_kernel_compat.h"

#define OI_K6_LLR_INFTY 127

// Inverse of OCUDU's deinterleave_bits_Qm<Qm> (ldpc_rate_dematcher_impl.cpp): given a position in
// the virtual deinterleaved sequence, returns the corresponding byte from the real rate-matched
// input. modulation_order==1 (BPSK-tail path, real code's `if (modulation_order == 1)` branch)
// means no deinterleave at all -- direct passthrough.
inline char oi_k6_read_deinterleaved(__global const char* llr_in, uint rm_length,
                                     uint modulation_order, uint virtual_idx) {
  if (modulation_order <= 1) {
    return llr_in[virtual_idx];
  }
  uint E = rm_length;
  uint K = E / modulation_order;
  uint i = virtual_idx % K;
  uint j = virtual_idx / K;
  uint in_index = i * modulation_order + j;
  return llr_in[in_index];
}

__kernel void k6_rate_dematch(__global const char* llr_in, uint rm_length, uint full_length,
                              uint nof_filler_bits, uint shift_k0, uint modulation_order,
                              __global char* cb_llr_out) {
  uint cb = get_global_id(0);
  __global const char* my_in = llr_in + (size_t)cb * rm_length;
  __global char* my_out = cb_llr_out + (size_t)cb * full_length;

  uint buffer_length = full_length;  // Nref capping out of scope (HARQ deferred)

  // Derive base-graph K (info+parity columns) from full_length, same resolution order as
  // OCUDU's real code (try BG1's N_SHORT=66 divisor first, then BG2's N_SHORT=50).
  uint BG_K;
  uint lifting_size;
  if (full_length % 66 == 0) {
    BG_K = 22;
    lifting_size = full_length / 66;
  } else {
    BG_K = 10;
    lifting_size = full_length / 50;
  }
  uint nof_systematic_bits = (BG_K - 2) * lifting_size;
  uint nof_info_bits = nof_systematic_bits - nof_filler_bits;

  // Copy-mode always in MVP scope -> start from an all-zero CB (matches the real code's final
  // "zero the unfilled tail" step, generalized: everything not explicitly written below stays 0).
  for (uint k = 0; k < full_length; k++) {
    my_out[k] = 0;
  }

  uint tmp_idx = shift_k0;  // always 0 in MVP (rv=0); kept as a parameter per the LLD prototype
  uint virtual_idx = 0;
  uint remaining_in = rm_length;

  // Systematic region.
  if (tmp_idx < nof_info_bits) {
    uint nbits_systematic = min(nof_info_bits - tmp_idx, remaining_in);
    for (uint k = 0; k < nbits_systematic; k++) {
      my_out[tmp_idx + k] =
          oi_k6_read_deinterleaved(my_in, rm_length, modulation_order, virtual_idx + k);
    }
    tmp_idx += nbits_systematic;
    virtual_idx += nbits_systematic;
    remaining_in -= nbits_systematic;
  }

  // Filler bits: always +LLR_INFTY (127) in copy mode (P2-R8).
  for (uint k = 0; k < nof_filler_bits; k++) {
    my_out[nof_info_bits + k] = (char)OI_K6_LLR_INFTY;
  }

  if (tmp_idx < nof_systematic_bits) {
    tmp_idx = nof_systematic_bits;
  }

  // Parity region (single pass suffices: MVP scope guarantees rm_length bits are fully consumed
  // by systematic + parity without wraparound, since buffer_length == full_length and HARQ/Nref
  // capping that would require multi-pass combining is out of scope).
  uint nbits_parity = min(buffer_length - tmp_idx, remaining_in);
  for (uint k = 0; k < nbits_parity; k++) {
    my_out[tmp_idx + k] =
        oi_k6_read_deinterleaved(my_in, rm_length, modulation_order, virtual_idx + k);
  }
}
