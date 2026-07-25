# PHYSICAL Tier — Deep Feasibility Study (M1 chain + rig specifics)

**Date:** 2026-07-17 · **Scope:** [`ARCHITECTURE_v3_PHYSICAL.md`](../ARCHITECTURE_v3_PHYSICAL.md) (T3)
· **Builds on:** [`phase1_feasibility_cloud_hw.md`](phase1_feasibility_cloud_hw.md) (hardware ladder, §3–§5)
**Question answered:** is the PHYSICAL plan — specifically the M1 open NIC→VRAM ingest chain and its
cloud wiring — implementable as written, link by link, with public evidence for each link?

---

## 0. Verdict (TL;DR)

**Feasible. Every link of the M1 chain is individually proven in public; the assembly remains ours
(unchanged), but this pass found no blocker in any link — and it found two upgrades to the plan:**

1. **The wiring problem is smaller than planned.** mlx5 supports **same-function raw-QP self-loopback**
   (`MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/MC`, documented in `mlx5dv_create_qp(3)`), so the M1
   spike needs **no VFs, no second host, no cable**: one process sends eCPRI frames from a raw QP and a
   second raw QP on the *same function* receives them into a dmabuf MR. SR-IOV VF loopback moves from
   "M1 prerequisite" to "milestone-PHY-2 wiring option" (ru_emulator↔DU).
2. **The NVIDIA leg of M1 has an open reference implementation to diff against.**
   [NVIDIA l2fwd-nv](https://github.com/NVIDIA/l2fwd-nv) + DPDK `gpudev` receive plain Ethernet frames
   into GPU memory on ConnectX using **vanilla DPDK API** ([NVIDIA blog](https://developer.nvidia.com/blog/optimizing-inline-packet-processing-using-dpdk-and-gpudev-with-gpus/),
   [DPDK CUDA driver guide](https://doc.dpdk.org/guides/gpus/cuda.html)). Stage A can bring up
   l2fwd-nv first as a known-good, then swap in our verbs implementation — a debugging ladder the
   plan didn't have.
3. **Hardware capability is not in question — only open software assembly is.** ConnectX silicon
   demonstrably lands raw Ethernet streams in VRAM in production: NVIDIA **Rivermax** (GPUDirect over
   Ethernet on CX-5+, [FAQ](https://developer.nvidia.com/networking/rivermax/faq)) and NVIDIA's own
   **CloudRAN/Aerial fronthaul demo** (O-RAN eCPRI received into GPU memory on a CX6-DX,
   [blog](https://developer.nvidia.com/blog/building-accelerated-5g-cloudran-at-the-edge/)) — both
   closed software. The DOCA GPUNetIO **open-source cut** ([NVIDIA-DOCA/gpunetio](https://github.com/NVIDIA-DOCA/gpunetio))
   covers **RDMA RC only, not Ethernet receive** — so the "open raw-Ethernet→VRAM path" claim
   remains ours to make. This is the contribution, restated with evidence.
4. **Two genuinely open items survive, both bounded:** (a) whether OCI bare metal lets us create our
   own VFs / set mlx5 loopback knobs on an Oracle-managed NIC (day-1 probe; self-loopback makes it
   non-fatal for M1); (b) **A10 BAR1 aperture size** — unresolved in public sources; even a 256 MiB
   BAR1 fits our ingest rings (tens of MB), so it degrades capacity, not feasibility.

---

## 1. The M1 chain, link by link

Chain: `GPU alloc → dmabuf export → ibv_reg_dmabuf_mr → raw-packet QP + flow steering → RX WQEs
into VRAM → (loopback wiring) → byte-verify`.

| # | Link | Evidence | Status |
|---|---|---|---|
| 1 | GPU VRAM → dmabuf fd (NVIDIA) | `cuMemGetHandleForAddressRange` — requires **open kernel modules** (Turing+; A10 ✓), CUDA ≥11.7; open modules *export* dmabuf (import unsupported — we never import, only export) ([open-gpu-kernel-modules #243](https://github.com/NVIDIA/open-gpu-kernel-modules/discussions/243), [GPU Operator RDMA doc](https://docs.nvidia.com/datacenter/cloud-native/gpu-operator/latest/gpu-operator-rdma.html)) | ✅ proven; gate with `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED` day 1 |
| 2 | GPU VRAM → dmabuf fd (AMD) | `hsa_amd_portable_export_dmabuf` (v2 w/ flags in ROCm 7.1); AMD's ROCm XIO uses exactly export→`ibv_reg_dmabuf_mr` ([XIO memory-modes](https://rocm.docs.amd.com/projects/rocm-xio/en/beta-0.1.0/conceptual/memory-modes.html)); ROCm perftest forks ship dmabuf support ([ROCm/rdma-perftest](https://github.com/ROCm/rdma-perftest)) | ✅ mature |
| 3 | dmabuf fd → NIC MR | `ibv_reg_dmabuf_mr` (kernel ≥5.12 mlx5; GPU Operator recommends kernel ≥6.1 + rdma-core ≥44); mlx5 additionally has `mlx5dv_reg_dmabuf_mr` ([man page](https://man.archlinux.org/man/mlx5dv_reg_dmabuf_mr.3.en)); dmabuf workflow spelled out in [DatenLord writeup](https://medium.com/@datenlord/the-evolution-and-implementation-of-gpudirect-rdma-19751f7b9413) | ✅ proven (perftest `--use_cuda_dmabuf` / ROCm analogue) |
| 4 | dmabuf MR usable by **raw-packet QP** RX | An MR yields a standard lkey; RX WQEs take any registered MR — no documented mlx5 restriction excluding dmabuf MRs from `IBV_QPT_RAW_PACKET`. **No public example of the exact combo found** → this remains the M1 novelty | ⚠️ the one unproven junction — bounded by links 1–3 + 5 all proven |
| 5 | NIC hardware can steer raw Ethernet into VRAM | Rivermax (CX-5+, GPUDirect-over-Ethernet, header-split into app memory); Aerial CloudRAN eCPRI→GPU (CX6-DX); l2fwd-nv (open, DPDK gpudev) | ✅ silicon capability beyond doubt |
| 6 | Loopback wiring on one box | `MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC` — raw QP receives traffic sent by the same function ([mlx5dv_create_qp(3)](https://github.com/linux-rdma/rdma-core/blob/master/providers/mlx5/man/mlx5dv_create_qp.3.md)); DPDK mlx5 has an equivalent loopback mode ([dpdk-dev patch](https://dev.dpdk.narkive.com/WMNCrTI2/dpdk-patch-net-mlx5-enable-loopback-by-configured-mode)) | ✅ documented API — **new to the plan** |
| 7 | Flow steering (ethertype 0xAEFE → our QP) | standard `ibv_create_flow` on raw QPs; mlx5 supports on PFs and VFs (VF-scoped) | ✅ standard |

**Failure containment:** if link 4 fails (e.g. firmware rejects dmabuf-MR lkey on raw-QP RX rings),
the ladder degrades gracefully: (i) host-RAM MR raw QP (proves 5–7) + `cudaMemcpy`/`hipMemcpy` = the
CPU-staged freeze-breaker, already planned; (ii) on NVIDIA only, DPDK gpudev path (l2fwd-nv-style)
as a second mechanism proof. Neither invalidates the architecture; both are honest, labeled fallbacks.

### 1.1 M1 spike probe order (refined)

1. `ibv_devinfo` + dmabuf MR probe (links 1–3) — tiny C program, PASS/FAIL per box.
2. Raw QP + flow rule, **host-RAM MR**, self-loopback flags (links 5–7, no GPU).
3. Swap host-RAM MR → dmabuf MR (link 4 — the actual M1 moment). Byte-verify in VRAM via
   device-side CRC kernel + readback.
4. Only then: ru_emulator as the sender (real OFH traffic shape, PHY-2 wiring per §3 below).
5. NVIDIA-only cross-check: l2fwd-nv upstream build (validates box, isolates our-code bugs).

---

## 2. GPU-side specifics per stage box

### Stage A — OCI BM.GPU.A10.4 (NVIDIA)
- Shape confirmed: 4×A10, Xeon 8358 (64 OCPU), 1 TB RAM, **2×50 Gbps** frontend, 256 VNICs
  ([OCI shapes doc](https://docs.oracle.com/en-us/iaas/Content/Compute/References/computeshapes.htm)).
- **Must install open kernel modules** (`nvidia-open`), not proprietary — dmabuf export is
  open-modules-only. Ampere datacenter parts fully supported.
- **A10 BAR1 size: unverified in public sources** (workstation Ampere siblings switch 256 MiB ↔ 32 GiB
  by mode). Day-1: `nvidia-smi -q | grep -A2 BAR1`. Mitigation: ingest rings are ≪256 MiB → worst
  case costs capacity headroom, not the mechanism.
- NIC silicon on A10.4 not stated in shapes doc; OCI GPU metal has historically shipped
  Mellanox/ConnectX ([Oracle GPU page](https://www.oracle.com/cloud/compute/gpu/): CX-5 100G RoCE on
  cluster-network shapes, CX-7 on newer). High confidence, still day-1 checklist item 1.

### Stage C — OCI BM.GPU.MI300X.8 (AMD)
- Shape confirmed: 8×MI300X, 2×8480+ (112 OCPU), 2 TB RAM, **1×100 Gbps frontend + 8×400 Gbps RDMA
  backend** ([OCI shapes doc](https://docs.oracle.com/en-us/iaas/Content/Compute/References/computeshapes.htm));
  frontend is mlx5 per [oci-gpu-quickstarts MI300X README](https://github.com/oracle-quickstart/oci-gpu-quickstarts/blob/main/amd/MI300X/README-MI300X.md)
  (phase-1 finding, unchanged). eCPRI work runs on the frontend NIC; the 400G backend is a bonus
  probe target (vendor unverified — check `lspci` day 1).
- ROCm dmabuf path is the mature one (§1 link 2). Run kernels via HIP/AdaptiveCpp USM on the same
  allocation that was exported (phase-1 §2.4 strategy unchanged).

### Stage B — Hot Aisle 1×MI300X VM
- Kernels/bit-exactness only (no NIC work) — VM virtualization is irrelevant to that purpose. No change.

---

## 3. Wiring decision tree (revised — replaces "SR-IOV VFs first")

**Correction to PHYSICAL §2:** on OCI *bare metal*, secondary VNICs are delivered as **Oracle-assigned
VLAN tags on the physical NIC** — the OS configures VLAN interfaces, not customer-visible VFs
([OCI VNIC doc](https://docs.oracle.com/en-us/iaas/Content/Network/Tasks/managingVNICs.htm)); the
mlx5-VF VNIC story applies to **VM** shapes. Whether a BM tenant may create their own VFs
(`echo N > sriov_numvfs`) on the Oracle-managed NIC is **unverified** — possible firmware/eswitch
lockdown. Hence:

| Priority | Wiring | Needs | Verdict |
|---|---|---|---|
| 0 (M1) | **Same-function raw-QP self-loopback** (TIR flags, §1 link 6) | nothing but the PF | primary — no VFs, no fabric, traffic never leaves the NIC |
| 1 (PHY-2) | ru_emulator (DPDK) + ingest (verbs) **sharing one PF**: mlx5 is bifurcated, both attach to the same port; DPDK TX loopback mode + our flow rule on ethertype 0xAEFE | mlx5 loopback devarg honored | probe ½ day |
| 2 (PHY-2) | Self-created SR-IOV VF pair, eswitch loopback VF0↔VF1 | `sriov_numvfs` writable on OCI BM — **unverified** | probe ½ day; original plan demoted here |
| 3 | Two OCI VNICs on an **OCI L2 VLAN** (VCN VLAN = delivery by dst MAC, true L2 w/ broadcast/multicast — [a-team overview](https://www.ateam-oracle.com/oci-layer-2-networking-support)) — same box or two boxes | L2 VLAN carries non-IP ethertype 0xAEFE — plausible (VMware use case), **unverified** | fallback, also the 2-host option |
| 4 | UDP-encapsulated eCPRI over VCN L3 | nothing | last resort, always works |

---

## 4. Fallback legs — updated evidence

- **Broadcom (Vultr/Hot Aisle metal):** stronger than phase 1 stated. Broadcom publishes an official
  **MI300X + BCM957608 Ethernet/RoCE guide** including perftest-with-dmabuf validation
  ([docs.broadcom.com 957608-AN2XX](https://docs.broadcom.com/doc/957608-AN2XX)) — i.e. RDMA-into-VRAM
  works on Broadcom; only the **raw-Ethernet-QP receive** is missing. CPU-staged eCPRI ingest
  (DPDK `bnxt` PMD → pinned → GPU) remains the honest mode there; label unchanged.
- **DPDK gpudev:** CUDA driver only ([DPDK gpus/cuda](https://doc.dpdk.org/guides/gpus/cuda.html)) —
  no ROCm gpudev driver exists, so the DPDK route can't carry the AMD leg; the verbs path is not
  just philosophically preferred but **required** for AMD. (Reinforces building M1 on verbs.)
- **DOCA GPUNetIO open cut:** RDMA RC verbs only (IB/RoCE), GPU-side doorbells, NVIDIA GPUs; no
  Ethernet receive in the open cut ([repo](https://github.com/NVIDIA-DOCA/gpunetio)). Not a
  competitor and not a dependency — v3 §4's "we don't need GDAKI" stands, now verified.
- **Intel Tiber:** still selling Max 1100 at $0.39/hr ([Tiber AI Cloud](https://ai.cloud.intel.com/)),
  but Falcon Shores cancellation ([Fortune](https://fortune.com/2025/01/31/intels-ai-dreams-slip-further-out-of-reach-as-it-cancels-its-big-data-center-gpu-hope-falcon-shores/))
  dead-ends the Max GPU line → M5 checkbox retains its "opportunistic, may vanish" label; do it
  early if cheap, never depend on it.

---

## 5. Risk register (delta vs PHYSICAL §5 / phase-1 §5)

| # | Risk | Change this pass | Mitigation |
|---|---|---|---|
| 1 | Raw-QP + dmabuf-MR junction (link 4) unproven | **narrowed**: every neighboring link proven; silicon capability proven (Rivermax/Aerial); open NVIDIA reference exists (l2fwd-nv) | probe order §1.1; freeze-breaker unchanged |
| 2 | Cloud L2 wiring | **downgraded**: self-loopback removes wiring from M1 entirely | decision tree §3 |
| 3 | Own-VF creation on OCI BM may be locked down | **new (replaces generic SR-IOV risk)** | wiring priorities 0/1/3 don't need it |
| 4 | A10 BAR1 aperture unknown | **new, minor** | `nvidia-smi -q` day 1; rings ≪256 MiB |
| 5 | Open-kernel-modules requirement on NVIDIA boxes | **new, procedural** | install `nvidia-open` in setup automation; verify `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED` |
| 6 | OCI MI300X quota latency | unchanged | file week 1; Vultr CPU-staged fallback (now with Broadcom's own doc as setup guide) |
| 7 | Intel Max line dead-ended | **confirmed** | opportunistic only |

---

## 6. Day-1 checklist additions (append to PHYSICAL §3)

7. `nvidia-smi -q | grep -A2 BAR1` (NVIDIA boxes) — record BAR1 total.
8. Confirm open kernel modules: `modinfo nvidia | grep license` → `Dual MIT/GPL`; then
   `cuDeviceGetAttribute(..., CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, ...)` = 1.
9. `cat /sys/class/net/<pf>/device/sriov_totalvfs` and attempt `sriov_numvfs=2` — records whether
   wiring priority 2 is available (non-fatal either way).
10. Raw-QP self-loopback smoke test (wiring priority 0): send N eCPRI-ethertype frames from QP-A,
    count arrivals on QP-B (host-RAM MR) on the same function.

## 7. Sources (this pass)

mlx5dv_create_qp self-loopback flags: [rdma-core man page](https://github.com/linux-rdma/rdma-core/blob/master/providers/mlx5/man/mlx5dv_create_qp.3.md) ·
mlx5dv_reg_dmabuf_mr: [man](https://man.archlinux.org/man/mlx5dv_reg_dmabuf_mr.3.en) ·
dmabuf workflow: [DatenLord](https://medium.com/@datenlord/the-evolution-and-implementation-of-gpudirect-rdma-19751f7b9413), [GPU Operator RDMA](https://docs.nvidia.com/datacenter/cloud-native/gpu-operator/latest/gpu-operator-rdma.html) ·
NVIDIA open modules dmabuf export: [discussion #243](https://github.com/NVIDIA/open-gpu-kernel-modules/discussions/243) ·
l2fwd-nv: [repo](https://github.com/NVIDIA/l2fwd-nv), [inline packet processing blog](https://developer.nvidia.com/blog/optimizing-inline-packet-processing-using-dpdk-and-gpudev-with-gpus/) ·
DPDK gpudev CUDA-only: [guide](https://doc.dpdk.org/guides/gpus/cuda.html) ·
Rivermax GPUDirect-over-Ethernet: [FAQ](https://developer.nvidia.com/networking/rivermax/faq) ·
Aerial CloudRAN eCPRI→GPU (CX6-DX): [blog](https://developer.nvidia.com/blog/building-accelerated-5g-cloudran-at-the-edge/) ·
DOCA GPUNetIO open cut (RC-only): [repo](https://github.com/NVIDIA-DOCA/gpunetio) ·
ROCm XIO dmabuf: [memory modes](https://rocm.docs.amd.com/projects/rocm-xio/en/beta-0.1.0/conceptual/memory-modes.html) · ROCm perftest: [ROCm/rdma-perftest](https://github.com/ROCm/rdma-perftest) ·
Broadcom MI300X Ethernet guide: [957608-AN2XX](https://docs.broadcom.com/doc/957608-AN2XX) ·
OCI shapes: [computeshapes](https://docs.oracle.com/en-us/iaas/Content/Compute/References/computeshapes.htm) ·
OCI BM VNIC/VLAN model: [managingVNICs](https://docs.oracle.com/en-us/iaas/Content/Network/Tasks/managingVNICs.htm) ·
OCI L2 VLANs: [a-team](https://www.ateam-oracle.com/oci-layer-2-networking-support) ·
OCI GPU networking (CX-5/CX-7): [oracle.com/cloud/compute/gpu](https://www.oracle.com/cloud/compute/gpu/) ·
Intel Tiber pricing/status: [ai.cloud.intel.com](https://ai.cloud.intel.com/), [Fortune on Falcon Shores](https://fortune.com/2025/01/31/intels-ai-dreams-slip-further-out-of-reach-as-it-cancels-its-big-data-center-gpu-hope-falcon-shores/) ·
srsRAN ru_emulator/DPDK: [O-RAN 7.2 RU guide](https://docs.srsran.com/projects/project/en/latest/tutorials/source/oranRU/source/index.html), [gNB with DPDK](https://docs.srsran.com/projects/project/en/latest/tutorials/source/dpdk/source/index.html).
