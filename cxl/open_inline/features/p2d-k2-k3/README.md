# p2d-k2-k3

Implementation slice of [`p2-phy-kernels`](../p2-phy-kernels/spec/) — the two chained
float-tolerance kernels (channel estimation feeds directly into equalization; grouped together
since K3's oracle vectors are generated downstream of K2's in the same oracle pipeline run).

## Scope

- **K2a — per-DMRS-symbol LS + FD-smoothing + frequency interpolation**
  (`src/kernels/k2a_chanest_symbol.cl`), port of `port_channel_estimator_average_impl.cpp`'s
  per-symbol LS/matched-filter + FD "filter" smoothing (real 31-tap raised-cosine FIR + virtual-
  pilot extrapolation, port_channel_estimator_helpers.cpp) + `interpolator_linear_impl.cpp`'s
  frequency linear interpolation. **Split from a single "K2" per a mid-implementation design
  correction — see below.**
- **K2b — cross-symbol time-domain combine + noise/EPRE**
  (`src/kernels/k2b_chanest_combine.cl`), port of `apply_td_domain_strategy()`/
  `simd_vector_interpolate()` (time-domain interpolate/hold between DM-RS symbols) +
  `estimate_noise()`/`do_compute()` (noise-variance/EPRE, one documented simplification — see
  `VERIFICATION.md`). Together K2a+K2b satisfy **P2-R4**.
- **K3 — equalizer, 1 Tx layer × 1 Rx port** (`src/kernels/k3_equalizer.cl`), port of
  `channel_equalizer_generic_impl.cpp`'s MMSE branch — which for 1 Tx layer is, by OCUDU's own
  code comment, identical to the ZF formula: `x_hat = y*conj(h) / (tx_scaling*|h|^2)`,
  `sigma2_out = sigma^2 / (tx_scaling^2*|h|^2)`. **Corrects the parent LLD's original
  `1/(|h|^2+sigma^2)` description, which doesn't appear anywhere in this code path (never checked
  against real code before this slice).** **P2-R5**.
- **Q1 (parent LLD §8) resolved**: T_K2/T_K2n/T_K3/T_K3n measured via a 3-point SNR sweep against
  the real linked OCUDU `dmrs_pusch_estimator`/`channel_equalizer_generic_impl` and recorded in
  the parent LLD §7 table, all with 2x-50x margin over the measured maxima.
- `tests/k2_test.cpp`, `tests/k3_test.cpp` — real kernels (via PoCL) vs the real linked OCUDU
  libraries, not synthetic vectors. **k2_test: NRMSE(ch_est) max 0.0226; k3_test: 19/19 assertions
  pass, NRMSE(eq_symbols)=0.0000985.** Full detail, including two real bugs found (a citation
  error caught before landing, and a genuine parent-LLD formula error) in `VERIFICATION.md`.

## K2 split rationale (K2a + K2b) — read before touching either kernel

K2's original committed prototype took a scalar `dmrs_symbol_idx`, implying one launch handles
one DM-RS symbol independently. But the LLD's own time-domain hold rule needs linear interpolation
*between* two different DM-RS symbols (e.g. data symbols 3-6 need values from both DM-RS symbol 2
and DM-RS symbol 7) — a single-symbol kernel can't produce that alone. Split into K2a (per-symbol
FD estimate) + K2b (cross-symbol combine), which turned out to mirror a seam OCUDU's own port
sources already have, not an invented one. Full rationale, and the two other structural options
considered and rejected, in `VERIFICATION.md`.

## Gates this slice owns

P2.2={R4}, P2.3={R5} from the parent gate-mapping table — **and P2-R14/R14a for both K2 and K3**,
claimed explicitly, same caveat as `p2b-k5-k6`'s README: what got built is a single strong oracle
(the real linked OCUDU library) rather than the literal srsRAN-AGPL-CI-only + Sionna-shippable
dual-artifact structure R14/R14a describes. P2-R4/R5 correctness and Q1's threshold-setting are
both met against this oracle; wiring the CI-only-vs-shippable dual-oracle structure on top remains
open work for this slice, not silently resolved.

## Depends on

`p2a-scaffold` (host API; also required a stub-pipeline stage-count bump, 7→8, to accommodate the
K2a/K2b split — see `p2a-scaffold/VERIFICATION.md`). Does **not** depend on `p2c-k1` — unit gates
feed K2/K3 oracle-supplied RE-grid/chan-est inputs directly, not live K1 output.

## Watch item

Open Question Q5 (parent LLD §8): DC-subcarrier / transform-precoding fields are assumed
irrelevant for this MVP's full-band, non-DFT-s-OFDM allocation — K2a/K2b's kernels follow this
same assumption (no `dc_position` handling at all). Still open, not newly resolved by this slice.
