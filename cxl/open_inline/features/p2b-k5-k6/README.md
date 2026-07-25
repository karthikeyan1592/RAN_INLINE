# p2b-k5-k6

Implementation slice of [`p2-phy-kernels`](../p2-phy-kernels/spec/) — the two bit-exact integer
kernels, chosen to land first because they have zero float-tolerance ambiguity and prove the whole
port+oracle pattern on the smallest possible surface before harder kernels.

## Scope

- **K5 — descrambler** (`src/kernels/k5_descrambler.cl`), port of
  `lib/phy/upper/sequence_generators/pseudo_random_generator_impl.{h,cpp}` (`apply_xor`, `init`,
  fast-advance). Gold-sequence x1/x2 generation (TS 38.211 §5.2.1, `c_init = RNTI·2¹⁵ + n_ID` per
  §6.3.1.1) + sign-flip on int8 LLRs, `±LLR_INFTY` passed through unreinterpreted. **P2-R7**.
- **K6 — rate-dematcher** (`src/kernels/k6_rate_dematcher.cl`), port of
  `lib/phy/upper/channel_coding/ldpc/ldpc_rate_dematcher_impl.{h,cpp}` (generic class only, never
  the avx2/avx512/neon variants — D3). Bit-selection revert (k₀ for rv=0) + per-Qm de-interleave,
  filler-bit LLR = `+LLR_INFTY`. **P2-R8**.
- `tests/k5_test.cpp`, `tests/k6_test.cpp` — built and run for real (2026-07-22): the actual
  `.cl` kernels, compiled and executed via real PoCL, compared bit-exact against the REAL,
  currently-built OCUDU libraries (`libocudu_sequence_generators.a`,
  `libocudu_ldpc.a`'s `ldpc_rate_dematcher_impl`, generic variant per D3) — not vector files.
  K5: 25/25 assertions pass. K6: 12/12 assertions pass, both base graphs, all three MVP Qm values.
  See `VERIFICATION.md` for bugs found (two invalid K6 test vectors, both caught by the real-
  library comparison) and one open scope limitation (K6 has no wraparound/repetition pass).
- `Makefile`, `tests/README.md` — `make bootstrap-ocudu` (once) + `make test` reproduces the
  above from scratch: correct OCUDU CMake targets, correct static-link order, correct working
  directory for the CWD-relative kernel paths. Added after an independent re-verification pass
  flagged that none of this was previously written down anywhere (see `VERIFICATION.md`).

## Gates this slice owns

P2.5={R7}, P2.6={R8} from the parent gate-mapping table — **and P2-R14/R14a for both K5 and K6**,
claimed explicitly here since it's cross-cutting and easy to leave unowned.

**Actual status vs. the gate as specified:** R7/R8 (bit-exact correctness) are met — verified
against the real linked OCUDU library, the strongest single oracle available (stronger than a
static vector file either CI-only or shippable path would give). **R14/R14a as literally
specified are NOT yet met**: that requires *both* the srsRAN-AGPL CI-only oracle *and* a
Sionna-generated shippable oracle wired and passing side by side, with R14a's lineage-flag check
gating trust in the srsRAN result. What got built instead links directly against the real OCUDU
library (`third_party/ocudu`, BSD-3) as a single strong oracle — correctness is verified, but the
CI-only-vs-shippable dual-artifact structure R14/R14a actually asks for (so that a shippable
build never needs srsRAN AGPL present) has not been built. Wiring the srsRAN-AGPL and
Sionna-vector paths on top of this remains open work for this slice, not silently resolved by the
OCUDU-library comparison.

## Depends on

`p2a-scaffold` (host API, `oi_kernel_compat.h`, provenance/lint tooling).

## Explicitly NOT in this slice

K1–K4, LDPC hookup, pipeline integration. K5/K6 are tested standalone against oracle-supplied LLR
inputs, not chained from a live K4 output — no dependency on p2c/p2d/p2e.
