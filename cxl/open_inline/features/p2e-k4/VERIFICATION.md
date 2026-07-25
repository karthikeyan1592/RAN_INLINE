# p2e-k4 — Verification Status

See [`README.md`](README.md) for scope. This file records what was actually built and verified
(2026-07-22).

## What's implemented and verified for real

| Component | File(s) | Verified how | Result |
|---|---|---|---|
| K4 demapper (QPSK/16QAM/64QAM) | `src/kernels/k4_demapper.cl` | `tests/k4_test.cpp`: the actual kernel via PoCL vs the real linked OCUDU `demodulation_mapper` (scalar dispatch), across random symbols (all 3 MVP Qm), saturation-boundary vectors, near-zero-symbol vectors (16/64QAM only), and degenerate noise_var vectors | **17/17 real assertions pass, all bit-exact** (P2-R6's gate has no tolerance column) |

## Real bugs found and fixed during this pass

1. **OpenCL C rejects `__constant` arrays declared inside a non-kernel function.** The first
   draft declared the 64QAM piecewise-linear lookup tables (`slope`/`intercept`, 8/8/4 entries)
   as local `__constant` arrays inside `oi_k4_qam64_01/23/45`. PoCL's build failed with
   "non-kernel function variable cannot be declared in constant address space" — OpenCL C
   requires `__constant` variables at program (file) scope. Fixed by hoisting all three
   slope/intercept table pairs to file scope. Caught immediately by the real build (PoCL), not by
   manual review — the kind of portability bug the P2-R2 lint doesn't catch (it's not a banned
   token, just an invalid scope), which is itself worth noting for future kernels using lookup
   tables.

No other issues: the QPSK/16QAM/64QAM formulas, quantize()'s clip-then-round, and the
near-zero/degenerate-noise_var guards all matched the real OCUDU demapper bit-for-bit on the
first run after the scope fix — no precision-tolerance workaround was needed here (unlike K1's
bf16 finding or K3's SIMD-reciprocal finding), because K4's own committed gate is genuinely
bit-exact and OCUDU's scalar demapper path (the one ported) has no float-storage or SIMD-
approximation step in it.

## Port scope note

Ported the SCALAR demapper paths only (`demod_QPSK_symbol`, `demod_16QAM_symbol_01/23`,
`demod_64QAM_symbol_01/23/45`), never the AVX2/AVX512/NEON batched functions in the same source
files (P2-R2/D3) — those exist purely as a vectorized fast path over the identical scalar
formula, confirmed by reading both and finding the batched code just packs N calls of the same
math into one SIMD instruction sequence, not a different algorithm.

## P2-R14/R14a status

Same substitution as K1/K5/K6/K2/K3: verified against the real linked OCUDU
`demodulation_mapper` (a single strong oracle), not the literal srsRAN-AGPL-CI-only +
Sionna-shippable dual-artifact structure R14/R14a describes. Bit-exactness (P2-R6) is fully met
against this oracle — there is no tolerance column to set, so this slice's dual-oracle gap is
narrower than K2/K3's (no threshold-setting task is left incomplete, just the CI-only-vs-
shippable artifact wiring).
