# Open Inline v3 — Feature Breakdown (spec-first)

**Status:** scaffold + specs only (2026-07-17). **No implementation exists yet — `src/`, `docker/`,
`helpers/`, `tests/` are intentionally empty until a feature's spec set is reviewed.**

Authority chain (specs must not contradict these; on conflict, fix the spec):
1. [`../ARCHITECTURE_v3.md`](../ARCHITECTURE_v3.md) — master (thesis, data path, milestones M1–M6)
2. [`../ARCHITECTURE_v3_SIM.md`](../ARCHITECTURE_v3_SIM.md) — T1 tier; **§3 backend contract is normative**; §4 phase table defines the gates
3. [`../ARCHITECTURE_v3_PHYSICAL.md`](../ARCHITECTURE_v3_PHYSICAL.md) — T3 tier (M1 spike, hardware ladder)
4. [`../research/phase1_feasibility_cloud_hw.md`](../research/phase1_feasibility_cloud_hw.md) and
   [`../research/physical_deep_feasibility.md`](../research/physical_deep_feasibility.md) — evidence base

## Feature map

| Folder | Source phase | One-line scope |
|---|---|---|
| `p0-rig-scaffold/` | SIM P0 | Fork upstream srsRAN compose; add `gpu-phy` + `oracle` skeleton images (PoCL/AdaptiveCpp-OMP); CI builds containers + runs existing LDPC bit-exact suite in-container |
| `p1-ran-baseline/` | SIM P1 | Pure-upstream rig: ru_emulator ↔ gnb (test-mode UEs) ↔ Open5GS on compose networks; gate = real eCPRI (0xAEFE) on the fronthaul bridge, stable 10 min |
| `p2-phy-kernels/` | SIM P2 | **Spec only — implementation split into 6 sub-features below** (2026-07-22, feature too large for one implementation pass). SPEC/HLD/LLD here remain authoritative; sub-features reference it rather than duplicating it. |
| `p2a-scaffold/` | SIM P2 (impl slice) | Host API (`oi_p2_host` setup/feed/launch_slot/drain/tap/teardown), `oi_kernel_compat.h`, provenance/portability/AGPL/no-perf lint tooling. Depended on by all other p2 slices. |
| `p2b-k5-k6/` | SIM P2 (impl slice) | K5 descrambler + K6 rate-dematcher — bit-exact integer kernels, landed first (simplest, proves the port+oracle pattern). |
| `p2c-k1/` | SIM P2 (impl slice) | K1 depacketizer — fresh (T4), highest risk, own structural oracle (OCUDU CPU `uplane_message_decoder`). |
| `p2d-k2-k3/` | SIM P2 (impl slice) | K2 chan-est + K3 equalizer — chained float-tolerance kernels; first real oracle run also sets the NRMSE thresholds the parent LLD left open. |
| `p2e-k4/` | SIM P2 (impl slice) | K4 soft demapper — float input, bit-exact int8 LLR output. |
| `p2f-integration/` | SIM P2 (impl slice) | LDPC hookup, CPU CRC/TB tail, growing-pipeline integration gate (canned pcaps, class a structural + class b bit-exact). Depends on all other p2 slices. |
| `p3-live-tap-ul-inject/` | SIM P3 | ru_emulator patch to inject oracle RE grids as UL U-plane + gpu-phy live tap of the bridge; gate = live decode bit-exact while DU runs undisturbed |
| `p4-phy-l2-seam/` | SIM P4 | shm-ring IPC carrying decoded TB + CRC verdicts from gpu-phy to L2 stub/DU integration point |
| `p5-one-command-rig/` | SIM P5 | `make simtest`: compose up → P1–P4 assertions → teardown; identical on WSL2 + GCP VM; this artifact = the PHYSICAL deploy unit |
| `p6-physical-m1-ingest/` | PHYSICAL PHY-0/1 | `ingest_backend` real implementation: mlx5 raw-packet QP + `ibv_reg_dmabuf_mr` → VRAM; day-1 box probes; self-loopback wiring; CPU-staged fallback |

PHY-2..5 (vendor replays, measurements) are runs of these same artifacts on hardware — they get
run-books in `p6`/`p5` specs, not separate feature folders.

## Per-feature layout

```
pX-name/
  spec/      SPEC.md  (WHAT: requirements, scope, acceptance gates)
             HLD.md   (design: components, interfaces, data flow, deployment)
             LLD.md   (module-level: APIs, data structures, config schemas, test plan)
  src/       source code            (empty until spec approved)
  docker/    Dockerfiles/compose    (empty until spec approved)
  helpers/   scripts                (empty until spec approved)
  tests/     unit + integration     (empty until spec approved)
```

### Implementation-slice sub-features (e.g. `p2a`–`p2f`)

When one feature's spec is too large to implement in one pass, split the *implementation* (not the
spec) into sub-features named `pXa-name`, `pXb-name`, etc. These carry **`README.md` instead of
`spec/`** — a short pointer into the parent's already-approved SPEC/HLD/LLD naming exactly which
requirement IDs and gates that slice owns, plus its dependency on sibling slices. The parent
feature's `spec/` stays the sole source of truth; sub-feature READMEs must never restate or
re-derive requirements, only reference them. See `p2a-scaffold/README.md` for the pattern.

## Spec conventions (normative for all features)

- **SPEC.md sections:** Purpose · In scope / Out of scope · Requirements (numbered `PX-R1…`,
  each testable) · Acceptance gates (unit + integration, copied/refined from SIM §4 or PHYSICAL §4
  tables) · Dependencies on other features · Honesty-ledger notes (what this feature does NOT prove).
- **HLD.md sections:** Context diagram · Components · Interfaces (name every boundary) · Data flow ·
  Deployment view (which container/host/tier) · Design decisions with rationale · Rejected alternatives.
- **LLD.md sections:** Module breakdown · Public APIs (signatures) · Data structures & formats
  (wire/shm layouts byte-precise where crossing a boundary) · Configuration (YAML/env schema) ·
  Error handling · Test plan (per requirement ID) · Open questions.
- **Backend contract vocabulary** (SIM §3) is mandatory: `ingest_backend`, `compute_backend`,
  `handoff_backend`. Every feature states which backends it touches and which implementation
  (SIM or PHYSICAL) it provides.
- **Kernel rules** (SIM §3) bind p2/p3/p6: OpenCL C source-JIT and/or AdaptiveCpp generic SSCP; no
  warp-width assumptions, no inline asm, no vendor intrinsics outside one abstraction header.
  Integer stages bit-exact; float stages tolerance-gated (tolerance recorded in the spec).
- **Pins:** **OCUDU `release_26_04`** (`gitlab.com/ocudu/ocudu` — successor of srsRAN Project,
  BSD-3; re-pin rationale + path/namespace deltas: [`../research/ocudu_repin.md`](../research/ocudu_repin.md)).
  Key deltas vs old srsRAN pin: `ru_emulator` now at `apps/examples/ofh/`; namespace
  `srsran`→`ocudu`; golden conformance vectors NOT in OCUDU — dual-oracle rule applies (srsRAN
  AGPL vectors CI-only/never redistributed; Sionna-generated vectors are the shippable oracle).
  Ubuntu 24.04 images; versions recorded per feature in
  LLD Configuration. SIM proves function only — no performance numbers may appear as requirements
  anywhere except `p6` (and there only as *measurement deliverables*, not pass/fail thresholds).
- Specs reference each other by folder name (e.g. "consumes `p2-phy-kernels` pipeline API").
