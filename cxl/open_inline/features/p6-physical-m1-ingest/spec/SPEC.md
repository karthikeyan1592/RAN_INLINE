# p6-physical-m1-ingest — SPEC

> **PIN NOTE:** where this feature drives real eCPRI traffic (M1 spike step 4, wiring modes 1–4),
> the sender is OCUDU `release_26_04` (`gitlab.com/ocudu/ocudu`, BSD-3) `ru_emulator`
> (`apps/examples/ofh/`), per `../../../research/ocudu_repin.md`. Steps 1–3 and 5 of the M1 spike
> need no RAN stack at all — see below.

**Feature:** PHYSICAL PHY-0/PHY-1 — day-1 box verification, the M1 NIC→VRAM ingest spike, and the
production `ingest_backend` (PHYSICAL implementation) with its CPU-staged fallback.
**Tier:** PHYSICAL (T3). **Authority:** `ARCHITECTURE_v3_PHYSICAL.md` §2–§5 (data-path deltas,
day-1 checklist, milestones, risks); `research/physical_deep_feasibility.md` (primary spec source:
§1 M1 chain, §1.1 probe order, §3 wiring decision tree, §6 checklist additions); `ARCHITECTURE_v3_SIM.md`
§3 (backend contract, normative) and §5 (gap ledger — the list this feature exists to close);
`research/phase1_feasibility_cloud_hw.md` §3–§5 (hardware ladder, NIC-vendor fork, risk register).

**Backend statement:** this feature **provides the PHYSICAL-tier `ingest_backend`** (SIM §3 row:
`mlx5 raw-packet QP + ibv_reg_dmabuf_mr → VRAM`) through the common `oi_ingest` API first named in
`p3-live-tap-ul-inject` (SIM implementation, af_packet) — **p6 is the API's other implementation,
not a second API.** It also provides a second, config-selected implementation of the same API
(CPU-staged: DPDK/af_packet RX → pinned host buffer → GPU memcpy) as the freeze-breaker fallback.
p6 does **not** touch `compute_backend` (vendor ICD swap is env-only, owned by p2) or
`handoff_backend` design (pinned-DMA handoff is v3 §5 / p4's seam concern) beyond invoking the
existing handoff at the ingest→compute boundary.

---

## Purpose

Prove and then productionize the one PHYSICAL-only mechanism SIM structurally cannot exercise
(SIM §5 gap #1): real Ethernet frames landing directly in GPU VRAM via a raw-packet QP and a
dmabuf-backed memory region, with no RDMA fabric, no cable, and (for the M1 spike itself) no VFs.
Concretely: (a) a day-1 box-verification suite that any new PHYSICAL box must pass before spike
work starts; (b) the M1 spike, run in the exact link order the deep-feasibility study derived,
with a bounded freeze-breaker if the one unproven junction (link 4: dmabuf MR usable by a
raw-packet QP's RX ring) does not close; (c) a production `ingest_backend` — VRAM RX-buffer ring
plus CPU-staged fallback — implementing the SIM §3 contract so `compute_backend` code is
identical regardless of which ingest path is live; (d) a wiring-mode selector formalizing the
deep-feasibility §3 decision tree as explicit, probed configuration.

## In scope

- Day-1 verification probes 1–10 (PHYSICAL §3 items 1–6 + deep-feasibility §6 items 7–10), each
  with a PASS/FAIL criterion, run on every new PHYSICAL box before any M1 or production work.
- The M1 spike: the 5-step probe order of deep-feasibility §1.1, run on Stage-A (OCI
  `BM.GPU.A10.4`, NVIDIA) first; steps 1–4 are vendor-parametrized (NVIDIA/AMD) and repeat on
  Stage-C (OCI `BM.GPU.MI300X.8`) at PHY-4 (out of this feature's PHY-0/1 gate, in scope as the
  same code path).
- The freeze-breaker rule as a spec-level fallback trigger (not silent degradation): if link 4
  does not close within the bounded spike window, ingest falls back to CPU-staged and the
  architecture continues unchanged above the `ingest_backend` boundary.
- The production `ingest_backend` API (`oi_ingest`, matching the SIM-tier af_packet
  implementation's signature shape per `p3-live-tap-ul-inject` SPEC P3-R7): VRAM RX-buffer ring
  structure, flow-steering rule configuration, stats/counters.
- The CPU-staged fallback implementation of the same API (DPDK or af_packet RX → pinned host
  buffer → GPU memcpy), selected at runtime by config, never by code branch above the API.
- The wiring-mode selector: priorities 0–4 from deep-feasibility §3, each a config selection with
  its own precondition probe (not a runtime auto-negotiation protocol).
- Per-stage latency/throughput measurement recording format (dmabuf registration time, QP setup,
  byte-landing latency, throughput at swept packet sizes) — **explicitly measurement deliverables,
  never pass/fail gates**, the one place in the whole spec set numbers are allowed to appear
  (README spec conventions, "Pins" note).

## Out of scope

- Any SIM-tier claim, implementation, or gate — this feature is PHYSICAL-only; SIM's `ingest_backend`
  (af_packet on a bridge) belongs to `p3-live-tap-ul-inject` and is referenced here only as the API
  shape p6 must match.
- Multi-vendor GPU support beyond NVIDIA (Stage A) + AMD/ROCm (Stage C) for M1 — Intel/Level Zero
  dmabuf export exists in principle (phase1 §2.4) but is not part of the M1 gate; PHY-5 (Intel
  Tiber, kernels-only) does not touch ingest at all and is out of scope here.
- Full SIM pipeline replay with real backends (PHY-2), seam/L2 hardware integration (PHY-3), AMD
  graduation run (PHY-4), Intel leg (PHY-5) — these consume this feature's `ingest_backend` but
  their own gates live in `ARCHITECTURE_v3_PHYSICAL.md` §4, not here (PHY-0/1 only, per assignment).
- SR-IOV VF creation as an M1 prerequisite — demoted by deep-feasibility §3 to a PHY-2 wiring
  option (priority 2); this feature probes VF *availability* (day-1 item 9) but M1 itself does not
  depend on it.
- GPU-autonomous NIC doorbell-ringing (DOCA GPUNetIO/GDAKI) — v3 §4 established CPU-posted
  receives are sufficient; not attempted here (see HLD rejected alternatives).
- DL-direction ingest, C-plane handling, HARQ retransmission semantics — out of scope, same as p3.

## Requirements

Every requirement is testable; test mapping in LLD §6. IDs group by sub-area: R1–R10 day-1 probes,
R11–R16 M1 spike, R17–R21 production `ingest_backend`, R22–R26 wiring modes, R27 measurement
deliverables.

### Day-1 box verification (PHYSICAL §3 items 1–6 + deep-feasibility §6 items 7–10)

| ID | Requirement | Criterion |
|---|---|---|
| **P6-R1** | `lspci \| grep -iE 'mellanox\|broadcom'` + `ibv_devinfo` SHALL run first on any new box and record NIC silicon + RDMA device(s) present. | PASS = ≥1 mlx5 RDMA device found (required for M1 wiring priorities 0–1); Broadcom-only = FAIL for M1, routes straight to CPU-staged (phase1 §2.2). |
| **P6-R2** | dmabuf MR probe SHALL: GPU-runtime-alloc a buffer, export it as a dmabuf fd (`cuMemGetHandleForAddressRange` NVIDIA / `hsa_amd_portable_export_dmabuf` AMD), then `ibv_reg_dmabuf_mr` it. | PASS/FAIL decides the box (PHYSICAL §3 item 2, verbatim). FAIL blocks all further M1 work on that box. |
| **P6-R3** | Raw-packet QP smoke test SHALL create an `IBV_QPT_RAW_PACKET` QP with a flow-steering rule (ethertype `0xAEFE`), host-RAM MR, and receive one self-loopback frame (`MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC` — **not** an external cable or VF; that dependency would contradict R22's "no cable" claim, which is gated on this requirement). | PASS = QP created, flow rule installed, self-loopback frame received into the host-RAM MR. |
| **P6-R4** | SR-IOV VF probe SHALL attempt creating 2 VFs and confirm eswitch loopback carries an eCPRI-ethertype frame VF0→VF1. | Recorded PASS/FAIL; **non-fatal** to M1 (deep-feasibility §3 demotes this to wiring priority 2 / day-1 item 9's `sriov_numvfs` write is the same probe, see R9). |
| **P6-R5** | Hugepages + DPDK bind + `dpdk-testpmd` SHALL run on a VF (or the PF in bifurcated mode). | PASS = testpmd forwards traffic at a non-zero rate (function, not a throughput gate). |
| **P6-R6** | GPU probe SHALL run `clinfo`/`rocminfo` and the LDPC kernel bit-exact suite (SIM-2 artifacts, unmodified) on the box's vendor ICD. | PASS = device enumerated + LDPC suite green, bit-exact vs the same golden vectors used in SIM. |
| **P6-R7** | (NVIDIA boxes) `nvidia-smi -q \| grep -A2 BAR1` SHALL run and record BAR1 total. | Recorded value, no fixed pass threshold (deep-feasibility §2: even 256 MiB BAR1 fits ingest rings — capacity note, not a gate); FAIL only if BAR1 reads as absent/zero. |
| **P6-R8** | (NVIDIA boxes) SHALL confirm open kernel modules (`modinfo nvidia \| grep license` → `Dual MIT/GPL`) and `cuDeviceGetAttribute(CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED)` == 1. | PASS = both true. FAIL on proprietary modules blocks R2 (dmabuf export is open-modules-only, deep-feasibility §1 link 1) — remediation is reinstalling `nvidia-open`, not a workaround. |
| **P6-R9** | SHALL read `/sys/class/net/<pf>/device/sriov_totalvfs` and attempt `sriov_numvfs=2`. | Recorded PASS/FAIL (own-VF creation availability); **non-fatal either way** (deep-feasibility §6 item 9) — feeds wiring-mode priority-2 eligibility only. |
| **P6-R10** | Raw-QP self-loopback smoke test SHALL send N eCPRI-ethertype frames from QP-A and count arrivals on QP-B (host-RAM MR) on the *same function*, using `MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC`/`_MC`. | PASS = arrivals == N, zero loss, on the same PF/function — this is wiring priority 0 and the M1 spike's step-2 mechanism; FAIL here blocks the primary M1 wiring mode (falls to priority 1). |

### M1 spike (deep-feasibility §1.1 probe order, exact)

| ID | Requirement | Criterion |
|---|---|---|
| **P6-R11** | Step 1 — dmabuf MR probe (chain links 1–3): GPU alloc → dmabuf export (`cuMemGetHandleForAddressRange` NVIDIA-open-modules / `hsa_amd_portable_export_dmabuf` ROCm) → `ibv_reg_dmabuf_mr`. | PASS/FAIL, tiny standalone C program, no NIC traffic yet (same probe as R2, run again as the formal spike entry point with recorded evidence). |
| **P6-R12** | Step 2 — raw QP + flow rule (ethertype `0xAEFE`) with a **host-RAM MR**, using `MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC`/`_MC`. Proves chain links 5–7 with zero GPU involvement. | PASS = self-sent frames arrive in the host-RAM MR via the flow rule, same function, zero loss over a counted run. |
| **P6-R13** | Step 3 — swap the host-RAM MR for the dmabuf MR from step 1, same QP/flow-rule setup otherwise unchanged. **This is the M1 moment** (chain link 4 — the one link with no public prior art). | PASS = RX WQEs complete against the dmabuf MR's lkey with no firmware/driver rejection; FAIL triggers the freeze-breaker (R16). |
| **P6-R14** | Step 4 — byte-verify landed data in VRAM: a device-side CRC kernel computes a checksum over the landed buffer; host reads back the checksum (small, not the payload) and compares against the sender's precomputed CRC of the same frames. | PASS = 0 CRC mismatches over a counted run (≥1000 frames, mirroring p3-R11's run-size convention). |
| **P6-R15** | Step 5 — NVIDIA-only cross-check: build and run upstream `l2fwd-nv` (DPDK gpudev) receiving the same frame stream into GPU memory, in parallel with (not instead of) our verbs path. | PASS = l2fwd-nv lands frames correctly, establishing the box/driver/mechanism is sound independent of our code — used to isolate our-code bugs from box bugs if R13/R14 fail. This does **not** run on AMD (no ROCm gpudev driver exists, phase1 §2.2/deep-feasibility §4) — see honesty-ledger. |
| **P6-R16** | **Freeze-breaker:** if step 3 (R13) has not passed within a bounded spike window of **≤2 weeks** of dedicated effort on Stage-A, ingest SHALL fall back to the CPU-staged path (R20) as the box's active `ingest_backend` implementation, labeled as such, with the rest of the architecture (compute_backend, handoff_backend, seam, L2) unchanged. | This is a **defined fallback path, not a failure exit** — PASS condition is "a working `ingest_backend` is selected and recorded, VRAM-ring or CPU-staged" within the window; not "VRAM-ring works." |

### Production `ingest_backend`

| ID | Requirement | Criterion |
|---|---|---|
| **P6-R17** | The VRAM-ring `ingest_backend` implementation SHALL expose a pre-registered ring of dmabuf-backed VRAM buffers (structure: LLD §3), each independently postable/reapable, no per-frame allocation after setup. | PASS = ring operates for a sustained run (≥10 min) with zero device allocations post-setup (mirrors p2 D10's "zero device allocations after setup" rule, extended to ingest). |
| **P6-R18** | The implementation SHALL expose flow-steering rule configuration (ethertype match, optionally 5-tuple for wiring modes ≥3) through the `oi_ingest` config surface, not hardcoded. | PASS = changing the configured ethertype/steering rule changes which frames are captured, verified by a negative test (wrong ethertype = zero delivered frames). |
| **P6-R19** | The implementation SHALL expose stats/counters: frames seen, ethertype-matched, delivered, dropped (ring-full or CQE-error), matching the counter categories p3-R13 already established for the SIM implementation. | PASS = counters are queryable via `oi_ingest_get_stats` and a run with induced ring-full drops shows nonzero drop count (i.e., the counter is wired to something real, not a stub). |
| **P6-R20** | The CPU-staged fallback SHALL implement the identical `oi_ingest` API (DPDK PMD or af_packet RX → pinned host buffer → explicit GPU memcpy/`hipMemcpy`/`cudaMemcpy` into the same arena ABI compute_backend expects). | PASS = `compute_backend`-facing behavior (arena contents, descriptor semantics, stats surface) is indistinguishable from the VRAM-ring implementation in a black-box test that only calls the `oi_ingest` API. |
| **P6-R21** | Selection between VRAM-ring and CPU-staged SHALL be a runtime config value read once at `oi_ingest_init`, with no code above the `oi_ingest` boundary (i.e. `compute_backend` and up) referencing which implementation is active. | PASS = a build of `compute_backend`/pipeline code with no recompilation runs unmodified against both implementations, selected only by changing the config file/env var. |

### Wiring modes (deep-feasibility §3 decision tree)

| ID | Requirement | Criterion |
|---|---|---|
| **P6-R22** | Priority 0 — same-function raw-QP self-loopback SHALL be the default/primary wiring mode, requiring no VFs, no fabric, no cable (needs only the PF). | PASS = R10/R12 pass; this is M1's wiring mode and requires no further precondition probe beyond R1/R3. |
| **P6-R23** | Priority 1 — `ru_emulator` (DPDK) and ingest (verbs) sharing one PF via mlx5 bifurcation + loopback devarg SHALL be probed as the PHY-2 wiring mode when priority 0 alone is insufficient (i.e., a real external sender is needed, not self-generated frames). | PASS = mlx5 loopback devarg honored (probe budget: ½ day, deep-feasibility §3); FAIL routes to priority 2 or 3. |
| **P6-R24** | Priority 2 — self-created SR-IOV VF pair with eswitch loopback SHALL be probed only if priority 1 is unavailable, gated on R4/R9's recorded VF-creation result. | PASS = VF0→VF1 eCPRI frame observed (probe budget: ½ day); this mode is explicitly **unverified on OCI bare metal** going in (honesty-ledger). |
| **P6-R25** | Priority 3 — two hosts/VNICs on an OCI L2 VLAN SHALL be available as a fallback wiring mode not requiring same-box loopback at all. | PASS = a non-IP ethertype (`0xAEFE`) frame observed traversing the VLAN between two VNICs; **unverified going in**, probe on first use. |
| **P6-R26** | Priority 4 — UDP-encapsulated eCPRI over L3 SHALL be available as the last-resort wiring mode with no precondition beyond ordinary IP connectivity. | PASS = frame observed at the receiver after UDP decapsulation; always expected to work (no novel mechanism). |

### Measurement deliverables (not gates)

| ID | Item | Label |
|---|---|---|
| **P6-R27** | Per-stage latency/throughput breakdown: dmabuf registration time, QP setup time, byte-landing latency (send-to-VRAM-visible), throughput at a swept set of packet sizes, recorded once each mechanism (VRAM-ring and, separately, CPU-staged if activated) is working. | **Measurement deliverable — never a PASS/FAIL gate**, anywhere in this spec set (README spec conventions: performance numbers may appear only in p6, and only as measurement deliverables). Record format: LLD §3.4. |

## Acceptance gates

Traceable to `ARCHITECTURE_v3_PHYSICAL.md` §4.

**PHY-0** ("OCI tenancy/quota filed; Stage-A box passes §3 checklist"):
- **P6-G0 (unit):** R1–R10 all recorded PASS or explicitly-non-fatal-FAIL-with-fallback-noted on
  Stage-A (`BM.GPU.A10.4`) before any spike work begins. A hard FAIL on R2 or R6 blocks PHY-0.

**PHY-1** ("M1 spike (≤2 weeks, freeze-breaker armed): eCPRI frames, same-function self-loopback →
raw QP → dmabuf → correct bytes verified in VRAM" — `ARCHITECTURE_v3_PHYSICAL.md` §4, already
corrected there per the deep-feasibility wiring finding; VF0→VF1 is demoted to a PHY-2 wiring
option, priority 2):
- **P6-G1 (unit):** R11+R12 pass independently (dmabuf MR probe; host-RAM raw-QP self-loopback) —
  each is a standalone tiny-program gate, no dependency on the other.
- **P6-G2 (integration):** R13+R14 pass together in one session (dmabuf-MR raw-QP self-loopback,
  byte-verified in VRAM) — **this is the M1 moment**, and is PHY-1's exit criterion.
- **P6-G3 (cross-check, NVIDIA-only):** R15 passes on Stage-A, run in parallel with G2, not gating
  G2 but required before treating a G2 FAIL as "our bug" vs "box/driver bug."
- **P6-G4 (fallback path, conditional):** if G2 has not passed within the R16 window, R16+R20+R21
  pass instead (CPU-staged `ingest_backend` selected, API-compatible, labeled) — **this satisfies
  PHY-1** under the freeze-breaker rule; it is a recorded degradation, not a spec failure.
- **P6-G5 (production API, unit):** R17–R21 pass for whichever implementation(s) exist after G2/G4
  — ring structure, flow-steering config, stats, fallback parity, and the "no code change above
  `oi_ingest`" swap test.
- **P6-G6 (wiring modes, unit):** R22 always passes (it *is* G2/G4's mechanism); R23–R26 recorded
  PASS/FAIL/unattempted per their own probe budgets — none block PHY-1, all feed PHY-2 planning.

**Definition of done (both PHY-0 and PHY-1):** results — including every FAIL and every fallback
trigger — logged to the honesty ledger before the milestone is marked complete, per the project's
inherited CXL-PoC convention (`ARCHITECTURE_v3_PHYSICAL.md` §0 lineage, SIM §4 DoD wording).

## Dependencies

- **Hardware ladder** (`research/phase1_feasibility_cloud_hw.md` §3): **OCI `BM.GPU.A10.4`**
  (Stage A, dev box, all of PHY-0/1 runs here) and **OCI `BM.GPU.MI300X.8`** (Stage C, graduation
  box, PHY-4 re-run of this same feature's code on AMD — file the quota request in week 1, per
  PHYSICAL §5 risk #6/phase1 §5 risk #4). **Vultr 8×MI300X** (phase1 §3.3, "C-fallback" if OCI
  MI300X quota stalls) is not a separate code path for this feature — it is the exact scenario
  `oi_ingest_cpustaged` (LLD's CPU-staged fallback implementation) exists for: Vultr ships
  Broadcom NICs (no raw-Ethernet-QP/dmabuf path), so a Stage-C run there always selects the
  CPU-staged `ingest_backend`, never the VRAM/dmabuf one, by construction.
- **`p2-phy-kernels`** — defines the I1 packet-arena/descriptor-ring ABI this feature's
  `ingest_backend` writes into; p6 supplies the arena's allocator (VRAM dmabuf region or pinned
  host buffer) and descriptor writer only, per p2 HLD D10 ("p6 swaps only the arena's allocator and
  the descriptor writer" — the ABI itself is p2's, unchanged).
- **`p3-live-tap-ul-inject`** — first names the `oi_ingest` API (P3-R7) and provides the SIM-tier
  implementation this feature's implementation must be signature-compatible with; p3's LLD.md does
  not yet exist at time of writing, so this feature's LLD §2 is the first concrete `oi_ingest`
  signature set — p3's eventual LLD should converge to it, not the reverse (flagged as a
  cross-feature note, not fixed here).
- **`p4-phy-l2-seam`** — consumes decoded TBs downstream of this feature's `compute_backend`
  hand-off; at time of writing `p4-phy-l2-seam/spec/` is empty (no handoff_backend contract to
  check against). This feature does not depend on p4's contents — the `handoff_backend` boundary
  this feature touches is v3's existing pinned-DMA design (v3 §5, SIM §3 row), not anything p4
  will add. Noted as an open dependency to re-check once p4's spec lands.
- **`p5-one-command-rig`** — bidirectional: this feature's production `ingest_backend` is what
  p5's PHYSICAL deploy unit swaps in (no p6-side action required for that direction); conversely,
  per `ARCHITECTURE_v3_PHYSICAL.md` §1 ("all rig setup = replay of SIM-5 automation + this doc's
  backend swaps"), p6's eventual integrated (non-spike) deployment is not a standalone artifact —
  it sits inside p5's compose/rig lineage, reusing p5's discovery/assertion runner with device
  mounts and this feature's backend swapped in, not a parallel deployment mechanism.

## Honesty-ledger notes (what this feature does NOT prove)

- **The freeze-breaker is a real, planned degradation path — not a blocker or a failure.** If M1
  (R13/R14) does not close in the bounded window, CPU-staged ingest is a first-class, spec'd
  outcome for PHY-1 (P6-G4), and the architecture above `ingest_backend` is provably unaffected
  (that provability — R21's swap test — is itself a requirement, not an afterthought).
- **The NVIDIA-only `l2fwd-nv` cross-check (R15/G3) does not cover the AMD leg.** No ROCm gpudev
  driver exists (DPDK gpudev is CUDA-only, deep-feasibility §4) — on Stage C (MI300X), a G2 FAIL
  has no equivalent independent mechanism validator; the verbs path is not just preferred but
  *required* there, and a failure there is harder to triage than on Stage A. This asymmetry is
  structural, not a gap to be closed by this feature.
- **Own-VF creation on OCI bare metal (R4/R9, wiring priority 2) is unverified going in.** Possible
  firmware/eswitch lockdown on Oracle-managed NICs (deep-feasibility §3 correction to PHYSICAL §2).
  Non-fatal to M1 (priority 0 doesn't need it) but PHY-2 planning should not assume it works.
- **A10 BAR1 aperture size is unverified in public sources** (R7). Even a small (256 MiB) BAR1 does
  not block the mechanism — ingest rings are tens of MB — but may cap ring capacity below what a
  larger BAR1 would allow; this is recorded as a capacity constraint, discovered day-1, not assumed.
- **This feature does not prove NIC data-plane realism at line rate, VF eswitch behavior under
  load, or flow-steering under contention** — those are PHY-2/PHY-3 concerns (SIM §5 gap #2); PHY-1
  proves the mechanism exists and lands correct bytes, once, at whatever rate the spike happens to
  run at (R27 measurement, not a gate).
- **Whether firmware permits the self-loopback flags (`MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_*`)
  on every box in the hardware ladder is unverified** — documented as an API in rdma-core, not
  confirmed present/enabled on OCI's specific ConnectX firmware revision (LLD open questions).
