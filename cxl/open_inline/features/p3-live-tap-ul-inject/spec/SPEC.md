# p3-live-tap-ul-inject — SPEC

> **PIN UPDATE (2026-07-19):** upstream re-pinned srsRAN Project `release_24_10_1` → **OCUDU `release_26_04`** (`gitlab.com/ocudu/ocudu`, BSD-3). Read `../../../research/ocudu_repin.md` before implementing. Deltas: `ru_emulator` moved to `apps/examples/ofh/`; namespace/headers `srsran`→`ocudu`; golden conformance vectors absent in OCUDU (dual-oracle rule: srsRAN AGPL vectors CI-only, Sionna vectors shippable). Mentions of "srsRAN Project" below refer to OCUDU unless explicitly about the legacy AGPL checkout.

**Feature:** SIM phase P3 — ru_emulator oracle UL injection + gpu-phy live fronthaul tap.
**Tier:** SIM (T1). **Authority:** `ARCHITECTURE_v3_SIM.md` §2/§3/§4 (P3 row), `ARCHITECTURE_v3.md` §2/§6.3(b),
`research/phase1_feasibility_cloud_hw.md` §2.3.

**Backend statement:** this feature **provides the SIM-tier `ingest_backend`**
(af_packet RX on the fronthaul bridge + memcpy, per SIM §3). It **consumes** the
`compute_backend` only indirectly, through the opaque `p2-phy-kernels` pipeline API (§Dependencies).
It does **not** touch the `handoff_backend`.

---

## Purpose

Turn the P1 rig's fronthaul from "real protocol, dummy data" into "real protocol, **known** data":

1. Patch OCUDU `ru_emulator` (pinned `release_26_04`, `apps/examples/ofh/`), which today generates
   **static UL IQ** (pre-generated random bytes per eAxC/symbol, reused every slot), so it injects
   **externally supplied oracle RE grids** as the UL U-plane IQ payload — byte-identical to the
   oracle files.
2. Have `gpu-phy` **tap the live fronthaul bridge passively** (af_packet) and decode the live eCPRI
   stream **bit-exact** against oracle expected TBs, while the unmodified DU (`gnb`) continues to
   consume the same stream undisturbed.

This is v3 milestone M3 realized at the SIM tier: injector → wire → GPU pipeline → decoded TB,
bit-exact vs ground truth, with zero exotic hardware.

## In scope

- Patch series against OCUDU `release_26_04` `ru_emulator` (`apps/examples/ofh/`, namespace
  `ocudu`): oracle-grid file loader, slot→file schedule, UL U-plane payload substitution, YAML
  config extension.
- **Stretch deliverable (post-re-pin):** OCUDU accepts contributions — prepare the injection patch
  as an upstreamable, config-gated, default-off feature and submit it as a merge request. Stretch
  only; no gate depends on upstream acceptance.
- Oracle RE-grid **file format** (wire-format payload sections; defined byte-precisely in LLD §3).
- `gpu-phy` SIM `ingest_backend`: af_packet RX bound to the container's fronthaul interface,
  ethertype filter `0xAEFE`, delivery into the p2 pipeline feed API.
- Rig plumbing that makes DU-bound unicast frames visible to the tap (bridge hub-mode or tc mirror
  — HLD §Design decisions).
- Verification tooling: pcap byte-comparator (injected payload vs oracle file), live bit-exact
  harness, DU-undisturbed checker.

## Out of scope

- p2 pipeline internals (kernels, golden vectors) — opaque dependency; only its feed/drain
  contract is stated here.
- The PHY↔L2 seam (p4) — p3's harness reads decoded TBs via the p2 drain API directly.
- Any PHYSICAL-tier ingest (raw QP / dmabuf / DPDK) — p6.
- DL-direction injection; C-plane changes; HARQ retransmission data.
- Any latency or throughput requirement — **forbidden at SIM tier** (SIM preamble rule).

## Requirements

Every requirement is testable; test mapping in LLD §6.

| ID | Requirement |
|---|---|
| **P3-R1** | The ru_emulator modification SHALL be a patch series against OCUDU tag `release_26_04` that applies cleanly (`git apply --check` green in CI against a pristine pinned checkout). |
| **P3-R2** | Patched ru_emulator SHALL, when injection is enabled by config, source every UL U-plane section IQ payload from an oracle grid file set (format: LLD §3.1) instead of the static-IQ generator. |
| **P3-R3** | For every injected U-plane section, the IQ payload bytes on the wire SHALL be **byte-identical** to the corresponding oracle-file section payload (verified frame-by-frame from a bridge pcap by the comparator tool; 0 mismatching sections). ru_emulator IQ compression config MUST equal the oracle file's `iq_format` (pinned: uncompressed 16-bit, no `udCompHdr`) — a mismatch is a startup error, not silent re-encode. |
| **P3-R4** | With injection **disabled**, patched ru_emulator SHALL be behaviorally identical to upstream: same config schema accepted, and the P1 integration gate (real eCPRI on bridge, 10-min stability) still passes with the patched binary. |
| **P3-R5** | The patch SHALL NOT modify C-plane generation, slot timing logic, or DL handling. Diff scope is verifiable by review + a C-plane pcap field-compare (patched vs upstream run) showing no structural differences. |
| **P3-R6** | Slot→oracle-file mapping SHALL be deterministic: file index = `(sfn*slots_per_frame + slot) mod N` over the configured file list, so the oracle harness can compute the expected TB for any captured slot independently. |
| **P3-R7** | `gpu-phy` SHALL implement the SIM `ingest_backend` as af_packet (`SOCK_RAW`, `ETH_P_ALL` bind + BPF/ethertype filter `0xAEFE`) on its fronthaul interface, exposed through the common `oi_ingest` API (LLD §2 — same signatures as p6's PHYSICAL implementations). The BPF filter SHALL match `0xAEFE` at byte 12 (untagged) OR byte 16 (802.1Q-tagged, TPID `0x8100` at byte 12) — not a single fixed offset (LLD §2's `oi_ingest_open_af_packet` note, 2026-07-24: p1's ru_emulator has no untagged `--vlan_tag` option, so a real captured frame is expected to always carry a tag). |
| **P3-R8** | The tap SHALL be passive: gpu-phy transmits nothing on the fronthaul network and never modifies frames in flight. |
| **P3-R9** | The rig SHALL make all DU-bound `0xAEFE` unicast frames visible at gpu-phy's interface (bridge hub-mode `ageing_time 0`, or tc `mirred` mirror fallback). Verified: tap frame count == ru-emu TX U-plane frame count over a counted run. |
| **P3-R10** | The ingest SHALL deliver frames to the p2 pipeline feed API in kernel-delivery order. Each frame's RX timestamp (`CLOCK_MONOTONIC_RAW` at reap) SHALL be recorded in the ingest's own internal log (LLD §Data structures, `oi_ingest_rx_log_entry`) — **not** part of `oi_p2_feed`/`oi_frame_desc`, which carry no timestamp field by design (`p2-phy-kernels` LLD §4.1) — and consumed only by this feature's own verification tooling (P3-U3/P3-I1). |
| **P3-R11** | **Live bit-exact:** over a configured run (default ≥1000 injected UL slots), every decoded TB drained from the p2 pipeline SHALL be bit-identical to the oracle expected TB for that (sfn, slot), with the expected CRC verdict. Zero mismatches. |
| **P3-R12** | **DU undisturbed:** during the R11 run plus a 10-minute soak with the tap active, `gnb` SHALL show no crash, no new ERROR-level logs vs the P1 baseline, and stable test-mode UE / ru_emulator KPI counters (same pass criteria as the P1 integration gate). |
| **P3-R13** | The ingest SHALL expose counters (frames seen / ethertype-matched / delivered / socket-level drops via `PACKET_STATISTICS`). A verification run with nonzero socket drops is **invalid and rerun** (drops break bit-exact accounting) — this is a validity condition, not a performance number. |
| **P3-R14** | No latency/throughput requirement exists in this feature; any timing printed by tools SHALL be labeled `SIM — not quotable`. |

## Acceptance gates

Traceable to SIM §4, row **P3**. Definition of done: both gates green on WSL2 **and** the GCP VM
(SIM §4 DoD), results logged to the honesty ledger.

**Unit gates**
- **P3-U1 (SIM §4 P3 unit gate):** "injected frames byte-identical to oracle grids" — capture
  bridge pcap during an injection run; comparator reports 0 payload mismatches across all U-plane
  sections (P3-R3, P3-R6).
- **P3-U2:** patch hygiene — applies cleanly on pinned tag; injection-disabled regression equals
  upstream behavior (P3-R1, P3-R4, P3-R5).
- **P3-U3:** ingest unit — replay a canned P1 pcap into a veth pair; `oi_ingest` delivers exactly
  the `0xAEFE` frames, in order, with correct lengths and stats (P3-R7, P3-R10, P3-R13).

**Integration gate**
- **P3-I1 (SIM §4 P3 integration gate):** "gpu-phy decodes the **live** stream bit-exact while DU
  runs undisturbed" — full compose rig up (5gc + gnb + patched ru-emu + gpu-phy), live run:
  P3-R9 + P3-R11 + P3-R12 + P3-R13 all pass in one session.

## Dependencies on other features

- **`p1-ran-baseline`** — provides the running rig (fronthaul bridge, gnb test-mode UEs, KPI
  baseline used by P3-R12) and the pcap corpus for P3-U3.
- **`p2-phy-kernels`** — **opaque dependency.** p3 does not define or constrain p2's internals.
  p3 requires only this feed/drain contract of the pipeline (final naming owned by the p2 spec;
  shapes here are p3's minimum demands):
  - *feed:* accept one raw Ethernet frame (eCPRI, as captured) plus RX timestamp; non-blocking with
    a backpressure return code (`oi_p2_feed`, LLD §2.2);
  - *drain:* yield decoded TB results via `oi_p2_tb_record` (`oi_p2_drain`, LLD §2.2) — real
    fields per `p2-phy-kernels` LLD §4.7: `slot_id, tb_size_bytes, nof_cb, base_graph,
    crc24a_ok, mcs_index, tb_data`. **No `sfn`/`rnti`/`harq_id` fields exist in the record** —
    the MVP config pins `RNTI = 0x4601` and single-shot HARQ (`harq_id` constant, no combining)
    for the entire run, so p3's harness derives `sfn = slot_id / slots_per_frame` from the
    pinned numerology and treats `rnti`/`harq_id` as the fixed config constants, not per-record
    fields, when matching a drained TB back to its oracle file (LLD §Data structures);
  - accept frames for the pinned rig configuration (numerology, PRB count, eAxC set) used by P1/P3.
- **`p0-rig-scaffold`** — container images (gpu-phy with PoCL, build of patched ru_emulator).
- Consumed later by **`p4-phy-l2-seam`** (replaces p3's direct drain with the seam ring) and
  **`p5-one-command-rig`** (automates P3-I1).

## Honesty-ledger notes (what this feature does NOT prove)

- **Protocol-real / data-synthetic** (feasibility §2.3): no protocol-attached live UE traverses the
  fronthaul; UL payload is replayed oracle data. That is the claim's designed shape, not a defect —
  but say it plainly.
- Proves nothing about NIC→VRAM ingest (p6/M1), DPDK, flow steering, or line rate: af_packet on a
  veth bridge is functionally real eCPRI, mechanically nothing like a ConnectX.
- PoCL executes the kernels — no real-GPU claim from this phase (SIM §3.1 ladder).
- No performance numbers of any kind leave this feature (SIM preamble rule; P3-R14).
- The tap topology (hub-mode/mirrored Linux bridge) has no PHYSICAL analogue — on hardware the
  ingest owns its own port/flow-rule (p6); only the `oi_ingest` API shape carries over.
