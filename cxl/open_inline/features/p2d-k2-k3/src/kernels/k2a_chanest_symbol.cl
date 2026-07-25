/* k2a_chanest_symbol.cl — K2a, per-DMRS-symbol LS estimate + FD smoothing + frequency
 * interpolation (T3 port; P2-R4, half of it -- K2b does the cross-symbol combine).
 *
 * Port grounding: lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp
 * (LS matched-filter, lines ~490-492; FD "filter" smoothing dispatch + virtual-pilot
 * extrapolation, port_channel_estimator_helpers.cpp compute_v_pilots/add_v_pilots/apply_fd_smoothing);
 * lib/phy/support/interpolator/interpolator_linear_impl.cpp (frequency linear interpolation).
 * third_party/ocudu, BSD-3, release_26_04.
 *
 * MVP fixes the FD-smoothing filter's shape completely (allocation is always 51 PRB, DMRS comb
 * stride is always 2 -- port_channel_estimator_helpers.cpp's filter_type clamps nof_rb to 3
 * internally regardless of the true 51, so the resampled filter is the SAME 15 taps for any
 * allocation >= 3 PRB at stride 2): the 15 coefficients below were derived by transcribing
 * filter_type's exact resampling/renormalization arithmetic (RC_FILTER, nof_rbs=min(51,3)=3,
 * stride=2 -- confirmed real via configure_interpolator on our comb pattern, offset=0/stride=2)
 * and are cross-validated against the real linked OCUDU dmrs_pusch_estimator in tests/k2_test.cpp,
 * not just hand-derived. filter_type also computes an unused "tail_correction" -- confirmed by
 * reading every caller that it is never actually applied anywhere in this code path (computed,
 * never read); faithfully NOT applying it here either, since faithful porting means matching what
 * OCUDU's shipped code actually does, not what an unused field suggests it might.
 *
 * Single work-item does the whole symbol (HLD §4 step 3: "work-group = one DM-RS symbol" -- read
 * here as one work-group's single work-item does all of that symbol's processing serially; no
 * performance claim, SIM tier, matches K5/K6's per-work-item redundancy precedent).
 */
#include "oi_kernel_compat.h"

#define OI_K2_NOF_PRB 51u
#define OI_K2_PILOTS_PER_PRB 6u
#define OI_K2_NOF_PILOTS (OI_K2_NOF_PRB * OI_K2_PILOTS_PER_PRB)  // 306
#define OI_K2_NOF_SUBCARRIERS 612u
#define OI_K2_NOF_V_PILOTS 7u
#define OI_K2_FILTER_LEN 15u
#define OI_K2_FILTER_MID 7u

// filter_type(nof_rbs=min(51,3)=3, stride=2) resampled + renormalized taps (see file header).
__constant float oi_k2_fd_filter[OI_K2_FILTER_LEN] = {
    -0.0391585909f, -0.0287990728f, 0.0000000000f, 0.0445245383f, 0.0970724536f,
    0.1467045930f,  0.1821531663f,  0.1950058248f, 0.1821531663f, 0.1467045930f,
    0.0970724536f,  0.0445245383f,  0.0000000000f, -0.0287990728f, -0.0391585909f};

inline float oi_k2_cabs(float2 z) { return sqrt(z.x * z.x + z.y * z.y); }

// Linear regression on (magnitude, unwrapped phase) over nof_v_pilots real pilots, extrapolating
// nof_v_pilots virtual pilots outward (port_channel_estimator_helpers.cpp compute_v_pilots).
// `arg_in` must already be unwrapped by the caller (matches ocuduvec::unwrap_arguments' contract).
void oi_k2_compute_v_pilots(float2* out, const float* abs_in, const float* arg_in, uint n, int is_start) {
  float mean_x = (float)(n * (n - 1)) / 2.0f / (float)n;
  float norm_x_sq = (float)((n - 1) * n * (2 * n - 1)) / 6.0f;

  float mean_abs = 0.0f, mean_arg = 0.0f;
  for (uint i = 0; i < n; i++) {
    mean_abs += abs_in[i];
    mean_arg += arg_in[i];
  }
  mean_abs /= (float)n;
  mean_arg /= (float)n;

  float slope_abs = 0.0f, slope_arg = 0.0f;
  for (uint i = 0; i < n; i++) {
    slope_abs += abs_in[i] * (float)i;
    slope_arg += arg_in[i] * (float)i;
  }
  slope_abs -= mean_x * mean_abs * (float)n;
  slope_abs /= (norm_x_sq - (float)n * mean_x * mean_x);
  float intercept_abs = mean_abs - slope_abs * mean_x;

  slope_arg -= mean_x * mean_arg * (float)n;
  slope_arg /= (norm_x_sq - (float)n * mean_x * mean_x);
  float intercept_arg = mean_arg - slope_arg * mean_x;

  int v_offset = is_start ? -(int)n : (int)n;
  for (uint i = 0; i < n; i++) {
    int i_virtual = (int)i + v_offset;
    float rho = slope_abs * (float)i_virtual + intercept_abs;
    float arg = slope_arg * (float)i_virtual + intercept_arg + ((rho > 0.0f) ? 0.0f : M_PI_F);
    float mag = fabs(rho);
    out[i] = (float2)(mag * cos(arg), mag * sin(arg));
  }
}

// Unwraps phase across a short sequence (matches ocuduvec::unwrap_arguments' effect: successive
// differences folded into [-pi, pi] before accumulating, so the regression sees a continuous
// phase ramp rather than +-pi wraps).
void oi_k2_unwrap(float* out_arg, const float2* in, uint n) {
  float prev = atan2(in[0].y, in[0].x);
  out_arg[0] = prev;
  float acc = prev;
  for (uint i = 1; i < n; i++) {
    float a = atan2(in[i].y, in[i].x);
    float d = a - prev;
    while (d > M_PI_F) d -= 2.0f * M_PI_F;
    while (d < -M_PI_F) d += 2.0f * M_PI_F;
    acc += d;
    out_arg[i] = acc;
    prev = a;
  }
}

__kernel void k2a_chanest_symbol(__global const float2* re_grid, __global const uint* symbol_bitmap,
                                 __global const float2* dmrs_ref_seq, uint dmrs_symbol_idx,
                                 float beta_scaling, __global float2* fd_est_out,
                                 __global float2* filtered_pilot_out, __global float* epre_partial_out) {
  float total_scaling = 1.0f / beta_scaling;  // td_interpolation_strategy=interpolate: no /nof_dmrs_symbols term

  // 1. LS estimate at pilot REs (matched filter: rx*conj(ref)), comb-0 stride-2 -> global
  // subcarrier index 2*p for pilot p (port_channel_estimator_average_impl.cpp:490-492).
  float2 pilots_lse[OI_K2_NOF_PILOTS];
  float epre_sum = 0.0f;
  for (uint p = 0; p < OI_K2_NOF_PILOTS; p++) {
    float2 rx = re_grid[dmrs_symbol_idx * OI_K2_NOF_SUBCARRIERS + 2u * p];
    epre_sum += rx.x * rx.x + rx.y * rx.y;
    float2 ref = dmrs_ref_seq[p];
    // rx * conj(ref)
    float2 ls = (float2)(rx.x * ref.x + rx.y * ref.y, rx.y * ref.x - rx.x * ref.y);
    pilots_lse[p] = ls * total_scaling;
  }
  *epre_partial_out = epre_sum;

  // 2. Virtual pilots (7 each side) via magnitude/phase linear regression, extrapolated outward.
  float2 enlarged[OI_K2_NOF_PILOTS + 2u * OI_K2_NOF_V_PILOTS];
  {
    float abs_buf[OI_K2_NOF_V_PILOTS];
    float arg_buf[OI_K2_NOF_V_PILOTS];
    float2 v_pilots[OI_K2_NOF_V_PILOTS];

    for (uint i = 0; i < OI_K2_NOF_V_PILOTS; i++) abs_buf[i] = oi_k2_cabs(pilots_lse[i]);
    oi_k2_unwrap(arg_buf, pilots_lse, OI_K2_NOF_V_PILOTS);
    oi_k2_compute_v_pilots(v_pilots, abs_buf, arg_buf, OI_K2_NOF_V_PILOTS, 1);
    for (uint i = 0; i < OI_K2_NOF_V_PILOTS; i++) enlarged[i] = v_pilots[i];

    for (uint i = 0; i < OI_K2_NOF_V_PILOTS; i++) {
      abs_buf[i] = oi_k2_cabs(pilots_lse[OI_K2_NOF_PILOTS - OI_K2_NOF_V_PILOTS + i]);
    }
    oi_k2_unwrap(arg_buf, &pilots_lse[OI_K2_NOF_PILOTS - OI_K2_NOF_V_PILOTS], OI_K2_NOF_V_PILOTS);
    oi_k2_compute_v_pilots(v_pilots, abs_buf, arg_buf, OI_K2_NOF_V_PILOTS, 0);
    for (uint i = 0; i < OI_K2_NOF_V_PILOTS; i++) {
      enlarged[OI_K2_NOF_V_PILOTS + OI_K2_NOF_PILOTS + i] = v_pilots[i];
    }

    for (uint i = 0; i < OI_K2_NOF_PILOTS; i++) enlarged[OI_K2_NOF_V_PILOTS + i] = pilots_lse[i];
  }

  // 3. FD smoothing: 15-tap FIR, every real pilot gets a full (non-truncated) window since the
  // virtual padding (7) exactly equals the filter half-width (verified: convolution_same's
  // partial-overlap edge handling never triggers for any of the 306 real-pilot output positions
  // -- see p2d-k2-k3/VERIFICATION.md for the derivation/empirical check).
  float2 filtered_pilots[OI_K2_NOF_PILOTS];
  for (uint p = 0; p < OI_K2_NOF_PILOTS; p++) {
    float2 acc = (float2)(0.0f, 0.0f);
    for (uint k = 0; k < OI_K2_FILTER_LEN; k++) {
      acc += enlarged[p + k] * oi_k2_fd_filter[OI_K2_FILTER_LEN - 1u - k];
    }
    filtered_pilots[p] = acc;
    filtered_pilot_out[p] = acc;
  }

  // 4. Frequency linear interpolation, offset=0/stride=2 (interpolator_linear_impl.cpp): even REs
  // get the filtered pilot directly, odd REs get the midpoint of their two neighboring pilots,
  // except the very last RE (611) which flat-repeats the last pilot (no pilot follows it).
  for (uint p = 0; p < OI_K2_NOF_PILOTS; p++) {
    fd_est_out[2u * p] = filtered_pilots[p];
    if (p + 1u < OI_K2_NOF_PILOTS) {
      fd_est_out[2u * p + 1u] = filtered_pilots[p] + (filtered_pilots[p + 1u] - filtered_pilots[p]) * 0.5f;
    } else {
      fd_est_out[2u * p + 1u] = filtered_pilots[p];  // tail: flat-repeat, matches std::fill of the last value
    }
  }
}
