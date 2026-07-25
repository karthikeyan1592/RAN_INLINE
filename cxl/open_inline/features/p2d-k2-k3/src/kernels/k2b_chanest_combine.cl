/* k2b_chanest_combine.cl — K2b, cross-symbol time-domain combine + noise/EPRE (T3 port; P2-R4,
 * second half -- K2a does the per-DMRS-symbol frequency-domain work).
 *
 * Port grounding: lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp's
 * apply_td_domain_strategy() (time-domain interpolate/hold, lines ~614-676) and its
 * simd_vector_interpolate() (the actual per-RE blend formula, lines ~864-892:
 * out[i]=first[i]+(second[i]-first[i])*weight, with flat-repeat before the first and after the
 * last DM-RS symbol) -- NOT interpolator_linear_impl (that class is frequency-only, K2a's job;
 * an earlier draft of this citation named it in error and was caught before landing, see
 * VERIFICATION.md). Noise-variance grounded in estimate_noise() (lines ~763-862) and its
 * normalization in do_compute() (lines ~284-286); EPRE in do_compute() line ~274 +
 * compute_hop()'s accumulator (lines ~354-356). third_party/ocudu, BSD-3, release_26_04.
 *
 * KNOWN SIMPLIFICATION (flagged, not silent -- see VERIFICATION.md "noise-variance formula"
 * section): the real estimate_noise() regenerates predicted pilots using a channel estimate
 * (`scaled_estimates`) that is constructed at the same width as the full frequency-interpolated
 * `freq_response` (612 REs), while the values it's compared against (`symbol_pilots`/
 * `rx_pilots`) are pilot-width (306) -- the exact indexing OCUDU uses to reconcile those two
 * widths inside `ocuduvec::prod()` was not fully resolved from reading alone. This kernel instead
 * operates entirely at the 306-pilot-RE granularity (time-averaging K2a's `filtered_pilot`
 * outputs, not the 612-wide frequency-interpolated estimate), which is dimensionally unambiguous
 * and captures the same underlying idea (residual between actual and estimate-regenerated
 * pilots). Validated empirically against the real oracle in tests/k2_test.cpp; the measured
 * relative error IS the acceptance signal for this simplification (Q1's own methodology), not
 * asserted correct by construction.
 *
 * Single work-item (matches K2a; no performance claim, SIM tier).
 */
#include "oi_kernel_compat.h"

#define OI_K2_NOF_PILOTS 306u
#define OI_K2_NOF_SUBCARRIERS 612u
#define OI_K2_NOF_DATA_SYMBOLS 11u
#define OI_K2_MAX_SINR_LINEAR 1.0e10f  // 10^(MAX_SINR_DB/10), MAX_SINR_DB=100 (port_channel_estimator_average_impl.cpp)

// Data-symbol-index -> linear position in ch_est's data-RE-linear order (LLD §4.3): symbols
// {0,1,3,4,5,6,8,9,10,12,13} map to linear positions 0..10 in that same left-to-right order
// (DMRS symbols {2,7,11} carry no data REs in this MVP's 2-CDM-group-without-data config).
__constant int oi_k2_symbol_to_linear[14] = {0, 1, -1, 2, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10};

__kernel void k2b_chanest_combine(
    __global const float2* fd_est_sym2, __global const float2* fd_est_sym7,
    __global const float2* fd_est_sym11, __global const float2* filtered_pilot_sym2,
    __global const float2* filtered_pilot_sym7, __global const float2* filtered_pilot_sym11,
    __global const float2* dmrs_ref_seq_sym2, __global const float2* dmrs_ref_seq_sym7,
    __global const float2* dmrs_ref_seq_sym11, __global const float2* re_grid,
    float epre_partial_sym2, float epre_partial_sym7, float epre_partial_sym11,
    __global float2* ch_est_out, __global float* noise_var_out, __global float* epre_out) {
  // --- Time-domain combine (apply_td_domain_strategy's "interpolate" branch) ---
  for (uint sym = 0; sym < 14u; sym++) {
    int lin = oi_k2_symbol_to_linear[sym];
    if (lin < 0) continue;  // DMRS symbol, no data REs to fill
    __global float2* out_row = ch_est_out + (uint)lin * OI_K2_NOF_SUBCARRIERS;

    float weight;
    __global const float2* first;
    __global const float2* second;
    if (sym <= 2u) {
      first = second = fd_est_sym2;
      weight = 0.0f;
    } else if (sym < 7u) {
      first = fd_est_sym2;
      second = fd_est_sym7;
      weight = (float)(sym - 2u) / (float)(7u - 2u);
    } else if (sym == 7u) {
      first = second = fd_est_sym7;
      weight = 0.0f;
    } else if (sym < 11u) {
      first = fd_est_sym7;
      second = fd_est_sym11;
      weight = (float)(sym - 7u) / (float)(11u - 7u);
    } else {
      first = second = fd_est_sym11;
      weight = 0.0f;
    }
    for (uint re = 0; re < OI_K2_NOF_SUBCARRIERS; re++) {
      out_row[re] = first[re] + (second[re] - first[re]) * weight;
    }
  }

  // --- EPRE: average received pilot power across all DM-RS REs of all DM-RS symbols ---
  float epre_total = epre_partial_sym2 + epre_partial_sym7 + epre_partial_sym11;
  *epre_out = epre_total / (float)(3u * OI_K2_NOF_PILOTS);

  // --- Noise variance: time-averaged filtered-pilot estimate regenerates predicted pilots per
  // DM-RS symbol; residual against actual rx pilots, summed over all pilots and symbols.
  float noise_energy = 0.0f;
  float rsrp_energy = 0.0f;
  for (uint p = 0; p < OI_K2_NOF_PILOTS; p++) {
    float2 h_avg = (filtered_pilot_sym2[p] + filtered_pilot_sym7[p] + filtered_pilot_sym11[p]) / 3.0f;
    rsrp_energy += h_avg.x * h_avg.x + h_avg.y * h_avg.y;

    float2 rx2 = re_grid[2u * OI_K2_NOF_SUBCARRIERS + 2u * p];
    float2 rx7 = re_grid[7u * OI_K2_NOF_SUBCARRIERS + 2u * p];
    float2 rx11 = re_grid[11u * OI_K2_NOF_SUBCARRIERS + 2u * p];

    float2 ref2 = dmrs_ref_seq_sym2[p];
    float2 ref7 = dmrs_ref_seq_sym7[p];
    float2 ref11 = dmrs_ref_seq_sym11[p];

    float2 pred2 = (float2)(h_avg.x * ref2.x - h_avg.y * ref2.y, h_avg.x * ref2.y + h_avg.y * ref2.x);
    float2 pred7 = (float2)(h_avg.x * ref7.x - h_avg.y * ref7.y, h_avg.x * ref7.y + h_avg.y * ref7.x);
    float2 pred11 = (float2)(h_avg.x * ref11.x - h_avg.y * ref11.y, h_avg.x * ref11.y + h_avg.y * ref11.x);

    float2 res2 = rx2 - pred2;
    float2 res7 = rx7 - pred7;
    float2 res11 = rx11 - pred11;

    noise_energy += res2.x * res2.x + res2.y * res2.y;
    noise_energy += res7.x * res7.x + res7.y * res7.y;
    noise_energy += res11.x * res11.x + res11.y * res11.y;
  }

  float noise_var = noise_energy / (float)(3u * OI_K2_NOF_PILOTS - 1u);
  float rsrp_avg = rsrp_energy / (float)OI_K2_NOF_PILOTS;
  float min_noise_variance = rsrp_avg / OI_K2_MAX_SINR_LINEAR;
  *noise_var_out = fmax(min_noise_variance, noise_var);
}
