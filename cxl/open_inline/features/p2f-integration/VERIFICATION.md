# p2f-integration — Verification Status

See [`README.md`](README.md) for scope. This file records what was actually built and verified
(2026-07-22/23).

## What's implemented and verified for real

| Component | File(s) | Verified how | Result |
|---|---|---|---|
| CRC16/24A/24B | `src/host/oi_p2_crc.{h,cpp}` | Used as ground truth inside `cb_segment_test.cpp`'s round-trip (a wrong CRC port would show up as every "reassembled TB bit-exact" / "corrupted CB detected" check failing) | Passing as part of cb_segment_test below |
| CB segmentation sizing + desegmentation | `src/host/oi_p2_cb_segment.{h,cpp}` | `tests/cb_segment_test.cpp`: sizing vs the real linked OCUDU `ldpc_segmenter_tx` across all 3 MVP MCS points (C=1,2,4); full round-trip (real segmenter builds valid CBs -> our desegmenter reassembles) bit-exact; corruption detection | **30/30 assertions pass** |
| LDPC hookup (K6 output -> p0's decoder -> CB bits) | `src/host/oi_p2_ldpc_decode.{h,cpp}` | `tests/ldpc_decode_test.cpp`: real OCUDU `ldpc_encoder` builds valid codewords, this hookup's `oi_p2_ldpc_decode_cb` (including its internal 2*Z zero-padding bridge) decodes them, compared bit-exact to the original message, across all 3 MVP (base_graph, lifting_size) points plus p0's own BG1/BG2xLS={384,256} baseline | **18/18 assertions pass** |
| `bg_tables.h` provenance fix | `../p0-rig-scaffold/docker/gpu-phy/ldpc_suite/bg_tables.h` | Regenerated from OCUDU's real BSD-3 API; 0/4,096,000 mismatches, identical to pre-regeneration baseline; `oi/gpu-phy:dev` rebuilt from scratch and re-verified inside the container | See `p0-rig-scaffold/docker/gpu-phy/ldpc_suite/MODIFICATIONS.md`'s "bg_tables.h provenance correction" |
| **Real 8-stage pipeline wiring** | `../p2a-scaffold/src/host/oi_p2_host.cpp` (rewritten) | `../p2a-scaffold/tests/host_api_test.cpp`: real K1->K2a(x3)->K2b->K3->K4->K5->K6->LDPC+CRC chain, real kernels throughout, runs to completion without crashing, correct record fields (nof_cb/base_graph for MCS4 matching the real segmenter), mcs_index rejection, all taps readable | **24/24 assertions pass** |
| **Full real TX->RX loop (P2-R15b, the actual pass/fail decode gate)** | `tools/oracle_tx_gen.cpp` (real OCUDU TX chain), `tools/pipeline_runner.cpp` (real RX pipeline driver), `helpers/pcap_packer.py`, `tests/integration/pipeline_test.py` | A real, self-verified, wire-valid PUSCH-like transmission (random TB -> real CRC/segment/LDPC-encode/rate-match/scramble/modulate -> real eCPRI+O-RAN wire frames) fed through the real oi_p2_host pipeline via a stand-in ingest_backend, for all 3 MVP MCS points | **CRC24A pass + TB bit-exact vs the oracle's own known TB, for MCS 4 (C=1), 13 (C=2), and 21 (C=4)** -- 21/21 pipeline_test.py assertions pass |

## Real bugs found and fixed during this pass

1. **K6's `full_length` argument confused with `segment_length`.** While wiring K6 into
   `oi_p2_host.cpp`, used `oi_p2_cb_segment_params::segment_length` (K = 22Zc/10Zc, the LDPC
   decoder's OUTPUT length) as K6's `full_length` argument, which actually needs the CODEWORD
   length N = 66Zc/50Zc (K6's rate-dematch OUTPUT / LDPC decoder's INPUT length) -- two genuinely
   different quantities that happen to be related by a fixed multiplier (N = K*3 for BG1, K*5 for
   BG2) but are not interchangeable. Caught by re-reading my own K6 kernel's internal `%66`/`%50`
   base-graph-detection logic against the value I was about to pass it, before running anything --
   added a `codeword_length` field to `oi_p2_cb_segment_params` (ported from
   `compute_full_codeblock_size`, `ldpc.h`) rather than patching the call site with a one-off
   multiply, so the two lengths stay distinguishable at every future call site.
2. **Missing data-RE compaction step between K1 and K3.** K1 writes the full 14x612 I2 grid; K3's
   already-built-and-verified kernel (`k3_equalize`) indexes `re_grid[re]` assuming a compact,
   data-RE-linear buffer (same shape as I3/I4, 6732 entries) -- a real gap between two
   independently-correct kernels that only surfaced when actually wiring them together. Fixed with
   11 `clEnqueueCopyBuffer` calls (one per non-DMRS symbol row), fully device-side, chained via
   events after K1 -- no kernel change needed to either K1 or K3, no host round-trip.
3. **`bit_diff_test.cpp` reproducibility**: not a new bug, but re-confirmed as part of rebuilding
   `oi/gpu-phy:dev` -- Docker's cache initially reported the ldpc-build stage as `CACHED` after the
   `bg_tables.h` change; a `--no-cache` rebuild was used to remove any doubt, followed by running
   the actual `bit_diff_test` binary inside the freshly built container (not just trusting the
   build log) to confirm the regenerated table is what's actually baked into the shipped image.
4. **64-QAM's peak constellation amplitude gets silently clipped by the real wire codec.**
   Discovered building `oracle_tx_gen.cpp`'s own self-check (real-decoder round-trip over all 14
   wire frames, run before trusting the oracle for anything downstream): MCS 21 (64-QAM, Qm=6)
   failed where MCS 4/13 (QPSK/16-QAM) passed. Root cause: OCUDU's real
   `iq_compression_none_impl` quantizes floats to fixed-point assuming a unit full-scale range
   (`iq_scaling=1.0`, matching `k1_test.cpp`'s own precedent); 64-QAM's outer constellation points
   (amplitude 7/sqrt(42) ~= 1.0801) exceed that range and silently saturate to 1.0 on decode. No
   earlier test in this project ever exercised this because every one of them used amplitudes kept
   below 1.0 by construction (e.g. `k1_test.cpp`'s `dist(-0.9, 0.9)`) -- this is the first time a
   real, correctly-scaled 64-QAM constellation was pushed through the wire codec. Fixed by applying
   a conservative TX amplitude scale (0.9, comfortably below 1/1.0801) to both the modulated data
   symbols and the DMRS reference sequence before wire-encoding, in `oracle_tx_gen.cpp` only --
   **no change was needed to `oi_p2_host.cpp`/K3's `tx_scaling`**: pilot-based channel estimation
   is mathematically blind to any common gain applied identically to data and DMRS (the equalizer's
   `y*conj(h)/(tx_scaling*|h|^2)` cancels it out algebraically), which is exactly the point of using
   known pilots for channel estimation. Verified: all 3 MVP MCS points now pass the oracle's own
   self-check, and (see below) the full pipeline correctly recovers the true, unscaled TB.
5. **Arena-write gap**: `oi_p2_feed`'s own doc comment requires frame bytes to already be present
   in the arena at `desc->arena_offset` before `feed()` is called, but no function in the original
   `oi_p2_host.h` let any caller (a test harness, or eventually p3/p6's ingest_backend) actually
   place bytes there -- the arena `cl_mem` is private to `oi_p2_pipeline`. Found while designing
   `pipeline_runner.cpp` (which needs to feed real pcap-sourced frame bytes, not the garbage-filled
   arena `host_api_test.cpp`'s structural-only checks tolerate). Fixed additively: a new
   `oi_p2_write_arena(pipeline, offset, data, len)` function (2026-07-23), same P2-R17 reasoning
   already applied to the `mcs_index` addition -- nothing outside this project's own tests calls
   this API yet, so this is the cheapest point to close the gap, and it does not touch any existing
   signature.

## Design decisions made while wiring the real pipeline (flagged, not silently assumed)

1. **MCS conveyance: `oi_p2_launch_slot` gained an `mcs_index` parameter** (user-directed). See
   `../p2a-scaffold/src/host/oi_p2_host.h`'s doc comment on that parameter for the full rationale
   (MCS is a per-transmission MAC-scheduler decision, not derivable from PHY wire bytes or fixed
   for a pipeline's lifetime; `oi_p2_setup`/`oi_p2_feed`/`oi_p2_drain` are unchanged).
2. **TBS/code-rate values for the 3 MVP MCS points are a small lookup table** (`kMcsTable` in
   `oi_p2_host.cpp`), not a runtime port of TS 38.214 SS5.1.3.2's general TBS-quantization
   procedure. Values (TBS 4608/14600/27656 bits, code rate 0.3008/0.4785/0.6016) are real, confirmed
   via an actual OCUDU `tbs_calculator_calculate` run (STATUS.md), not estimated. This is the same
   class of decision as this project's other fixed-MVP-config simplifications (K1's hardcoded 51
   PRB, `oi_p2_buffers`' fixed dimensions) -- appropriate because P2-R11 rejects any config outside
   this exact fixed shape at setup time, so the general procedure would only ever be exercised at
   these 3 points anyway. If the MVP's MCS set is ever widened, this table (not a general
   TS 38.214 port) is the thing that needs revisiting.
3. **K6 + LDPC decode + CB desegmentation run as a host-orchestrated tail inside `oi_p2_drain`,
   not fully GPU-resident inside `oi_p2_launch_slot`.** K1 through K5 are fully GPU-resident,
   chained via real `cl_event`s on the in-order queue exactly as HLD specifies. K6 cannot be
   launched once for all codeblocks in a TB (different CBs can have different `rm_length` -- the
   "short vs long segment" rule -- so a single uniform-stride kernel launch across all CBs is
   wrong), and per-CB `clCreateSubBuffer` views into the shared LLR buffer risk violating
   device-specific alignment requirements for arbitrary bit-level offsets (a real portability risk
   on non-PoCL targets, not just a PoCL quirk to code around). Given `oi_p2_ldpc_decode_cb` was
   already built (this same slice, earlier) as a per-call-allocating function, extending that same
   pattern to K6 for this MVP was the pragmatic choice over engineering sub-buffer offset alignment
   for a case (heterogeneous per-CB rm_length) this project's OpenCL 1.2 portability floor doesn't
   guarantee is even safe. **This is a real, documented deviation from HLD SS5's "zero device
   allocations after setup"** for this specific (LDPC-adjacent) stage only -- K1-K5 fully honor
   that rule. Flagged for future work if PHYSICAL-tier performance requirements ever make this
   stage's allocation pattern matter (P2-R16: no such requirement exists at SIM tier today).
4. **Kernel source paths in `oi_p2_host.cpp` are hardcoded, CWD-relative paths to sibling
   features' `.cl` files** (e.g. `"../../p2c-k1/src/kernels/k1_depacketizer.cl"`), resolved only
   correctly when run from `p2a-scaffold/tests/`. `oi_p2_setup`'s signature is frozen (P2-R17), so
   this couldn't be parameterized without breaking that promise. Same CWD-relative fragility this
   project's own test suites already have (documented precedent, not a new pattern) -- but this is
   the first time it's inside *production* orchestration code, not just a test harness, so it's
   worth being explicit that a future real deployment packaging this pipeline needs to either run
   from that exact directory or the kernel-loading paths need revisiting (not urgent at SIM tier,
   where this is invoked from a fixed dev/test layout).

## P2-R15b oracle-vector generation and the growing-pipeline gate (2026-07-23)

5. **`oracle_tx_gen.cpp` has no OCUDU PUSCH *encoder* to call, because OCUDU is gNB-side/RX-only
   for the uplink** (confirmed by grepping the tree: `pusch_processor`/`pusch_decoder`/
   `pusch_demodulator` exist, no `pusch_encoder`/`pusch_modulator`). The TX chain this generator
   needs (segment -> per-CB LDPC-encode -> rate-match -> concatenate -> scramble -> modulate) is
   the same TS 38.212/38.211 procedure PDSCH's real TX chain (`pdsch_encoder_impl.cpp`,
   `pdsch_modulator_impl.cpp`) already implements for the downlink direction. Consulted those two
   files **read-only** for the call sequence and the c_init formula (HLD D9 precedent: same
   read-only-consultation status as K1's reading of OCUDU's O-RAN decoder for field semantics) --
   every individual primitive actually called (`ldpc_segmenter_tx`, `ldpc_encoder`,
   `ldpc_rate_matcher`, `pseudo_random_generator`, `modulation_mapper`, the eCPRI/O-RAN builders)
   is the real, unmodified, factory-constructed OCUDU implementation; nothing hand-reimplements a
   3GPP procedure. This is the same "read for the call sequence, use the real API for every actual
   computation" boundary this project draws consistently, not a new kind of exception.
6. **Wire container format**: `oracle_tx_gen.cpp` writes a plain, standard libpcap file (global
   header + per-packet records) directly, with no library dependency. This is a mechanical,
   standardized binary container format, not OCUDU-adjacent expression -- unlike the eCPRI/O-RAN
   payload bytes themselves (built exclusively via real OCUDU builders), there was nothing here
   worth routing through a second implementation to avoid.
7. **P2-R1 (prefix buildability) scope evolution**: the original design let each pipeline prefix
   (K1-only, K1-K2, ...) build and test independently against dummy downstream stages. The
   2026-07-23 stub-replacement wires all 7 real kernels into one `oi_p2_setup` call, so there is no
   longer a separate "K1-only" artifact to test in isolation -- a real, disclosed scope change (same
   class as p2d's earlier 7->8 stage bump), not a silent gap. `pipeline_test.py` checks what P2-R1
   actually cared about observably -- every intermediate buffer (I2..I5) independently inspectable
   -- via `pipeline_runner`'s `taps_ok` field, now over real oracle-packed data rather than a
   dummy sink.
8. **P2-R17 test substitute**: the LLD's literal test ("p3's live-tap-ul-inject stub and p4's seam
   stub... link against oi_p2_host.h unchanged") can't be literally built yet -- neither p3 nor p4
   exist. `pipeline_test.py`'s `check_p2_r17_api_surface()` grep-checks that `pipeline_runner.cpp`
   (itself standing in for a production ingest_backend) calls only the documented public surface,
   including the 2026-07-23 `oi_p2_write_arena` addition -- P2-R17 protects existing callers from a
   signature change after they depend on it, it does not forbid additive new calls before anyone
   does (same reasoning as the `mcs_index` addition).
9. **Class-a (P1-captured) gate is skipped, not failed**: `p1-ran-baseline` has no implementation
   yet (`STATUS.md`: `spec_only`), so no real captured pcap corpus exists. `pipeline_test.py` checks
   for a captures directory and prints an explicit `SKIP` with the reason rather than silently
   omitting the check or fabricating a pass -- matches this project's "no silent gaps" discipline,
   and the deferral was already anticipated in `README.md`'s "Note on p1 dependency".

**Result**: `pipeline_test.py` -- 21/21 assertions pass (7 per MCS point x 3 MCS points, plus 2
P2-R17 surface checks), covering real CRC24A pass + bit-exact TB recovery end-to-end for all three
MVP MCS points. Full p2 re-verification sweep after this work (`lint_no_perf.sh`,
`lint_portability.py`, `provenance_check.py`, and every p2a-p2f test suite) re-run clean.

## Known-open items (not addressed this pass)

- **p3/p6 push-vs-pull ingest control-flow inversion**: still flagged, still not resolved (same
  status carried forward from p2c-k1/p2a-scaffold).
- **Ethernet-layer wire assumption (Q2)**: still open, unaffected by this slice's work.
- **Class-a P1-captured structural gate**: implemented in `pipeline_test.py` but untested against
  real data (skipped, see item 9 above) until `p1-ran-baseline` produces a real pcap corpus.
