/* k5_descrambler.cl — K5, descrambler (T3 port, P2-R7).
 *
 * Port grounding: lib/phy/upper/sequence_generators/pseudo_random_generator_impl.{h,cpp}
 * (apply_xor, init), pseudo_random_generator_sequence.h (the LFSR step recurrence),
 * pseudo_random_generator_initializers.h (Nc=1600 warm-up) — third_party/ocudu, BSD-3,
 * release_26_04. Ported the ALGORITHM (LFSR recurrence + Nc warm-up), not OCUDU's SIMD-batched
 * host code (SSE3/__aarch64__ branches in the "generic" impl — those are exactly the
 * vendor-width-specific paths P2-R2 bans; the underlying single-bit-step math is what's ported,
 * verified bit-exact against the real linked OCUDU library in tests/k5_test.cpp).
 *
 * Work-item = one LLR block (HLD §4 step 6). Each work-item does its own Nc=1600-step warm-up
 * from c_init (shared across all blocks of one codeword per D4) — redundant across work-items
 * sharing the same c_init, but correctness-only (SIM makes no performance claim, P2-R16) and
 * keeps this kernel's argument list exactly matching the parent LLD's committed prototype
 * (c_init, not a host-precomputed x1/x2 state).
 */
#include "oi_kernel_compat.h"

#define OI_K5_NC 1600

// Single-bit LFSR step (see k5_descrambler.cl's file header for port-grounding). Returns the
// output bit c(n) at bit 31 of the return value (matching OCUDU's own convention, where
// apply_xor tests `c & 0x80000000` to decide the sign flip).
inline uint oi_k5_step(uint* x1, uint* x2) {
  uint c = (*x1) ^ (*x2);
  uint f1 = (*x1) ^ ((*x1) << 3);
  uint f2 = (*x2) ^ ((*x2) << 1) ^ ((*x2) << 2) ^ ((*x2) << 3);
  uint mask = 0x80000000u;
  f1 = (f1 & mask) >> 30;
  f2 = (f2 & mask) >> 30;
  *x1 = ((*x1) << 1) ^ f1;
  *x2 = ((*x2) << 1) ^ f2;
  return c;
}

__kernel void k5_descramble(__global char* llr_inout, uint c_init, uint nof_llrs) {
  uint block = get_global_id(0);
  __global char* block_llr = llr_inout + (size_t)block * nof_llrs;

  // Nc=1600-step warm-up. x1's Nc-state is always the same (3GPP fixes x1(0)); x2's depends on
  // c_init via bit-reversal into the raw 31-bit state (see oi_p2_gold_init.cpp for the
  // equivalent host-side reference computation and its port-grounding comment).
  uint x1 = 0x80000000u;
  uint x2 = 0u;
  for (uint i = 0; i < OI_K5_NC; i++) {
    oi_k5_step(&x1, &x2);
  }
  // x1 is now warmed; discard and recompute x2 from c_init's own warm-up (independent branch,
  // matching OCUDU's table construction: x1 and x2 are warmed from DIFFERENT raw start states).
  uint x2_raw_start = 0;
  for (uint i = 0; i < 31; i++) {
    if ((c_init >> i) & 1u) {
      x2_raw_start |= (1u << (31 - i));
    }
  }
  uint x1b = 0u;
  uint x2b = x2_raw_start;
  for (uint i = 0; i < OI_K5_NC; i++) {
    oi_k5_step(&x1b, &x2b);
  }
  x2 = x2b;
  // x1 already holds its warmed value from the first loop (independent of c_init).

  for (uint n = 0; n < nof_llrs; n++) {
    uint c = oi_k5_step(&x1, &x2);
    if (c & 0x80000000u) {
      // Sign-flip: negate. Safe for the full int8 range LLR uses (±120 finite, ±127 LLR_INFTY —
      // never -128, so negation never overflows int8; P2-R7's "sign-flipped, never
      // reinterpreted" requirement for LLR_INFTY is satisfied by this being a plain negation).
      block_llr[n] = (char)(-(int)block_llr[n]);
    }
    // c==0 (bit31 clear): LLR passes through unchanged.
  }
}
