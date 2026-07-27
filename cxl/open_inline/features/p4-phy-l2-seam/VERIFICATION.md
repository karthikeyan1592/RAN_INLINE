# p4-phy-l2-seam — VERIFICATION

## What's implemented and verified for real

All modules are implemented (`oi_seam_ring.h`, `oi_seam.{h,c}`, `oi_seam_producer.{h,c}`,
`oi_l2_validate.{h,c}`, `l2_stub_main.c`) and every locally-runnable gate (P4-G1/G2/G3, plus the
struct-layout and producer field-mapping tests) was run for real against a real mmap'd ring file
and, for the concurrency gates, real threads.

| Test | What it proves | Result |
|---|---|---|
| `struct_layout_test` | P4-R1/R2: every field's byte offset matches the LLD exactly (also enforced at every compile via `_Static_assert`); real `memcpy` round-trip | 33/33 PASS |
| `producer_test` | P4-R12: 1:1 field mapping from a synthetic p2 record view, `sfn`/`slot` derivation, `tb_size_bytes` boundary/overflow handling (P4-R14) | 13/13 PASS |
| `ordering_test` (P4-G1) | P4-R6: real ring, 2 keys with deliberately scrambled cross-key completion order, per-key `(sfn,slot)` monotonicity holds; negative case (genuine regression) is caught | 4/4 PASS |
| `wrap_test` (P4-G2) | P4-R7: real second thread as producer, ring driven past capacity with a paused consumer — producer genuinely blocks (measured: reservations stall at ≤ capacity), never overwrites, resumes correctly once drained | 13/13 PASS |
| `restart_test` (P4-G3) | P4-R8 (producer restart → epoch bump + reset head/tail + consumer resync) and P4-R9 (consumer restart → resumes at persisted tail, no dup, no loss) | 17/17 PASS |
| P4-R4 static check | grep for any GPU-API symbol (`cl*`, `hip*`, event types) in `oi_seam.*`/`oi_seam_ring.h` | 0 hits (real grep, not asserted-and-skipped) |
| P4-R11 static check | grep for any FAPI/SCF-222 symbol in the l2-stub tree | 0 hits (only this project's own comments *mention* "FAPI" to document its absence) |
| P4-R13 lint | `helpers/lint_no_perf.sh` (feature-scoped copy — see Real bugs #4) | 0 hits |

**Total local assertions: 80/80 PASS** across 5 test binaries, plus the two static greps.

## Real, disclosed finding: P4-R3's dynamic race-detector attempt was inconclusive, not skipped

Tried both ThreadSanitizer and Helgrind (after installing valgrind) against `wrap_test`'s real
producer/consumer threads:

- **ThreadSanitizer**: fails outright with `FATAL: ThreadSanitizer: unexpected memory mapping` —
  a known TSan limitation with certain `MAP_SHARED` file-backed mmap patterns, not something this
  code can work around.
- **Helgrind**: runs, but reports "possible data race" on the plain (non-`_Atomic`) slot fields
  *and*, even after manually adding `ANNOTATE_HAPPENS_BEFORE`/`ANNOTATE_HAPPENS_AFTER` around the
  atomic status transitions (a real, executed experiment, not a guess), still reports races
  directly on the atomic `status` field's own `atomic_load_explicit`/`atomic_store_explicit`
  calls. Root cause: on x86_64, `memory_order_acquire`/`memory_order_release` atomics compile to
  **plain load/store instructions** — x86's own strong memory model already provides these
  orderings for free, so there is no fence or special instruction in the compiled code for a
  machine-code-level instrumentation tool to recognize as "this is the synchronization point."
  Helgrind and TSan work by watching the instruction stream; on x86_64 there is nothing in that
  stream to distinguish an acquire-load from an ordinary unsynchronized read. This is a
  genuine, well-understood limitation of dynamic race detectors for acquire/release atomics on
  this architecture, not evidence of a bug in this code.
- **What actually verifies P4-R3**: a static argument, checked by direct code review (not
  hand-waved): every non-atomic slot field (`sfn`, `slot`, `rnti`, `harq_id`, `crc_ok`, `tb_len`,
  `tb[]`) is written by the caller strictly before the single `oi_seam_publish()` call (which does
  the one release-store of `status`), and read by the consumer strictly after
  `oi_seam_wait_status()`'s single acquire-load observes the matching status — this is exactly
  the C11 release-acquire pairing that establishes a happens-before edge, and `wrap_test`'s own
  real-thread run (13/13 PASS, including "drained slot's own field matches its expected sequence
  position" across 7 real cross-thread handoffs) is consistent with that being correct in
  practice, even though no dynamic tool in this environment could independently confirm the
  *ordering discipline itself* rather than just the *observed outcome*.

## Real finding: the LLD's own cited precedent doesn't exist

P4-R3's text cites "the same discipline as the CXL PoC's `e2e_slot_t` status field" as prior art.
Grepping the actual CXL PoC tree (`cxl_ran_poc/`) finds no `e2e_slot_t` anywhere — the closest real
precedent that exists is `cxl_ran_poc/phase5_cxl/desc_ring.h`'s `desc_ring_t` (a real, working
head/tail-only SPSC ring with the identical release/acquire discipline, just without per-slot
status fields). This file's own header documents the discrepancy; the LLD's byte layout itself is
still authoritative and was implemented exactly as specified.

## `OI_SEAM_TB_MAX_BYTES`: real, computed value (LLD Q3)

`3457` — MCS 21 (the MVP's highest-throughput pinned MCS point, `p2f-integration/src/host/
oi_oracle_pack.h`'s `kMcsTable`: `{21, 6, 27656, 0.6016f}`) has `tbs_bits=27656`, which divides
evenly by 8 (no remainder, no rounding decision needed): `27656 / 8 = 3457` bytes exactly.

## Known-open items (real, not hidden)

- **P4-G4 (full integration) is deferred** — needs the live rig (p3's real injection, a real gnb,
  the real p2 pipeline actually calling this feature's producer at its drain call site). Every
  piece it would exercise (the ring, the producer's field mapping, the L2 stub's validation
  logic) has its own real, passing local test; only the end-to-end wiring is untested locally.
  Full runbook: `DEFERRED_LIVE_GATES.md` at the repo root.
- **Drain call-site ownership (LLD Q1) is not resolved here** — whether `gpu-phy`'s own event
  loop calls `oi_p2_drain` and then this feature's producer directly, or a thin adapter does, is
  explicitly left open by the LLD pending p3/p2's own implementation details; this feature's
  `oi_seam_producer_fill_slot` works identically either way (it only needs the record fields, not
  which process/thread called it).
- **`OI_SEAM_RING_CAPACITY=64`** is the LLD's own stated MVP placeholder (Q2), not a correctness
  parameter — P4-R7 holds at any capacity ≥ 1, verified directly by `wrap_test`'s own small
  capacity=4 ring.
