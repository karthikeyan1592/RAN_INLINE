# p4-phy-l2-seam — SPEC

**Feature:** SIM phase P4 — the PHY↔L2 seam (shared-memory ring IPC).
**Tier:** SIM (T1). **Authority:** [`ARCHITECTURE_v3_SIM.md`](../../../ARCHITECTURE_v3_SIM.md)
§2/§3/§4 (P4 row is the gate source); master:
[`ARCHITECTURE_v3.md`](../../../ARCHITECTURE_v3.md) §5 (handoff), §7 (PHY↔L2 seam),
§11 (Phase-2 CXL scope, locked but not built).

**Backend statement (SIM §3):** P4 provides the SIM-tier **`handoff_backend`**: a plain,
synchronous `memcpy` of the decoded TB+CRC record into a shared-memory ring, consumed by a new
CPU **L2 stub** (project code) in a separate container. P4 consumes `compute_backend` output only
through `p2-phy-kernels`' drain contract (opaque, not redefined here) and, transitively, whatever
`ingest_backend`/injection `p3-live-tap-ul-inject` feeds that pipeline. P4 touches no
`ingest_backend` or `compute_backend` code itself.

## Purpose

Give the GPU-resident PHY (`gpu-phy`, running the `p2-phy-kernels` pipeline over frames delivered
by `p3-live-tap-ul-inject`) a real, byte-precise handoff to a CPU-side L2 stub: decoded TB bits +
CRC verdict + slot/UE metadata, delivered in-order, over a shared-memory ring living in a separate
container (`l2-stub`) from `gpu-phy` — proving the v3 §7 "custom IPC" seam and the v3 §5 handoff
leg at the SIM tier, with a wire format that is **already compatible with the Phase-2 CXL swap**
(v3 §11) without building Phase-2 now.

## In scope

- The ring wire format itself: header, slot layout, atomic status field, epoch/sequence fields —
  byte-precise (LLD §Data structures).
- The `oi_seam` host library: reserve/publish/wait/release API used by both the producer
  (inside `gpu-phy`) and the consumer (`l2-stub`).
- A new, small, project-authored **L2 stub** consumer: attaches to the ring, validates ordering
  and CRC verdicts, exposes pass/fail counters for the P4 gates.
- The shared-memory backing mechanism: a named, volume-persisted segment (not container tmpfs,
  not `ipc: container:X`) mounted identically into `gpu-phy` and `l2-stub`.
- Unit test harnesses for ordering, wrap, and restart (including a synthetic out-of-order
  completion injector, since the real P2/P3 pipeline is single-slot-in-flight in the MVP and would
  never organically exercise reordering — see Honesty notes).
- Config schema for ring sizing (capacity, slot size, TB max bytes, format version).

## Out of scope

- p2's kernel pipeline and its drain API internals (`p2-phy-kernels`, opaque dependency).
- p3's ingest/injection internals (`p3-live-tap-ul-inject`, opaque dependency); p3's own
  direct-drain verification harness (P3-R11) is unaffected by this feature.
- Real SCF FAPI / packed-FAPI message encoding — **rejected as an implementation target**
  (commercially gated in srsRAN/OCUDU open source, confirmed in prior research; HLD rejected
  alternatives). SCF FAPI 222 is consulted only as a semantics reference for what fields a PHY↔L2
  boundary conventionally carries.
- Real DU/gNB MAC integration ("DU integration point" per SIM §4 P4 row) — the L2 stub is a test
  consumer, not an OCUDU MAC integration; that remains future work, not scoped here.
- The PHYSICAL `handoff_backend` implementation (pinned-buffer async DMA + completion event) —
  `p6-physical-m1-ingest`'s concern; this spec only requires the ring format to be satisfiable by
  it (P4-R5).
- The Phase-2 CXL-backed region itself (v3 §11) — locked scope, not built; this spec only requires
  the ring format not to structurally preclude it (P4-R5).
- Any latency/throughput requirement — **forbidden at SIM tier** (SIM preamble rule).

## Requirements

Every requirement is testable; test mapping in LLD §Test plan.

| ID | Requirement |
|---|---|
| **P4-R1** | The seam SHALL be a shared-memory ring (`oi_seam_ring`): a fixed-size header (magic, `format_version`, `epoch`, `ring_capacity`, `slot_bytes`, atomic `head`/`tail` counters) followed by `ring_capacity` fixed-size slots. Byte layout is defined and versioned in LLD §Data structures. |
| **P4-R2** | Each slot SHALL carry, at minimum: an atomic `status`, an `epoch` stamp, a monotonic `seq`, `(sfn, slot, rnti, harq_id)`, `crc_ok`, `tb_len`, and up to `OI_SEAM_TB_MAX_BYTES` TB payload bytes. A producer-side enqueue timestamp MAY be present but is observational only and SHALL NOT participate in any pass/fail decision. |
| **P4-R3** | The `status` field SHALL be a plain atomic integer, written by the producer with **release** ordering after every other slot field is written, and read by the consumer with **acquire** ordering before any other slot field is read — the same discipline as the CXL PoC's `e2e_slot_t` status field (cited precedent, v3 §11). Correctness SHALL NOT depend on any OS-level completion-event or interrupt mechanism. |
| **P4-R4** | The SIM `handoff_backend` implementation SHALL be a plain synchronous `memcpy`: the producer computes the TB+CRC record, `memcpy`s it into the reserved slot, then publishes `status`. No async DMA, no GPU-API completion event, and no GPU-API type SHALL appear in the ring format or in the consumer's read path. |
| **P4-R5** | The ring format SHALL be handoff-mechanism-agnostic: satisfiable by (a) SIM plain memcpy + synchronous status flip (this feature), (b) PHYSICAL pinned-buffer async DMA + a completion-event callback that flips the same `status` field (p6, later), and (c) a future CXL-backed region where the consumer only **polls** the same `status` field, with no completion-event primitive available (Phase-2, v3 §11, not built now). No requirement in this feature SHALL assume (a) is the only satisfying mechanism. |
| **P4-R6** | **Per-key ordering:** for any fixed `(rnti, harq_id)` key, the sequence of TB records observable by scanning the ring from `tail` to `head` SHALL have monotonically increasing `(sfn, slot)`, even if the `compute_backend` completes TB records for *different* keys out of the order their slots' ingest completed. The producer is responsible for this guarantee before ring insertion (LLD §Design). |
| **P4-R7** | **Wrap semantics:** `ring_index = seq mod ring_capacity`. The producer SHALL NOT overwrite a slot whose `status` is not free; reservation SHALL block (bounded wait, no deadline — SIM tier) until the consumer advances `tail`. The ring SHALL never silently drop or overwrite an unconsumed TB record. |
| **P4-R8** | **Restart semantics (producer):** if the producer (re)initializes the ring — fresh start or crash recovery — it SHALL increment `epoch` and reset `head=tail=0` before publishing any slot. A consumer SHALL detect an `epoch` change and discard any prior per-key ordering state, treating the new epoch as a fresh stream. |
| **P4-R9** | **Restart semantics (consumer):** if the L2 stub restarts, it SHALL reattach to the existing ring segment (unchanged `epoch`), resume from the persisted `tail`, and SHALL NOT re-deliver any slot at or before its last-known `tail`, nor skip any slot the producer published since. |
| **P4-R10** | The ring segment SHALL be backed by a named, volume-persisted shared-memory mapping (not container-local tmpfs `/dev/shm`, not `ipc: container:X` namespace coupling), so either container may restart independently without destroying the segment (LLD records the exact mechanism). |
| **P4-R11** | A CPU **L2 stub** (new, project-authored, minimal) SHALL attach to the ring, validate each record's CRC verdict and per-key ordering, and expose pass/fail counters consumed by the P4 gates. It SHALL NOT implement SCF FAPI / packed-FAPI message encoding (out of scope; HLD rejected alternatives). |
| **P4-R12** | P4 SHALL consume the TB+CRC record already defined at the `p2-phy-kernels` drain boundary (I8, `oi_p2_tb_record` via `oi_p2_drain`: real fields `slot_id, tb_size_bytes, nof_cb, base_graph, crc24a_ok, mcs_index, tb_data` — per `p2-phy-kernels` LLD §4.7) as an opaque given. **Note:** the record itself carries no `sfn`/`rnti`/`harq_id` — the MVP config pins these to constants (fixed RNTI, single-shot HARQ, single UE), so the seam-writer populates `oi_seam_ring`'s `(sfn, slot, rnti, harq_id)` fields (P4-R2) from the pinned config + `slot_id` (`sfn = slot_id / slots_per_frame`), not by reading them off p2's record. This feature SHALL NOT redefine or modify p2's or p3's APIs. |
| **P4-R13** | No requirement, gate, config, or test in this feature SHALL contain a latency or throughput threshold (SIM preamble rule); any timestamp fields present are recorded for future PHYSICAL analysis only and are labeled non-quotable. |
| **P4-R14** | Ring configuration (`ring_capacity`, `slot_bytes`, TB max bytes, `format_version`) SHALL be fixed by one YAML/env schema (LLD §Configuration); any mismatch between producer and consumer configuration SHALL be detected at attach time via the header and fail fast with a structured error, never silently truncate or corrupt data. |

## Acceptance gates

Traceability: SIM §4, row **P4**. Definition of done (SIM §4 footer): gates green on WSL2 **and**
the GCP `n2-standard-16` VM; results logged to the honesty ledger (manually until p5 automates it).

| Gate | Type | Statement | SIM §4 source |
|---|---|---|---|
| **P4-G1** | unit | **Ordering:** synthetic producer harness enqueues records for ≥2 `(rnti,harq_id)` keys with intentionally out-of-order completion; consumer asserts per-key `(sfn,slot)` monotonicity holds (P4-R6). | P4 "Test gate (unit): ordering" |
| **P4-G2** | unit | **Wrap:** drive the ring past `ring_capacity` with a paused/slow consumer; assert the producer blocks (no overwrite, no silent drop) and resumes correctly once the consumer catches up (P4-R7). | P4 "Test gate (unit): wrap" |
| **P4-G3** | unit | **Restart:** (a) kill/respawn the producer mid-run — assert `epoch` bump + consumer resync (P4-R8); (b) kill/respawn the consumer mid-run — assert reattach at the correct `tail`, no duplicate delivery, no loss (P4-R9/R10). | P4 "Test gate (unit): restart" |
| **P4-G4** | integration | **End-to-end slot processing:** injected UL (p3) → p2 GPU pipeline → TB delivered through the ring with correct CRC verdicts, L2 stub reports 0 mismatches over a configured run. | P4 "Integration gate" |

## Dependencies on other features

- **`p2-phy-kernels`** — opaque dependency: this feature consumes only the TB+CRC record at the
  drain boundary (I8); no p2 internals are defined or constrained here.
- **`p3-live-tap-ul-inject`** — opaque dependency: p4 sits immediately downstream of the same drain
  calls p3's own bit-exact harness (P3-R11) already consumes directly; p4 is **additive** — it does
  not replace or alter p3's verification path. Which process/thread owns the drain-and-publish call
  site (gpu-phy's own loop vs. a thin adapter) is left open (LLD §Open questions).
- **`p0-rig-scaffold`** — provides the `gpu-phy` image/container this feature extends (additive
  volume mount) and the compose base the new `l2-stub` service attaches to.
- Consumed later by **`p5-one-command-rig`** (wraps P4-G1..G4 into the `make simtest` phase suite)
  and **`p6-physical-m1-ingest`** (realizes the PHYSICAL `handoff_backend` against this same ring
  format) and, eventually, Phase 2 (CXL transport swap under this same format, v3 §11 — not built
  now).

## Honesty-ledger notes (what P4 does NOT prove)

- **Not a DU/MAC integration.** The L2 stub is a minimal test consumer, not OCUDU's MAC; no claim
  is made about interop with a real unmodified L2 stack (SIM §4 P4 row's "DU integration point" is
  explicitly future work, not built here).
- **The out-of-order-completion gate is synthetic.** Per `p2-phy-kernels` HLD D4, the real MVP
  pipeline is slot-granular with one in-flight slot — it never organically produces out-of-order
  completion across keys. P4-G1 exercises the ordering guarantee via an injected test harness, not
  organic production traffic; this is stated plainly, not hidden.
- **Not proof that CXL Phase-2 works.** P4-R5 shows the ring format doesn't structurally preclude a
  polling-only CXL consumer — it is not a functional test of any real CXL region, and no performance
  or feasibility claim about Phase-2 follows from this feature.
- **Not proof of the PHYSICAL `handoff_backend`.** Pinned-buffer async DMA + completion-event
  behavior on real hardware is entirely `p6`'s concern; nothing here has run against a GPU-API
  completion event of any kind.
- No performance evidence of any kind (SIM preamble rule; P4-R13).
