/* k3_equalizer.cl — K3, equalizer, 1 Tx layer x 1 Rx port (T3 port; P2-R5).
 *
 * Port grounding: lib/phy/upper/equalization/channel_equalizer_generic_impl.cpp (the nof_tx_layers==1
 * MMSE special-case, lines ~589-597: "For one Tx layer... the MMSE equalizer is equivalent to the
 * ZF one" -- confirmed by reading the source, not assumed) + equalize_zf_1xn.h's scalar formula
 * (lines ~110-152) + channel_equalizer_generic_impl.cpp's equalize_zf_single_tx_layer_reduction()
 * (lines ~118-173, the "zero valid ports" pre-check). third_party/ocudu, BSD-3, release_26_04.
 *
 * CORRECTED vs parent LLD's original wording (found + fixed during p2d-k2-k3, not silently
 * applied): the LLD described K3 as "generalized to the MMSE 1/(|h|^2+sigma^2) denominator" --
 * that formula does not appear anywhere in this code path. For a single Tx layer, OCUDU's own
 * MMSE branch literally calls the ZF kernel (its own comment says so); there is no
 * Wiener/regularized-inverse term. The real formula is:
 *   x_hat     = (y * conj(h)) / (tx_scaling * |h|^2)
 *   sigma2out = (|h|^2 * sigma^2) / (tx_scaling * |h|^2)^2 = sigma^2 / (tx_scaling^2 * |h|^2)
 * Parent LLD should be corrected to match (flagged in VERIFICATION.md, not silently left wrong,
 * same pattern as K1's /32767 fix).
 *
 * Work-item = one data RE (HLD §4 step 4).
 */
#include "oi_kernel_compat.h"

__kernel void k3_equalize(__global const float2* re_grid, __global const float2* ch_est,
                          __global const float* noise_var, float tx_scaling,
                          __global float2* eq_symbols, __global float* eq_noise_var) {
  uint re = get_global_id(0);

  float nvar = *noise_var;

  // equalize_zf_single_tx_layer_reduction's "zero valid ports" pre-check (channel_equalizer_generic_impl.cpp
  // ~138-141): RX_PORTS=1, so "zero valid ports" reduces to "this one port's noise_var is degenerate".
  if (!(nvar > 0.0f) || isinf(nvar)) {
    eq_symbols[re] = (float2)(0.0f, 0.0f);
    eq_noise_var[re] = INFINITY;
    return;
  }

  float2 y = re_grid[re];
  float2 h = ch_est[re];
  float ch_est_norm = h.x * h.x + h.y * h.y;  // |h|^2

  // Defaults (equalize_zf_1xn.h lines ~130-131), overwritten below only if everything is normal.
  float2 symbol_out = (float2)(0.0f, 0.0f);
  float nvar_out = INFINITY;

  if (isnormal(ch_est_norm) && isnormal(nvar) && nvar > 0.0f) {
    float nvar_acc = ch_est_norm * nvar;                                 // |h|^2 * sigma^2
    float2 re_out = (float2)(y.x * h.x + y.y * h.y, y.y * h.x - y.x * h.y);  // y * conj(h)

    float d_pinv = tx_scaling * ch_est_norm;  // tx_scaling * |h|^2
    if (isnormal(d_pinv) && isnormal(nvar_acc)) {
      float d_pinv_rcp = 1.0f / d_pinv;
      symbol_out = re_out * d_pinv_rcp;
      nvar_out = nvar_acc * d_pinv_rcp * d_pinv_rcp;
    }
  }

  eq_symbols[re] = symbol_out;
  eq_noise_var[re] = nvar_out;
}
