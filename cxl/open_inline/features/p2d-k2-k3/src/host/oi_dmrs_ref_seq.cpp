#include "oi_dmrs_ref_seq.h"

#include "../../../p2b-k5-k6/src/host/oi_p2_gold_init.h"

namespace {

constexpr float kAmplitude = 0.70710678118654752f;  // M_SQRT1_2 (dmrs_helper.cpp sequence_generation)

// Faithful copy of K5's step_once (oi_p2_gold_init.cpp) -- same LFSR, reused verbatim rather than
// re-derived, for the same transcription-risk reason documented there.
uint32_t step_once(uint32_t* x1, uint32_t* x2) {
  uint32_t c = (*x1) ^ (*x2);
  uint32_t f1 = (*x1) ^ ((*x1) << 3U);
  uint32_t f2 = (*x2) ^ ((*x2) << 1U) ^ ((*x2) << 2U) ^ ((*x2) << 3U);
  uint32_t mask = 0x80000000u;
  f1 = (f1 & mask) >> 30U;
  f2 = (f2 & mask) >> 30U;
  *x1 = ((*x1) << 1U) ^ f1;
  *x2 = ((*x2) << 1U) ^ f2;
  return c;
}

}  // namespace

extern "C" void oi_dmrs_ref_seq_generate(uint32_t nslot, uint32_t symbol, uint32_t n_id, uint32_t n_scid,
                                         oi_cf32* out, uint32_t nof_prb) {
  // dmrs_pusch_estimator_impl.cpp:95 (NORMAL CP -> nsymb=14; DMRS_REF_POINT_K_TO_POINT_A=0 and
  // MVP's full-band, PRB0-starting allocation means dmrs_helper.cpp's "skip preceding RBs" advance
  // is always 0 -- no bit-skip needed before this contiguous 51-PRB sequence).
  const uint32_t nsymb = 14;
  uint32_t c_init = ((nsymb * nslot + symbol + 1) * (2 * n_id + 1) * (1u << 17) + (2 * n_id + n_scid)) &
                    0x7FFFFFFFu;  // % 2^31

  oi_gold_state st = oi_gold_init(c_init);
  uint32_t x1 = st.x1, x2 = st.x2;

  uint32_t nof_pilots = nof_prb * OI_DMRS_PILOTS_PER_PRB;
  for (uint32_t p = 0; p < nof_pilots; p++) {
    uint32_t c_re = step_once(&x1, &x2);
    uint32_t c_im = step_once(&x1, &x2);
    out[p].re = (c_re & 0x80000000u) ? -kAmplitude : kAmplitude;
    out[p].im = (c_im & 0x80000000u) ? -kAmplitude : kAmplitude;
  }
}
