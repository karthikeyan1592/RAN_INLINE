# p0-rig-scaffold — SPEC

> **PIN UPDATE (2026-07-19):** upstream re-pinned srsRAN Project `release_24_10_1` → **OCUDU `release_26_04`** (`gitlab.com/ocudu/ocudu`, BSD-3). Read `../../../research/ocudu_repin.md` before implementing. Deltas: `ru_emulator` moved to `apps/examples/ofh/`; namespace/headers `srsran`→`ocudu`; golden conformance vectors absent in OCUDU (dual-oracle rule: srsRAN AGPL vectors CI-only, Sionna vectors shippable). Mentions of "srsRAN Project" below refer to OCUDU unless explicitly about the legacy AGPL checkout.

**Feature:** SIM phase P0 — rig scaffold.
**Authority:** [`ARCHITECTURE_v3_SIM.md`](../../../ARCHITECTURE_v3_SIM.md) §1–§4 (P0 row of §4 is the
gate source); backend contract = SIM §3. Master: [`ARCHITECTURE_v3.md`](../../../ARCHITECTURE_v3.md).

## Purpose

Stand up the containerized SIM rig skeleton: a fork of the upstream srsRAN Project
`release_26_04` docker compose (gNB image + dockerized Open5GS), extended with two project
images — `gpu-phy` (PoCL CPU-OpenCL + AdaptiveCpp OMP, the SIM `compute_backend`) and `oracle`
(golden-vector store + verdict harness) — plus a CI job proving the images build, PoCL is the
visible OpenCL platform, and the pre-existing LDPC bit-exact kernel suite passes **inside the
container**. Everything later (P1–P5) deploys onto this scaffold.

**Backend statement (SIM §3):** this feature provides the SIM `compute_backend`
(PoCL CPU-OpenCL ICD; AdaptiveCpp OMP backend for the SYCL side). It does **not** implement
`ingest_backend` or `handoff_backend` — the `gpu-phy` container is a skeleton whose only workload
in P0 is the LDPC suite. The image is built so the PHYSICAL `compute_backend` swap is
env + ICD + device mounts only (SIM §3.1), with no image rebuild of project code.

## In scope

- Vendored fork of upstream srsRAN Project `release_26_04` `docker/docker-compose.yml`
  (gNB image, Ubuntu 24.04; dockerized Open5GS 5GC), provenance recorded.
- Compose *extension* (override file) adding `gpu-phy` and `oracle` services; upstream services
  untouched.
- `gpu-phy` image: Ubuntu 24.04 + PoCL ICD + AdaptiveCpp (OMP backend) + `clinfo` + the
  pre-existing LDPC bit-exact kernel suite. No `/dev` mounts, unprivileged.
- `oracle` image: golden-vector store + verdict CLI skeleton (consumed fully from P2 on).
- `pins` manifest generated at image build (all version pins).
- CI job: build all images → `clinfo` platform assertion → LDPC suite in-container.
- Compose bring-up smoke: `docker compose up` starts 5gc + gnb with upstream defaults.

## Out of scope

- Fronthaul networks, ru_emulator, eCPRI, test-mode UEs (→ `p1-ran-baseline`).
- Any PHY kernel beyond the already-existing LDPC decoder suite (→ `p2-phy-kernels`).
- Live tap / UL injection (→ `p3-live-tap-ul-inject`); seam IPC (→ `p4-phy-l2-seam`).
- `make simtest` orchestration and ledger (→ `p5-one-command-rig`).
- Real GPU / vendor ICDs, device mounts, hugepages (→ PHYSICAL tier, `p6-physical-m1-ingest`).
- Any latency/throughput requirement — SIM proves function and integration only (SIM header rule).

## Requirements

| ID | Requirement (each testable) |
|---|---|
| **P0-R1** | The repo vendors the upstream srsRAN Project compose assets at tag `release_26_04` under this feature's `docker/upstream/`, unmodified, with provenance recorded (upstream URL, tag, commit SHA) in the pins manifest. |
| **P0-R2** | Project services (`gpu-phy`, `oracle`) are added via a compose override file (`compose.sim.yml`); `docker compose config` over upstream+override resolves without altering any upstream service definition (diff of rendered upstream services = empty). |
| **P0-R3** | The `gpu-phy` image is Ubuntu 24.04 and contains: PoCL OpenCL ICD, AdaptiveCpp with OMP backend, `clinfo`, and the LDPC suite binaries. The service declares no `devices:`, no `privileged:`, no added capabilities. |
| **P0-R4** | Platform selection is env-driven: `OI_CL_PLATFORM=pocl` (default in SIM). Inside the running `gpu-phy` container, `clinfo` lists exactly one platform whose name matches `Portable Computing Language`, and the suite's platform-selection helper resolves `pocl` to that platform. |
| **P0-R5** | The `oracle` image contains the golden-vector store (LDPC vectors in P0; layout extensible per-kernel for P2) and a verdict CLI that, given a result blob and a vector ID, emits `PASS`/`FAIL` + a machine-readable verdict JSON. |
| **P0-R6** | The pre-existing LDPC bit-exact kernel suite runs **unmodified** (same sources as prior work, vendoring recorded in pins) inside the `gpu-phy` container on PoCL and reports 0 bit mismatches over its full vector set. |
| **P0-R7** | Every image build emits a pins manifest (`pins.json`) recording: srsRAN tag+SHA, base image digest, PoCL version, AdaptiveCpp version, LDPC-suite source SHA, Open5GS image tags. The manifest is stored as an image label and as a build artifact. |
| **P0-R8** | A CI job builds all images from scratch, then runs the P0-R4 `clinfo` assertion and the P0-R6 suite in the built `gpu-phy` container; the job is required-green. The CI definition contains no timing/perf threshold of any kind. |
| **P0-R9** | `docker compose up` (upstream defaults + override) brings `5gc` and `gnb` services to running state, stable (0 restarts) for ≥ 60 s, with gnb log evidence that the gNB process started and attempted NG setup toward the core. Precondition: host SCTP check (see p1-ran-baseline P1-R6; the same check script is reused here) — on stock WSL2 kernels without `CONFIG_IP_SCTP`, this gate is expected-blocked and must fail with the actionable SCTP message, not a raw crash. |

## Acceptance gates

Traceability: SIM §4, row **P0**.

| Gate | Type | Statement | SIM §4 source |
|---|---|---|---|
| **P0-G1** | unit | LDPC bit-exact suite green **in container** (`gpu-phy`, PoCL): 0/N bit mismatches, exit 0. Covers P0-R3/R4/R6. | P0 "Test gate (unit)" |
| **P0-G2** | integration | `docker compose up` brings up 5gc + gnb with upstream defaults per P0-R9. | P0 "Integration gate" |
| **P0-G3** | CI | The P0-R8 CI job (build + clinfo + suite) is green on the main branch. | P0 feature column ("CI runs container build + PoCL clinfo + existing LDPC kernel suite") |

Definition of done (SIM §4 footer): P0-G1/G2 pass on **both** WSL2 and the GCP `n2-standard-16`
VM; results logged to the honesty ledger (manually until p5 automates it).

## Dependencies on other features

- None upstream of it (P0 is the root). Consumers: `p1-ran-baseline` (compose base),
  `p2-phy-kernels` (gpu-phy/oracle images, vector-store layout), `p5-one-command-rig`
  (compose files + pins as the deploy artifact).
- External: pre-existing LDPC bit-exact kernel suite (prior project work); upstream srsRAN
  Project `release_26_04`; upstream Open5GS images referenced by the srsRAN compose.

## Honesty-ledger notes (what P0 does NOT prove)

- No RAN function is proven: gnb/5gc merely start; no UE, no fronthaul, no eCPRI (P1).
- PoCL green ≠ GPU green: warp width, memory hierarchy, vendor JIT, float rounding are all
  unexercised (SIM §3.1 "not preserved" list). Integer bit-exactness on PoCL is the only
  kernel claim made.
- No performance evidence of any kind is produced or recorded as a claim (SIM header rule).
- The `handoff_backend` and `ingest_backend` are absent, not stubbed — nothing about them is proven.
