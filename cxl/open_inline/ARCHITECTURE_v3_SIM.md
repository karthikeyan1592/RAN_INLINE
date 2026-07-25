# Open Inline v3 — SIMULATION Architecture (T1: containerized functional proof)

**Status:** working plan (2026-07-17, container revision — supersedes the same-day VM-centric
draft). Specializes [`ARCHITECTURE_v3.md`](ARCHITECTURE_v3.md) (master, unchanged) into the $0-GPU
functional tier. Companion: [`ARCHITECTURE_v3_PHYSICAL.md`](ARCHITECTURE_v3_PHYSICAL.md) (T3).
Feasibility basis: [`research/phase1_feasibility_cloud_hw.md`](research/phase1_feasibility_cloud_hw.md).
Tool-selection deep dive (srsRAN/OCUDU vs OAI vs Aerial vs ruled-out candidates, re-pin action items):
[`research/simulator_tool_selection.md`](research/simulator_tool_selection.md) (2026-07-19).

**Rule inherited from the CXL PoC:** SIM proves *function and integration only*. No latency,
throughput, or mechanism claims from this tier — those belong exclusively to PHYSICAL.

---

## 1. Deployment model: containers, not VMs (kills the TCG problem)

Containers share the host kernel → **native execution speed, no CPU emulation anywhere**. The
QEMU/TCG timing problem does not exist in this model. Docker bridge networks are veth pairs on a
Linux bridge — i.e. the "virtual fronthaul wire" is built into the deployment tool. srsRAN
upstream already ships the base we need: official `docker/docker-compose.yml` with a gNB image
(Ubuntu 24.04) and dockerized Open5GS.

**Hosts (same compose file everywhere):**
- **WSL2 directly** — primary dev loop. Day-1 check: `modprobe sctp` (NGAP needs SCTP; stock WSL2
  kernels often lack it — rebuild WSL2 kernel with `CONFIG_IP_SCTP=m` if missing; routine for us).
- **GCP `n2-standard-16`** (`sim/gcp/*.sh`, ≈$0.8/hr) — clean timed runs, CI-style full-rig tests,
  stock cloud kernel (has SCTP). Same containers, zero changes.
- **PHYSICAL boxes later** — same compose plus device mounts (`/dev/infiniband`, `/dev/kfd`,
  `/dev/dri`, hugepages) and the real backends. The compose file is the deploy artifact that
  migrates across all three.

## 2. Container topology

```
docker network "fronthaul"  (bridge, mtu 9000 — the eCPRI wire)
docker network "backhaul"   (gNB ↔ core)

┌──────────┐  fronthaul   ┌─────────────────┐  backhaul  ┌───────────────┐
│  ru-emu  │◄════eCPRI═══►│      gnb        │◄══NGAP/GTP═►│  5gc (core)   │
│ srsRAN   │              │ srsRAN CU+DU,   │   (SCTP)    │ Open5GS+mongo │
│ ru_emulator│            │ test-mode UEs   │             │ (upstream     │
│ (patched:│              │                 │             │  compose svc) │
│ oracle UL│              └───────┬─────────┘             └───────────────┘
│ IQ inject)│                     │ tap: UL RE grids
└──────────┘                      ▼
                          ┌───────────────┐      ┌─────────────────────┐
                          │   gpu-phy     │─────►│ oracle/CI harness    │
                          │ our kernels on│ shm  │ bit-exact vs srsRAN  │
                          │ PoCL (OpenCL) │ seam │ golden vectors       │
                          │ ipc:shareable │      │ (runs in CI too)     │
                          └───────────────┘      └─────────────────────┘
```

- `gpu-phy` ↔ `gnb` seam IPC: shared-memory ring via shared `/dev/shm` volume or
  `ipc: container:gnb` (v3 §7 custom IPC).
- `ru-emu` and `gpu-phy` join `fronthaul` with `cap_add: NET_RAW` (af_packet RX); no privileged
  mode needed except where upstream images already use it.
- Everything version-pinned in images (`pins/` recorded at build) — SIM↔PHYSICAL kernel/userland
  parity comes from the images, not the host.

## 3. Backend contract (normative for both SIM and PHYSICAL)

| Interface | SIM implementation | PHYSICAL implementation |
|---|---|---|
| `ingest_backend` | af_packet/socket RX on bridge + memcpy | mlx5 raw-packet QP + `ibv_reg_dmabuf_mr` → VRAM |
| `compute_backend` | PoCL (CPU OpenCL) / AdaptiveCpp-OMP | ROCm / CUDA / Level Zero — same kernel source |
| `handoff_backend` | plain memcpy | pinned-buffer async DMA + completion event |

Kernel rules: OpenCL C source-JIT (± SPIR-V) and/or AdaptiveCpp generic single binary; no
warp-width assumptions, no inline asm, no vendor intrinsics outside one abstraction header.
Integer stages (LDPC/descrambler/CRC) bit-exact everywhere; float stages (chan-est/eq) validated
to a recorded tolerance.

## 3.1 The "GPU" inside the SIM container (how L1-GPU code runs with no GPU)

No GPU simulator — a **device swap**. The `gpu-phy` image ships PoCL (OpenCL-on-CPU ICD; work-
groups→threads, spec-correct barriers/local-memory, bit-identical integer math) and AdaptiveCpp's
OMP backend for the SYCL side. No `/dev` mounts, no privileges. The identical image runs on real
GPUs by adding the vendor ICD + device mounts and flipping `OI_CL_PLATFORM`:
SIM=`pocl`(none) · AMD=ROCm ICD(`/dev/kfd`,`/dev/dri`) · NVIDIA=container-toolkit · Intel=NEO(`/dev/dri`).
Preserved on PoCL: kernel semantics, execution model, host API, integer bit-exactness (P2 gates
valid). Not preserved: warp width (good — enforces our no-warp-assumption rule), memory-hierarchy
effects, performance, vendor float rounding (tolerance gates). **Per-kernel verification ladder:**
(1) PoCL every commit — functional/bit-exact; (2) **Oclgrind** nightly CI — OOB access, barrier
divergence, work-group races (interpreter, slow); (2.5) **laptop GPU** (verified: RTX 2050 4 GB,
CUDA via WSL2 `/dev/dxg`) — the SYCL/HIP kernel variants run on real silicon *inside the SIM rig*:
AdaptiveCpp CUDA backend or HIP-on-NVIDIA in the `gpu-phy` container via nvidia-container-toolkit
(`gpus: all` in compose). Buys real warp-32 semantics, local-memory limits, device JIT, async-copy/
event behavior — before renting any cloud hour. Limits: NVIDIA ships **no OpenCL in WSL2** (the
OpenCL-C variant stays on PoCL/Oclgrind until a cloud box), no dmabuf/RDMA, and perf numbers are
indicative-only (mobile GPU + dxg layer — never quoted); (3) cloud GPUs — PHYSICAL tier (AMD/Intel
vendor JIT + all quoted numbers).

## 4. Phased feature plan — each phase: **code → deploy (compose) → test → integration-test**

| Phase | Feature (deliverable) | Test gate (unit) | Integration gate (full rig) |
|---|---|---|---|
| **P0** rig scaffold | Fork upstream compose; add `gpu-phy` + `oracle` skeleton images; CI runs container build + PoCL `clinfo` + existing LDPC kernel suite | LDPC bit-exact suite green **in container** | `docker compose up` brings up 5gc+gnb (upstream defaults) |
| **P1** RAN baseline | gnb test-mode UEs + ru-emu on `fronthaul` net — pure upstream, zero our code | gnb/ru-emu KPIs clean | **tcpdump on the bridge asserts real eCPRI (0xAEFE) C/U-plane flow**; attach/traffic counters stable over 10 min |
| **P2** PHY kernels (one sub-phase per kernel) | depacketizer → chan-est → equalizer → demapper → descrambler → rate-dematch, each landed separately in `gpu-phy` | each kernel bit-exact/tolerance vs srsRAN golden vectors | growing `gpu-phy` pipeline decodes canned eCPRI pcaps (from P1 capture) end-to-end |
| **P3** live tap + UL injection | ru_emulator patch: inject oracle RE grids as UL U-plane; `gpu-phy` af_packet-taps the live bridge | injected frames byte-identical to oracle grids | `gpu-phy` decodes the **live** stream bit-exact while DU runs undisturbed |
| **P4** seam | shm-ring IPC: `gpu-phy` → TB+CRC → L2 stub (then DU integration point) | IPC unit tests (ordering, wrap, restart) | end-to-end slot processing: injected UL → GPU pipeline → TB delivered with correct CRC verdicts |
| **P5** one-command rig | `make simtest`: compose up, run P1–P4 assertions, teardown; runs identically on WSL2 + GCP | CI green | **this artifact = PHYSICAL deploy unit** (backend swap only) |

Definition of done per phase: gates green on WSL2 **and** on the GCP VM (proves no
host-kernel accident), results logged to the honesty ledger.

## 5. Gap ledger: what SIM (+laptop GPU) proves vs what stays PHYSICAL-only

✅ Protocol flow (real eCPRI frames on a real bridge), kernel-source correctness (bit-exact),
real-GPU kernel execution on NVIDIA (rung 2.5: warp-32, JIT, local memory, events), partial
`handoff_backend` realism (pinned memory + async DMA works over WSL2 CUDA), seam design,
srsRAN/Open5GS integration, container deploy artifact, pinned userspace stack.

❌ **PHYSICAL-only, the complete list:**
1. **M1 NIC→VRAM ingest** (raw QP + dmabuf — no RDMA NIC; dxg GPUs can't export dmabuf) — the
   research claim itself;
2. NIC data-plane realism (DPDK mlx5, VF eswitch loopback, flow steering, line-rate/backpressure);
3. AMD/Intel vendors (ROCm JIT, **wavefront-64** correctness class, NEO) — the thesis needs them;
4. OpenCL-C variant on a real GPU driver (no OpenCL in WSL2);
5. every quoted number (paravirt mobile GPU + Microsoft host kernel — ledger rule: quote nothing
   from SIM). (+minor: host kernel not pinned on WSL2 — re-verify SCTP/hugepages on arrival.)

**Consequence:** PHYSICAL = validation bursts, not development — arrive correct, run day-1
checklist + M1 spike + vendor re-runs + measurements. Gaps 1–2 hold all the risk (fallback:
CPU-staged ingest demotes it to measured overhead); 3–5 are work, not risk.

## 6. Honest challenge list (unchanged in substance)

1. **P2 is the bulk of the build** (~2–3 months MVP; feasibility §6).
2. ru_emulator UL-injection patch — small but upstream-code archaeology (P3).
3. DU tap / seam surface — second-largest item (P4; v3 §7 warning stands).
4. WSL2 kernel SCTP module — check day 1, rebuild if absent (or develop on GCP VM only).
