#include "oi_p2_gold_init.h"

namespace {

constexpr unsigned kNc = 1600;  // TS 38.211 §5.2.1 pseudo_random_generator_Nc

// Single-bit LFSR step, copied faithfully (step_size=1 specialization) from OCUDU's
// pseudo_random_generator_sequence::step(unsigned) — NOT re-derived by hand, to avoid
// transcription risk; bit-exactness is verified empirically against the real linked OCUDU
// library in tests/k5_test.cpp, not asserted by manual proof here.
uint32_t step_once(uint32_t* x1, uint32_t* x2) {
  uint32_t c = (*x1) ^ (*x2);
  uint32_t f1 = (*x1) ^ ((*x1) << 3U);
  uint32_t f2 = (*x2) ^ ((*x2) << 1U) ^ ((*x2) << 2U) ^ ((*x2) << 3U);
  uint32_t mask = 0x80000000u;  // ((1U<<1)-1U) << (32U-1U)
  f1 = (f1 & mask) >> 30U;      // >> (31U - 1U)
  f2 = (f2 & mask) >> 30U;
  *x1 = ((*x1) << 1U) ^ f1;
  *x2 = ((*x2) << 1U) ^ f2;
  return c;
}

uint32_t simulate_n_steps(uint32_t x1_start, uint32_t x2_start, unsigned n, uint32_t* out_x1,
                           uint32_t* out_x2) {
  uint32_t x1 = x1_start;
  uint32_t x2 = x2_start;
  for (unsigned i = 0; i < n; i++) {
    step_once(&x1, &x2);
  }
  *out_x1 = x1;
  *out_x2 = x2;
  return 0;
}

}  // namespace

extern "C" oi_gold_state oi_gold_init(uint32_t c_init) {
  oi_gold_state st{};

  // x1's Nc-advanced state is fixed (3GPP: x1(0) is always the same regardless of c_init),
  // computed from raw start 0x80000000 (bit-reversed representation of x1(0)=1, matching OCUDU's
  // pseudo_random_initializer_x1 table[0] construction for its default get_reverse(c_init=1)).
  uint32_t dummy_x2;
  {
    uint32_t x1_result, x2_result;
    simulate_n_steps(0x80000000u, 0u, kNc, &x1_result, &x2_result);
    st.x1 = x1_result;
    dummy_x2 = x2_result;  // discarded
  }
  (void)dummy_x2;

  // x2's Nc-advanced state depends on c_init: raw start is the bit-reversal of c_init's low 31
  // bits (bit i of c_init -> bit (31-i) of the raw state), matching the GF(2)-linear combination
  // OCUDU's pseudo_random_initializer_x2::get_reverse(c_init) computes via its table.
  uint32_t x2_raw_start = 0;
  for (unsigned i = 0; i < 31; i++) {
    if ((c_init >> i) & 1u) {
      x2_raw_start |= (1u << (31 - i));
    }
  }
  {
    uint32_t x1_result, x2_result;
    simulate_n_steps(0u, x2_raw_start, kNc, &x1_result, &x2_result);
    st.x2 = x2_result;
  }

  return st;
}
