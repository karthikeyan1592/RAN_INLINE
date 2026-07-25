# p2d-k2-k3 — Verification Status

See [`README.md`](README.md) for scope. This file records what was actually built and verified
(2026-07-22).

## What's implemented and verified for real

| Component | File(s) | Verified how | Result |
|---|---|---|---|
| K2a — per-DMRS-symbol FD estimate | `src/kernels/k2a_chanest_symbol.cl` | `tests/k2_test.cpp`, part of the K2a+K2b pipeline test below | See combined K2 result |
| K2b — cross-symbol time-combine + noise/EPRE | `src/kernels/k2b_chanest_combine.cl` | same | Combined ch_est NRMSE, noise_var/epre relative error, all measured against the real linked OCUDU `dmrs_pusch_estimator` across a 3-point SNR sweep | **NRMSE(ch_est) max 0.0226, noise_var rel. err. max 0.0236, epre rel. err. max 0.00015** — see full table below |
| K3 — equalizer (MMSE=ZF, 1x1) | `src/kernels/k3_equalizer.cl` | `tests/k3_test.cpp`: the actual kernel via PoCL vs the real linked OCUDU `channel_equalizer_generic_impl`, 9 normal (noise_var, tx_scaling) combos + 2 degenerate cases (|h|=0, noise_var=0) | **19/19 assertions pass**; NRMSE(eq_symbols) = 0.0000985, rel. err.(eq_noise_var) max = 0.00055 |
| DM-RS reference sequence | `src/host/oi_dmrs_ref_seq.h/.cpp` | Reuses K5's already-verified Gold-sequence LFSR (`oi_p2_gold_init`); cross-validated implicitly by the k2_test.cpp pipeline matching the real OCUDU estimator's own DM-RS generation bit-for-bit (a mismatch here would show up as a large, not small, NRMSE) | Confirmed via the tight NRMSE numbers above |

**Full measured sweep (k2_test.cpp, 3 configs spanning low/moderate/high SNR):**

| Config | ch_est NRMSE | noise_var rel. err. | epre rel. err. |
|---|---|---|---|
| moderate SNR (h=0.8+0.3j, σ=0.02) | 0.00474 | 0.000172 | 0.0000526 |
| low SNR (h=0.1+1.2j, σ=0.15) | 0.02258 | 0.000340 | 0.0000161 |
| high SNR (h=-0.5-0.9j, σ=0.005) | 0.00165 | 0.0236 | 0.0000188 |

T_K2/T_K2n/T_K3/T_K3n set from these numbers with margin — recorded in the parent
`p2-phy-kernels/spec/LLD.md` §7 table (Q1 resolved there).

**Spec/code gap found and fixed (2026-07-22, independent re-verification pass):** the test
assertions initially enforced looser thresholds than the ones just written into the LLD — an
artifact of the test code carrying forward its own first-draft "provisional" envelope
(`< 0.1`/`< 0.01`) instead of being tightened once the LLD's real margin analysis landed
(`0.05`/`0.005`). Caught by an independent reviewer noticing the LLD's stated margins (~2.2x for
T_K2n, ~50x for T_K3) only reconcile arithmetically against the tighter numbers — e.g.
`0.005/0.0000985 ≈ 50x` matches T_K3's documented margin, while the code's `0.01` would have been
~100x, not what's written. That mismatch is the tell that the LLD numbers were the considered
ones (margin computed, K3's SIMD-approximation root cause identified and written down) and the
code's were unrefreshed scaffolding. **Fixed by tightening the code to match the LLD** (not the
reverse): `k2_test.cpp`'s `max_noise_var_err` bound `0.1 -> 0.05`, `k3_test.cpp`'s two bounds
`0.01 -> 0.005` each. All measured values clear the tighter bounds with 2x-50x room to spare, so
nothing broke; the "provisional" wording in both files' printed labels was also dropped, since Q1
is recorded as resolved in the LLD and the test's own gate shouldn't still call itself
provisional. Re-verified: both `k2_test`/`k3_test` still pass in full against the tightened
thresholds. Same category of gap as the `oi_p2_feed` ABI reconciliation and K1's mis-citation
catch — a real decision existed, the code just hadn't caught up.

## Real bugs / gaps found and fixed during this pass

1. **Parent LLD's K3 formula was wrong.** LLD text said MMSE uses a `1/(|h|^2+sigma^2)`
   denominator. Reading `channel_equalizer_generic_impl.cpp` (~line 589) shows OCUDU's own code
   comment: for 1 Tx layer, the MMSE branch literally calls the ZF kernel — "the MMSE equalizer is
   equivalent to the ZF one." Real formula: `x_hat = y*conj(h) / (tx_scaling*|h|^2)`,
   `sigma2_out = sigma^2 / (tx_scaling^2 * |h|^2)`. Fixed in the kernel and flagged for the parent
   LLD to correct (same pattern as K1's `/32767` fix).
2. **K2's committed prototype was under-specified for its own time-domain hold rule** (a scalar
   `dmrs_symbol_idx` can't produce a value that needs interpolating BETWEEN two DMRS symbols).
   Resolved by splitting into K2a (per-symbol FD estimate) + K2b (cross-symbol time-combine) — see
   the "K2 split rationale" section below. This required bumping p2a's stub pipeline from 7 to 8
   stages (`p2a-scaffold/VERIFICATION.md`).
3. **A citation error was caught before landing, not after.** An early plan cited K2b's
   time-domain-combine logic to `interpolator_linear_impl` — checked specifically and found wrong:
   that class is frequency-domain-only. The real grounding is `apply_td_domain_strategy()`/
   `simd_vector_interpolate()` in `port_channel_estimator_average_impl.cpp` (~lines 614-676,
   864-892). Corrected in `provenance.json` and the kernel's own header comment before either was
   committed.
4. **`ch_symbols`/`ch_estimates` in K3's real oracle path are stored as `cbf16_t`** (bf16-rounded,
   7 mantissa bits) — same lesson as K1's RE-grid precision finding. Initially caused all 9
   "normal" K3 test cases to fail with ~1e-4-level mismatches. Fixed by rounding the test's inputs
   through bf16 before feeding either implementation, so both compute on identical values.
5. **OCUDU's SIMD equalizer path uses an approximate reciprocal instruction**
   (`ocudu_simd_f_rcp`, `equalize_zf_1xn.h` ~line 88) instead of exact division — confirmed by
   reading the source after the bf16 fix still left small (~1.5e-4 relative) residual mismatches
   that didn't fit a precision-storage explanation. This kernel uses exact division and is if
   anything more precise; P2-R2 already bans porting SIMD-width-specific tricks into kernels, so
   matching the approximation would be both against the rules and a regression. Test tolerance
   set to 2e-3 relative to accommodate this understood, non-bug gap.

## K2 split rationale (K2a + K2b)

The parent LLD's original K2 prototype took a scalar `dmrs_symbol_idx` and was documented as
"work-group = one DM-RS symbol" — implying each launch handles one DM-RS symbol independently. But
the LLD's own time-domain hold rule (§4.3) requires linear interpolation *between* two different
DM-RS symbols for the data symbols in between (e.g., symbols 3-6 need values blended from DM-RS
symbols 2 *and* 7) — something a single-symbol-at-a-time kernel cannot produce on its own.

Resolution (user-directed, 2026-07-22): split into **K2a** (per-DM-RS-symbol LS + FD-smoothing +
frequency interpolation — one clean job, matches `port_channel_estimator_average_impl`'s
per-symbol processing) and **K2b** (cross-symbol time-combine + noise/EPRE — matches
`apply_td_domain_strategy()`/`simd_vector_interpolate()`). This mirrors a seam OCUDU's own port
sources already have (three separate cited files: `dmrs_pusch_estimator_impl`, one class for the
per-symbol FD estimate, and a distinct time-domain combine function) rather than inventing a new
one. Cost: p2a's stub pipeline grew from 7 to 8 stages (an internal implementation detail, not an
ABI other slices reference — see `p2a-scaffold/VERIFICATION.md`).

## Known simplification — noise-variance formula (flagged, not silent)

The real `estimate_noise()` (`port_channel_estimator_average_impl.cpp` ~lines 763-862) regenerates
predicted pilots using a channel estimate (`scaled_estimates`) built at the same width as the full
frequency-interpolated `freq_response` (612 REs), while comparing against pilot-width (306) actual
values — the exact indexing OCUDU uses to reconcile those two widths inside `ocuduvec::prod()` was
not fully resolved from reading alone. K2b instead operates entirely at 306-pilot-RE granularity
(time-averaging K2a's `filtered_pilot` outputs), which is dimensionally unambiguous and captures
the same underlying idea. **This was validated empirically, not asserted correct by construction**:
the measured noise_var relative error (max 2.36% across the sweep) IS the acceptance signal for
this simplification, per Q1's own stated methodology, and it came in well within the set 5%
threshold.

## P2-R14/R14a (dual-oracle rule) status

Same substitution as K5/K6/K1: what got built links against the real currently-built OCUDU
libraries (`dmrs_pusch_estimator`, `channel_equalizer_generic_impl`) as a single strong oracle,
not the literal srsRAN-AGPL-CI-only + Sionna-shippable dual-artifact structure R14/R14a describes.
Correctness (P2-R4/R5, and Q1's threshold-setting) is met against this oracle; the CI-only-vs-
shippable dual-oracle wiring itself remains open work for this slice, consistent with how p2b's
README already flags the same gap for K5/K6.

## Known-open items (not addressed this pass)

- **Q5 (parent LLD §8)**: DC-subcarrier/transform-precoding fields are assumed irrelevant for
  this MVP's full-band allocation. K2a/K2b's kernels follow this same assumption (no `dc_position`
  handling anywhere) — consistent with, but not a new resolution of, Q5's still-open status.
- **RSRP/SNR**: not implemented — not part of K2's committed I3 output (`ch_est`, `noise_var`,
  `epre` only), so out of scope by the existing prototype, not an oversight.
- Beta scaling (`cfg.scaling`, PUSCH-to-DMRS power ratio) fixed at 1.0 (0 dB) throughout testing —
  a reasonable MVP simplification (no 0dB-deviation value is pinned anywhere in the SPEC's fixed
  MVP table) but not swept across other values.
