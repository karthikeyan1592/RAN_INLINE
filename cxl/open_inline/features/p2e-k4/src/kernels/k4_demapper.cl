/* k4_demapper.cl — K4, soft demapper -> int8 LLR (T3 port; P2-R6).
 *
 * Port grounding: lib/phy/upper/channel_modulation/demodulation_mapper_impl.cpp (dispatch,
 * near_zero/noise_var>0 guard pattern), demodulation_mapper_qpsk.cpp (QPSK scalar path),
 * demodulation_mapper_qam16.cpp (16QAM scalar path, piecewise-linear bit01/bit23),
 * demodulation_mapper_qam64.cpp (64QAM scalar path, interval_function piecewise-linear tables for
 * bit01/23/45), log_likelihood_ratio.cpp's quantize() (clip to +-range_limit, round to nearest
 * int8 step = range_limit/LLR_MAX). third_party/ocudu, BSD-3, release_26_04. Ported the SCALAR
 * formulas only, never the AVX2/AVX512/NEON batched paths (P2-R2/D3).
 *
 * Work-item = one data RE, emits Qm LLR bits (HLD §4 step 5).
 */
#include "oi_kernel_compat.h"

#define OI_K4_LLR_MAX 120
#define OI_K4_NEAR_ZERO 1.0e-9f

inline char oi_k4_quantize(float value, float range_limit) {
  float clipped = value;
  if (fabs(value) > range_limit) {
    clipped = copysign(range_limit, value);
  }
  return (char)round(clipped / range_limit * (float)OI_K4_LLR_MAX);
}

// --- QPSK (Qm=2), demodulation_mapper_qpsk.cpp ---
inline char oi_k4_qpsk(float x, float noise_var) {
  if (!(noise_var > 0.0f)) {
    return 0;
  }
  float l_value = 2.0f * M_SQRT2_F * x / noise_var;
  return oi_k4_quantize(l_value, 24.0f);
}

// --- 16QAM (Qm=4), demodulation_mapper_qam16.cpp ---
#define OI_K4_SQRT1_10 0.31622776601683794f  // 1/sqrt(10)

inline char oi_k4_qam16_01(float x, float noise_var) {
  if (!(noise_var > 0.0f)) {
    return 0;
  }
  float l_value = 4.0f * OI_K4_SQRT1_10 * x;
  if (fabs(x) > 2.0f * OI_K4_SQRT1_10) {
    l_value = 2.0f * l_value - copysign(0.8f, x);
  }
  l_value /= noise_var;
  return oi_k4_quantize(l_value, 20.0f);
}

inline char oi_k4_qam16_23(float x, float noise_var) {
  if (!(noise_var > 0.0f)) {
    return 0;
  }
  float l_value = (0.8f - 4.0f * OI_K4_SQRT1_10 * fabs(x)) / noise_var;
  return oi_k4_quantize(l_value, 20.0f);
}

// --- 64QAM (Qm=6), demodulation_mapper_qam64.cpp: piecewise-linear interval_function ---
#define OI_K4_SQRT1_42 0.15430334996209194f  // 1/sqrt(42)

// OpenCL C requires __constant variables at program scope -- a non-kernel function may not
// declare one locally (found the hard way: PoCL rejected the first draft of this file with
// "non-kernel function variable cannot be declared in constant address space"). Hoisted here.
__constant float oi_k4_qam64_slope_01[8] = {16 * OI_K4_SQRT1_42, 12 * OI_K4_SQRT1_42, 8 * OI_K4_SQRT1_42,
                                            4 * OI_K4_SQRT1_42,  4 * OI_K4_SQRT1_42,  8 * OI_K4_SQRT1_42,
                                            12 * OI_K4_SQRT1_42, 16 * OI_K4_SQRT1_42};
__constant float oi_k4_qam64_intercept_01[8] = {24.0f / 21, 12.0f / 21, 4.0f / 21,   0.0f,
                                                0.0f,       -4.0f / 21, -12.0f / 21, -24.0f / 21};
__constant float oi_k4_qam64_slope_23[8] = {8 * OI_K4_SQRT1_42,  4 * OI_K4_SQRT1_42,  4 * OI_K4_SQRT1_42,
                                            8 * OI_K4_SQRT1_42,  -8 * OI_K4_SQRT1_42, -4 * OI_K4_SQRT1_42,
                                            -4 * OI_K4_SQRT1_42, -8 * OI_K4_SQRT1_42};
__constant float oi_k4_qam64_intercept_23[8] = {20.0f / 21, 8.0f / 21, 8.0f / 21, 12.0f / 21,
                                                12.0f / 21, 8.0f / 21, 8.0f / 21, 20.0f / 21};
__constant float oi_k4_qam64_slope_45[4] = {4 * OI_K4_SQRT1_42, -4 * OI_K4_SQRT1_42, 4 * OI_K4_SQRT1_42,
                                            -4 * OI_K4_SQRT1_42};
__constant float oi_k4_qam64_intercept_45[4] = {12.0f / 21, -4.0f / 21, -4.0f / 21, 12.0f / 21};

inline uint oi_k4_interval_idx(float value, float interval_width, uint nof_intervals) {
  int idx = (int)floor(value / interval_width) + (int)(nof_intervals / 2);
  int hi = (int)nof_intervals - 1;
  if (idx < 0) idx = 0;
  if (idx > hi) idx = hi;
  return (uint)idx;
}

inline char oi_k4_qam64_01(float x, float rcp_noise) {
  float width = 2.0f * OI_K4_SQRT1_42;
  uint idx = oi_k4_interval_idx(x, width, 8);
  float l_value = (oi_k4_qam64_slope_01[idx] * x + oi_k4_qam64_intercept_01[idx]) * rcp_noise;
  return oi_k4_quantize(l_value, 20.0f);
}

inline char oi_k4_qam64_23(float x, float rcp_noise) {
  float width = 2.0f * OI_K4_SQRT1_42;
  uint idx = oi_k4_interval_idx(x, width, 8);
  float l_value = (oi_k4_qam64_slope_23[idx] * x + oi_k4_qam64_intercept_23[idx]) * rcp_noise;
  return oi_k4_quantize(l_value, 20.0f);
}

inline char oi_k4_qam64_45(float x, float rcp_noise) {
  float width = 4.0f * OI_K4_SQRT1_42;
  uint idx = oi_k4_interval_idx(x, width, 4);
  float l_value = (oi_k4_qam64_slope_45[idx] * x + oi_k4_qam64_intercept_45[idx]) * rcp_noise;
  return oi_k4_quantize(l_value, 20.0f);
}

__kernel void k4_demap(__global const float2* eq_symbols, __global const float* eq_noise_var, uint qm,
                       __global char* llr_out) {
  uint re = get_global_id(0);
  float2 z = eq_symbols[re];
  float noise_var = eq_noise_var[re];
  __global char* out = llr_out + (size_t)re * qm;

  if (qm == 2) {
    out[0] = oi_k4_qpsk(z.x, noise_var);
    out[1] = oi_k4_qpsk(z.y, noise_var);
    return;
  }

  // 16QAM/64QAM: zero all Qm bits if the symbol itself is near zero (demodulation_mapper_qam16.cpp/
  // qam64.cpp's is_near_zero(*symbols_it) guard -- QPSK has no such guard in the real code).
  if (z.x * z.x + z.y * z.y < OI_K4_NEAR_ZERO) {
    for (uint b = 0; b < qm; b++) out[b] = 0;
    return;
  }

  if (qm == 4) {
    out[0] = oi_k4_qam16_01(z.x, noise_var);
    out[1] = oi_k4_qam16_01(z.y, noise_var);
    out[2] = oi_k4_qam16_23(z.x, noise_var);
    out[3] = oi_k4_qam16_23(z.y, noise_var);
  } else if (qm == 6) {
    float rcp_noise = (noise_var > 0.0f) ? (1.0f / noise_var) : 0.0f;
    out[0] = oi_k4_qam64_01(z.x, rcp_noise);
    out[1] = oi_k4_qam64_01(z.y, rcp_noise);
    out[2] = oi_k4_qam64_23(z.x, rcp_noise);
    out[3] = oi_k4_qam64_23(z.y, rcp_noise);
    out[4] = oi_k4_qam64_45(z.x, rcp_noise);
    out[5] = oi_k4_qam64_45(z.y, rcp_noise);
  }
}
