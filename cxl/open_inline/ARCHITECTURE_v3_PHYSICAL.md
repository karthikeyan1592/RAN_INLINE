# Open Inline v3 — PHYSICAL Architecture (T3: mechanism + performance, cloud bare metal)

**Status:** working plan (2026-07-17). Specializes [`ARCHITECTURE_v3.md`](ARCHITECTURE_v3.md) (master)
into the real-hardware tier. Companion: [`ARCHITECTURE_v3_SIM.md`](ARCHITECTURE_v3_SIM.md) (T1 — runs first;
its §3 backend contract is normative here too). Hardware basis:
[`research/phase1_feasibility_cloud_hw.md`](research/phase1_feasibility_cloud_hw.md) §3–§5.
Deep feasibility of this tier (M1 chain link-by-link, wiring decision tree, revised risks):
[`research/physical_deep_feasibility.md`](research/physical_deep_feasibility.md) (2026-07-17).

**PHYSICAL exists to prove exactly what SIM cannot:** (1) the open NIC→GPU-VRAM ingest mechanism
(M1), (2) all latency/throughput numbers, (3) real vendor-driver behavior (ROCm/NEO/NVIDIA JIT,
wavefront 64, local-memory limits).

---

## 1. Hardware ladder (from feasibility study §3–§4)

| Stage | Box | Rate | Purpose |
|---|---|---|---|
| A dev grind | **OCI `BM.GPU.A10.4`** — bare metal, 4×A10, mlx5 expected (verify day 1) | $8/hr | M1 spike + full pipeline bring-up on NVIDIA |
| B AMD kernels | **Hot Aisle 1×MI300X VM** | $2.99/hr | ROCm port/bit-exactness (no NIC work) |
| C graduation | **OCI `BM.GPU.MI300X.8`** — bare metal, 8×MI300X, mlx5 confirmed | $48/hr bursts | headline: full open path on AMD (**file quota request week 1**) |
| C-fallback | Vultr 8×MI300X on-demand (Broadcom NIC) | $14.80/hr | e2e with CPU-staged ingest if OCI quota stalls |
| D optional | Intel Tiber Max 1100 | $0.39/hr | third-vendor PHY checkbox (M5) |

Same pinned Ubuntu 24.04 + kernel as SIM. All rig setup = replay of SIM-5 automation + this doc's
backend swaps.

## 2. Data path deltas vs SIM (everything else identical)

```
ru_emulator (DPDK mlx5 PMD) ══ same-function raw-QP self-loopback (M1) ══ DU / ingest
                                                     │ raw-packet QP RX
                                                     ▼ dmabuf MR (GPU VRAM)
                     GPU: same kernels, vendor runtime (ROCm/CUDA/L0)
                                                     │ pinned-buffer DMA + completion event
                                                     ▼
                     CPU: CRC → seam IPC (unchanged from SIM) → L2/core (unchanged)
```

- **Wiring without cables (corrected 2026-07-17, see deep-feasibility §1.1/§3):** M1's primary
  mode needs **no VFs at all** — `MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC` lets a raw QP
  receive traffic sent by the same PF. SR-IOV VF-pair loopback (VF0→VF1) is demoted to a
  **PHY-2 wiring option** (priority 2 in the decision tree — used only if the shared-PF mode
  can't carry the real ru_emulator↔DU traffic shape). Fallbacks below that: OCI L2 VLAN between
  two hosts; UDP-encapsulated eCPRI.
- **Backend swaps (the only deltas):** `ingest_backend` → raw-QP+dmabuf (**new code = M1**);
  `compute_backend` → vendor ICD (env only); `handoff_backend` → pinned DMA (small glue);
  DPDK PMD virtio→mlx5 (config only).

## 3. Day-1 verification checklist (run on every new box before any real work)

1. `lspci | grep -iE 'mellanox|broadcom'` ; `ibv_devinfo` — confirm NIC silicon + RDMA device.
2. dmabuf MR probe: GPU-runtime alloc → export dmabuf → `ibv_reg_dmabuf_mr` (tiny C program;
   perftest `--use_dmabuf` analogue). PASS/FAIL decides the box.
3. Raw-packet QP smoke test: `IBV_QPT_RAW_PACKET` + flow steering rule, host-RAM MR first;
   confirm same-function self-loopback (`MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC`) — this
   is M1's primary wiring mode (deep-feasibility §1.1 item 2), no VFs required.
4. SR-IOV (PHY-2 wiring option, not required for M1): probe whether `sriov_numvfs` is writable on
   this box; if so, create 2 VFs and confirm eswitch loopback carries an eCPRI-ethertype frame
   VF0→VF1 (own-VF creation on OCI bare metal is unverified — non-fatal if this probe fails).
5. Hugepages + DPDK bind + `dpdk-testpmd` on the PF (or a VF, if #4 succeeded).
6. GPU: `clinfo`/`rocminfo`, run the LDPC kernel bit-exact suite (SIM-2 artifacts).

## 4. Milestones

| PHY | Deliverable | Maps to v3 |
|---|---|---|
| 0 | OCI tenancy/quota (A10 + MI300X requests filed week 1); Stage-A box passes §3 checklist | §6.2 |
| 1 | **M1 spike (≤2 weeks, freeze-breaker armed):** eCPRI frames, same-function self-loopback → raw QP → dmabuf → correct bytes verified in VRAM | M1 |
| 2 | SIM pipeline replayed with real backends: ru_emulator→NIC→GPU→decoded TB, bit-exact | M3 |
| 3 | Seam + L2 end-to-end on hardware; first honest latency numbers (per-stage breakdown) | M4 |
| 4 | AMD graduation on `BM.GPU.MI300X.8` (or Vultr CPU-staged fallback — label it as such) | M5 |
| 5 | Optional Intel leg (Tiber) — kernels only | M5 |

**Exit criterion = v3 §10:** PHY-1..3 bit-exact on ≥1 vendor gates Phase 2 (CXL).

## 5. Risks specific to this tier

Top: M1 combination unproven publicly (each piece open+documented; assembly is ours — that's the
contribution). Then: OCI MI300X quota latency (fallback armed); Broadcom boxes = CPU-staged only;
IB-fabric boxes (Voltage Park) need Ethernet-mode port or link-down eswitch loopback — unverified,
max 1 probe day. Full register: feasibility study §5; **updated register + M1 probe order:
deep-feasibility study §1.1/§5** (headline changes: M1 needs no VFs — mlx5 same-function raw-QP
self-loopback flags; NVIDIA boxes must run open kernel modules; own-VF creation on OCI BM
unverified but off the critical path).
