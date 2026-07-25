/* oi_p2_gold_init.h — TS 38.211 §5.2.1 Gold-sequence Nc-advanced state, for K5 (P2-R7).
 *
 * Port grounding: pseudo_random_generator_impl.cpp's init(c_init) + the underlying
 * pseudo_random_generator_sequence::step() recurrence + pseudo_random_generator_initializers.h
 * (third_party/ocudu, BSD-3, release_26_04). Ported the ALGORITHM (the LFSR recurrence and the
 * Nc=1600-step warm-up), not OCUDU's table-precomputation speed optimization (that trick exists
 * to answer "warmed-up state for ANY c_init quickly"; our MVP has exactly one c_init per codeword,
 * computed host-side once per HLD D4, so direct simulation is simpler and equally correct — the
 * two are mathematically identical by the GF(2)-linearity argument in OCUDU's own doc comment on
 * pseudo_random_initializer_x2).
 *
 * Runs on the HOST (CPU), once per codeword — NOT inside the K5 kernel. The kernel receives the
 * already-Nc-advanced (x1, x2) state and does simple, portable single-bit LFSR stepping from
 * there (see k5_descrambler.cl).
 */
#ifndef OI_P2_GOLD_INIT_H
#define OI_P2_GOLD_INIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t x1;
  uint32_t x2;
} oi_gold_state;

/// Computes the Nc=1600-advanced (x1, x2) state for the given c_init (TS 38.211 §5.2.1,
/// c_init = RNTI * 2^15 + n_ID per §6.3.1.1 — computing c_init itself is the caller's job, e.g.
/// K5's own doc comment / P2-R7). x1's Nc-state is always the same regardless of c_init (3GPP
/// fixes x1's own initial state); x2's depends on c_init.
oi_gold_state oi_gold_init(uint32_t c_init);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* OI_P2_GOLD_INIT_H */
