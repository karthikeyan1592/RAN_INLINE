/* oi_dmrs_ref_seq.h — DMRS type1 reference sequence generation (TS 38.211 §6.4.1.1.1/.2), for K2a.
 *
 * Port grounding: lib/phy/upper/signal_processors/pusch/dmrs_pusch_estimator_impl.cpp's
 * sequence_generation() (c_init formula) + lib/phy/upper/signal_processors/dmrs_helper.cpp's
 * dmrs_sequence_generate() (amplitude M_SQRT1_2, one Gold-sequence bit pair per pilot RE) +
 * lib/phy/upper/sequence_generators/pseudo_random_generator_impl.cpp's generate(span<cf_t>,float)
 * (confirmed via its header-inline span<cf_t> overload: reinterprets the complex buffer as a flat
 * 2N-float array and maps EACH bit independently via sign-flip-a-constant -- bit=0 -> +amplitude,
 * bit=1 -> -amplitude, i.e. real(seq[n]) from bit c(2n), imag(seq[n]) from bit c(2n+1)). This is
 * the SAME Gold-sequence generator as K5 (p2b-k5-k6/src/host/oi_p2_gold_init.h) -- reused directly
 * rather than re-implemented, differing only in what the bit stream is mapped to (a constant QPSK
 * amplitude here, vs sign-flipping an input LLR in K5).
 *
 * Runs on the HOST, once per DMRS symbol per slot -- matches K5's own precedent (Nc-warmup on
 * host, only the fully-portable single-bit stepping recurrence needs to run in a kernel at all,
 * and here the ENTIRE short 306-bit-pair sequence is cheap enough that there's no correctness or
 * architecture reason to push it on-device; the parent LLD's k2_chanest prototype already declares
 * dmrs_ref_seq as a plain kernel INPUT buffer, which this satisfies without amendment).
 */
#ifndef OI_DMRS_REF_SEQ_H
#define OI_DMRS_REF_SEQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float re;
  float im;
} oi_cf32;

/// Number of DMRS type1 pilot REs per PRB (one CDM group without data, comb-2 pattern): TS 38.211
/// Table 6.4.1.1.3-1, 6 pilot subcarriers per 12-subcarrier PRB.
#define OI_DMRS_PILOTS_PER_PRB 6u

/// Computes the DMRS type1, CDM-group-0, layer-0 reference sequence for one OFDM symbol, full
/// 51-PRB MVP band (306 pilot REs = 51 PRB * 6 pilots/PRB). `nslot` is the slot index within the
/// radio frame (`slot_point::slot_index()`); `symbol` is the OFDM symbol index (0-13, one of
/// {2,7,11} for MVP's DMRS positions); `n_id` is the DMRS scrambling ID (MVP: reuses n_ID=PCI=1,
/// SPEC "Fixed MVP configuration" -- no separate DMRS-specific scrambling ID is configured);
/// `n_scid` is 0 or 1 (MVP: 0, not signaled). `out` must have space for 306 oi_cf32 values.
void oi_dmrs_ref_seq_generate(uint32_t nslot, uint32_t symbol, uint32_t n_id, uint32_t n_scid,
                              oi_cf32* out, uint32_t nof_prb);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* OI_DMRS_REF_SEQ_H */
