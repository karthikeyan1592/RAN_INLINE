# p2b-k5-k6 — Verification Status

See [`README.md`](README.md) for scope. This file records what was actually built and verified
(2026-07-22), same convention as `p2a-scaffold/VERIFICATION.md`.

## What's implemented and verified for real

| Component | File(s) | Verified how | Result |
|---|---|---|---|
| K5 descrambler (host reference) | `src/host/oi_p2_gold_init.{h,cpp}` | `tests/k5_test.cpp` Test 1: compared against the REAL linked `libocudu_sequence_generators.a` (`create_pseudo_random_generator_sw_factory`), 4 c_inits (incl. MVP's real RNTI=0x4601/n_ID=1) x 4 lengths + LLR_INFTY boundary case | Bit-exact, all cases |
| K5 descrambler (kernel) | `src/kernels/k5_descrambler.cl` | `tests/k5_test.cpp` Test 2: the ACTUAL `.cl` kernel, built and run via real PoCL, compared against the same real library | Bit-exact |
| K6 rate-dematcher (kernel) | `src/kernels/k6_rate_dematcher.cl` | `tests/k6_test.cpp`: the ACTUAL `.cl` kernel, built and run via real PoCL, compared against the REAL linked `libocudu_ldpc.a`'s `ldpc_rate_dematcher_impl` (factory string `"generic"`, per D3 — never avx2/avx512/neon), both base graphs x all 3 MVP Qm values (2/4/6), plus a dedicated filler-bit-region check | Bit-exact, all cases |

**K5: 25/25 real assertions pass. K6: 12/12 real assertions pass.**

## Real bugs found and fixed during this pass

1. **K6 test vector exceeded single-pass capacity.** First `k6_test.cpp` draft picked
   `rm_length=522` for the BG1/QAM64 "full-span" case, 2 bits past the single-pass boundary
   (`full_length - nof_filler_bits = 520`). The real `ldpc_rate_dematcher_impl` would wrap the
   circular buffer and enter combine-mode for the excess — a path K6's MVP kernel deliberately
   doesn't implement (HARQ/repetition out of scope, P2-R8). This was a test-vector bug, not a
   kernel bug: caught by the real-library comparison mismatching at byte 524. Fixed by picking a
   value at the boundary instead of past it.
2. **K6 test vector not Qm-aligned.** The boundary fix's first attempt (`rm_length=520`) isn't a
   multiple of Qm=6, violating the real library's own precondition
   (`input.size() % modulation_order == 0`) — 3GPP rate-matched output length is always a multiple
   of Qm by construction (Qm bits per modulated symbol). The real library didn't assert-abort on
   the violation (release-mode `ocudu_assert` apparently doesn't), it silently produced a
   mismatching value at byte 524 instead — a second real bug caught by the same comparison,
   distinct from bug 1. Fixed by using `rm_length=516` (largest Qm=6 multiple within the
   single-pass boundary).

Both bugs were caught purely because the test compares against the REAL linked OCUDU library
rather than hand-computed expected values — a hand-computed oracle would have had no way to
independently reveal either mistake.

## Known scope limitation — NOT resolved, flagged for p2f-integration

K6's kernel handles exactly one pass through the rate-matching circular buffer: it recovers
`rm_length <= full_length - nof_filler_bits` correctly, but does not implement the real
algorithm's wraparound + `combine_softbits()` path for `rm_length` beyond that (which 3GPP allows
for aggressively repetition-coded transmissions, e.g. very low MCS with generous RE allocation).
This is believed consistent with the MVP's scope (rv=0, new_data=true always, HARQ/Nref
deferred — P2-R8), but **whether the MVP's actual fixed configuration (51 PRB, MCS {4,13,21})
can ever produce an E that large for a single codeblock has not been checked against the real
TBS/RE-mapping arithmetic.** Flagged as an open item for p2f-integration, where real TB sizes are
wired end-to-end; if it turns out E can exceed the single-pass boundary for one of the three MVP
MCS points, this kernel needs the wraparound pass added before P2-R8 can be called satisfied.
See the matching comment in `src/kernels/k6_rate_dematcher.cl`'s file header.

## Reproducibility gap — found and fixed (2026-07-22, independent re-verification pass)

An independent re-run of both tests (same day, separate session) reproduced every result above
bit-for-bit, but flagged a real process gap: this slice had no build script anywhere. The OCUDU
CMake target list, the required static-link order (`libocudu_channel_coding.a` before
`libocudu_ldpc.a` — reconstructed from `p0-rig-scaffold`'s own Dockerfile after first getting it
backwards and hitting undefined-vtable linker errors), and the CWD-relative kernel-source paths
baked into both test binaries were all undocumented, making "verified" hard to independently
reproduce without redoing that reconstruction. Fixed by adding `Makefile` (+ `tests/README.md`):
`make bootstrap-ocudu` builds OCUDU into a persistent `open_inline/.build/ocudu` (not
session-scratch `/tmp`, which had already been wiped twice during this slice's own development),
and `make test` builds and runs both binaries with the correct link order and working directory.
Verified clean end-to-end from a fully wiped `OCUDU_BUILD` and a `make clean`: both binaries
rebuild and re-pass (25/25, 12/12) with zero manual intervention.

## P2-R14/R14a (dual-oracle rule) status

K5 and K6 both satisfy P2-R14a's shippable-oracle requirement in spirit by linking directly
against the real currently-built OCUDU library rather than a checked-in golden-vector file — a
stronger oracle than either the CI-only srsRAN-AGPL path or a static Sionna-generated vector file
would give on their own. Neither test currently links against `third_party/srsRAN_Project`; the
dual-oracle CI-only cross-check against srsRAN's independent implementation (P2-R14) has not been
set up for K5/K6 and remains open, same status as recorded in this sub-feature's `README.md`.
