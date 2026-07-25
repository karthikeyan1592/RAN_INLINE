# End-to-End RAN LDPC-Offload Pipeline — Specification & Architecture

**Type:** Spec + Architecture document (design only; not an implementation guide)
**Scope:** Uplink 5G-NR receive chain — eCPRI ingress → CPU PHY → LDPC offload → CXL shared-memory hand-off → verified decode.
**Reader:** Assume cold start. This document defines *what we build, the expected latency, the hardware architecture, and the hardware configuration required to bring the workload up.*

---

## 1. What we are going to do

Build a functional end-to-end uplink pipeline that mirrors an O-RAN split-7.2x deployment:

- A **traffic generator** (plays the O-RU / UE side) encodes *known* transport blocks, modulates
  them, maps them to frequency-domain resource elements, and ships them as **eCPRI packets over a
  NIC** into a QEMU guest.
- Inside the guest, the **CPU runs the real srsRAN uplink PHY** (channel estimation → equalization
  → soft-demapping) to produce LLRs from the received resource grid.
- The **expensive LDPC decode is offloaded** to a separate "accelerator" context. LLRs are handed
  to it, and decoded transport blocks are handed back, **through a CXL-emulated shared-memory
  region** (the CPU↔accelerator fabric, the emucxl/NVLink-analogue).
- A **consumer** reads the decoded TB from CXL shm, runs the real CRC, and **bit-compares against
  the known transmitted bits** — a real end-to-end correctness oracle.

The purpose is to demonstrate the **architecture** (transparent/plumbed LDPC offload over a
coherent shared-memory fabric) and to **quantify the latency motivation** for offload — not to
achieve real-time compliance (impossible under emulation; see §4).

### Resolved design decisions (previously open)

| Decision | Choice | Rationale |
|---|---|---|
| **IQ domain / fronthaul split** | **Frequency-domain, O-RAN split 7.2x** (as Nokia deploys) | RU owns the FFT; only resource elements cross the fronthaul. Guest DU starts at channel estimation — **no FFT in the guest** — which is exactly `srsran::pusch_processor`'s input. |
| **LDPC offload trigger** | **Explicit plugin / IPC by default.** Use **bpftime uprobe only when intercepting a binary we do not control** ("if there's nothing to sniff, call directly; if we must sniff, use bpftime"). | We own the PHY, so there is nothing to sniff → direct call is deterministic and testable. bpftime becomes an overlay (§9 M4b) for the "transparent, no source change" story. |
| **eCPRI framing** | **Simplified, documented** eCPRI-style header + IQ payload for the core build; strict O-RAN 7.2x U-plane framing is an overlay (§9 M4a). | Fastest path to a real packet boundary without O-RAN section/compression complexity. |

---

## 2. Software architecture (summary)

Traffic generator runs on the **host**; RX, PHY, accelerator, and consumer run **inside the guest**
as **separate processes** sharing the CXL region (so the hand-off is a real inter-context boundary).

```
 HOST (outside QEMU)                         QEMU GUEST (the O-DU)
 ┌──────────────────────┐   eCPRI/UDP    ┌───────────────────────────────────────┐
 │ Traffic Generator     │ ────────────► │ eCPRI RX (virtio-net) → reassemble RE  │
 │  known TB → srsRAN     │  virtio-net   │            │ freq-domain grid          │
 │  encode → mod → RE map │               │            ▼                           │
 │  → eCPRI; writes       │               │ srsRAN UL PHY (pusch_processor):       │
 │  ground-truth TB bits  │               │  chan-est → equalize → soft-demap→LLR  │
 └──────────────────────┘               │            │ write LLR (every CB)      │
                                          │            ▼                           │
                                          │     ╔══════════════════════╗           │
                                          │     ║  CXL shm (NUMA node1) ║◄────┐     │
                                          │     ╚══════════════════════╝     │     │
                                          │            │ signal              │     │
                                          │            ▼                     │     │
                                          │ LDPC Accelerator (separate proc):│     │
                                          │  read LLR ← CXL, OpenCL decode,  │     │
                                          │  write TB → CXL (every CB) ──────┘     │
                                          │            │ signal                    │
                                          │            ▼                            │
                                          │ Consumer: read TB ← CXL, CRC24 +        │
                                          │  bit-compare vs ground truth → CSV      │
                                          └───────────────────────────────────────┘
```

Per-slot contract: generator emits known bits → RX reassembles grid → PHY writes LLRs to CXL →
accelerator decodes and writes TB to CXL → consumer verifies. **LLRs and TBs transit CXL shm for
every codeblock** (not once). Reuse the existing bit-exact `ldpc_decode.cl` and BG1/BG2
auto-detection. See companion notes for component-level detail; this document focuses on latency
and hardware.

---

## 3. Expected latency

### 3.1 The budget and the motivation

5G NR numerology µ=1 (30 kHz SCS) → **slot = 500 µs**. In a real-time DU the entire UL L1 RX chain
must complete within one slot. The reference workload (PRIMARY_CONFIG, unchanged anchor): 100 MHz,
273 PRB, MCS28, BG1, Z=384 → **24 codeblocks per slot**.

**The motivation for offload, in one line:** software LDPC on CPU misses the slot deadline; a real
accelerator brings it back inside. The numbers below are the quantitative case.

### 3.2 Per-stage latency (per slot, 24 CB)

Two columns: **real-hardware target** (what a production O-DU would see) and **this PoC (emulated)**
(what we actually measure — QEMU + PoCL). Emulated numbers are *not* real-time; they are per-stage
processing latency, reported honestly (§4).

| Stage | Real-HW target / slot | This PoC (emulated) / slot | Source |
|---|---|---|---|
| Fronthaul eCPRI ingress (one-way) | ~100 µs (O-RAN FH budget) | variable, non-RT (10s–100s µs, virtio-net) | O-RAN.WG4 FH budget |
| RE reassembly | few µs | tens of µs | — |
| Channel est + equalization (pusch_processor) | ~50–150 µs | higher (emulated CPU) | srsRAN pusch_processor_benchmark |
| Soft-demapping → LLR | ~10–50 µs | higher | srsRAN |
| **CXL hand-off** (LLR write + TB read, all CB) | ~150–250 ns/access; a few µs/slot buffer | DRAM-speed (**system-ram NUMA**, few µs) | Pond/CXL load-to-use; **NB: device-dax mmap = 23 µs/byte — forbidden (DEV-040)** |
| **LDPC decode (CPU)** | **11,703 µs** (24 × 487.6 µs) → **23.4× over budget** | ~11 ms/CB on PoCL (functional only) | `calibration_check.txt` (measured) |
| **LDPC decode (real GPU)** | **within slot** (~6–24% of 500 µs; ~6× vs CPU) | n/a (PoCL is not a GPU) | "Six Times to Spare", arXiv:2602.04652 (Blackwell GB10) |
| CRC24 + verify | < 10 µs | < 50 µs | — |

### 3.3 Headline results this pipeline should produce

- **CPU-only LDPC: 23.4× over the 500 µs slot budget** (11,703 µs/slot). This is the measured anchor
  and must remain unchanged.
- **Real-GPU LDPC: back inside the slot** (~6× speedup, within 6–24% of budget) — the projected
  payoff of offload (cite, do not fabricate; PoCL cannot produce this number).
- **CXL hand-off cost is negligible when backed by system-RAM NUMA** (few µs/slot) — this is the
  argument that a coherent shared-memory fabric between CPU and accelerator does not erode the
  offload gain. (On real CXL: ~150–250 ns/access load-to-use.)
- **Live bit-error / CRC oracle**: 0 errors at high SNR, rising at low SNR — proves the decode is
  real, not a pass-through.

---

## 4. Timing model (make-or-break constraint)

QEMU cannot meet O-RAN fronthaul real-time deadlines (fronthaul requires ±1.5 µs sync and µs-scale
delivery windows; virtio-net in a guest cannot). Therefore:

- The pipeline runs **non-real-time**: the generator is **backpressured** (emit slot N, wait for
  ACK/gap, then N+1); the guest processes each slot to completion without a slot deadline.
- All latencies are reported as **measured per-stage processing time**, with the 500 µs slot budget
  shown only as a **reference line**.
- Never claim real-time compliance. The real-time story belongs to §5's real-hardware architecture,
  not the emulation.

---

## 5. Hardware architecture

Two views: (A) the **emulation testbed** that runs this PoC today, and (B) the **real target O-DU**
the emulation stands in for. The software is identical across both; only the hardware under the CXL
NUMA node and the OpenCL device change.

### 5.A Emulation testbed (what we run now)

```
        ┌──────────────────────────── Cloud host / bare-metal server ───────────────────────────┐
        │  CPU: 2× Xeon (Sapphire Rapids / Cascade Lake)   ← 2 physical NUMA sockets              │
        │                                                                                         │
        │   ┌── Traffic Generator (host process) ──┐        nested KVM                            │
        │   │  srsRAN encode/mod → eCPRI → UDP      │                                             │
        │   └───────────────┬───────────────────────┘                                            │
        │                   │ virtio-net (tap/bridge)                                             │
        │   ┌───────────────▼──────────── QEMU 8.2+ guest (q35, cxl=on) ───────────────────────┐  │
        │   │  vCPUs (pinned, isolcpus)                                                         │  │
        │   │  virtio-net NIC ── eCPRI RX                                                        │  │
        │   │  ┌─ NUMA node 0: guest DRAM (RAN/PHY/consumer)                                     │  │
        │   │  └─ NUMA node 1: CXL  ◄── QEMU pxb-cxl → cxl-rp → cxl-type3                        │  │
        │   │        (memory-backend-file, share=on)  → daxctl -m system-ram → node 1           │  │
        │   │  OpenCL device = PoCL (CPU)  ← "GPU" stand-in                                       │  │
        │   └───────────────────────────────────────────────────────────────────────────────────┘  │
        └─────────────────────────────────────────────────────────────────────────────────────────┘
```

- "CXL" = QEMU `cxl-type3` volatile device surfaced through the real Linux `drivers/cxl/` stack,
  brought online as a **system-RAM NUMA node** (DRAM-speed; not device-dax). Functional only —
  QEMU models neither CXL coherency nor CXL latency.
- "GPU" = PoCL CPU OpenCL. Functional offload model, not GPU performance.
- The 2-socket host is required only for the **latency-sensitivity overlay** (§9 M4a): place the CXL
  shm on a **CPU-less remote NUMA socket** (Pond / emucxl) to obtain a real cross-socket latency
  delta. The functional pipeline (M0–M3) runs on a single socket.

### 5.B Real target O-DU (what the emulation represents)

```
   ┌────────┐  eCPRI / O-RAN 7.2x   ┌──────────────── O-DU server (2-socket) ────────────────┐
   │  O-RU   │══════════════════════▶│  Fronthaul NIC / DPU  (HW PTP 1588, SR-IOV, eCPRI off) │
   │ (radio) │   25/100/200 GbE      │        │ DPDK poll-mode                                 │
   └────────┘   + PTP grandmaster    │        ▼                                                │
        ▲                             │  CPU: 2× Xeon SPR/EMR (AVX-512), DDR5                   │
        │ GNSS/PTP sync               │        │ PHY: chan-est, equalize, soft-demap            │
   ┌────┴────┐                        │        ▼ LLRs                                           │
   │ PTP GM   │                        │  ╔═══ CXL 2.0 fabric ═══╗                              │
   └─────────┘                        │  ║ CXL Type-3 mem pool   ║  shared CPU↔accelerator      │
                                      │  ╚═══════════╤═══════════╝                              │
                                      │              ▼                                          │
                                      │  LDPC accelerator:                                      │
                                      │   • GPU (NVIDIA H100 / GB200 Grace-coherent), OR        │
                                      │   • inline FEC ASIC (Intel ACC100/ACC200 vRAN Boost), OR│
                                      │   • FPGA (AMD/Xilinx T2 Telco)                           │
                                      │              │ decoded TB → CXL/MAC                      │
                                      └──────────────┴──────────────────────────────────────────┘
```

- The **CXL Type-3 memory pool** is the real analogue of our CXL NUMA node — a coherent region both
  the CPU and (aspirationally, via CXL Type-2 / coherent GPU attach) the accelerator can address.
- The **coherent CPU↔GPU sharing** (your NVLink-via-CXL idea) maps to **CXL.cache / Type-2** or a
  Grace-Hopper/GB200-style coherent link. This is **frontier hardware** — see §6 note.

---

## 6. Hardware architecture details

- **Fronthaul split (7.2x):** RU performs FFT/iFFT, CP, PRACH, digital beamforming; DU performs
  channel estimation, equalization, demapping, descrambling, **LDPC decode**, and higher L1. Only
  frequency-domain IQ (resource elements) crosses the fronthaul → smaller payload, and the DU never
  runs an FFT (matches `pusch_processor`).
- **Timing/sync:** O-RAN fronthaul requires PTP (IEEE 1588) + SyncE, category-A/B ±1.5 µs at the
  RU. In emulation this is dropped (non-RT, §4); on real HW it needs a PTP grandmaster + a
  timing-capable NIC.
- **LDPC accelerator options (real HW):**
  - **GPU** (NVIDIA H100 / GB200): most flexible, best for research; ~6× over CPU (Six Times to
    Spare). Coherent host↔GPU memory on Grace-Hopper/GB200 is the closest real thing to your
    "CXL.mem shared with GPU" vision.
  - **Inline FEC ASIC** (Intel vRAN Boost / ACC100/ACC200): lowest latency/power, production vRAN
    path; LDPC + rate-match offloaded via DPDK BBDev.
  - **FPGA** (AMD/Xilinx T2): middle ground, deterministic latency.
- **CXL device classes:** Type-3 = memory expander/pool (available today, our NUMA analogue).
  Type-2 = coherent accelerator with CXL.cache (what a coherent GPU-over-CXL would be) — **not in
  mainline QEMU; not generally available in silicon/cloud as of 2026.** The GPU-coherency half of
  the vision is therefore aspirational and cannot be emulated faithfully today.

---

## 7. Hardware configuration to bring up the workload

Concrete, reproducible configuration. Two tiers: **(7.1) the emulation tier** you can stand up now,
and **(7.2) the real-hardware tier** a production/lab deployment needs.

### 7.1 Emulation tier — bring-up config (runs M0–M3 now)

**Host (cloud or bare-metal):**
- Instance: **GCP `n2-standard-128`** (128 vCPU / 512 GB) to guarantee **2 NUMA sockets** for the
  latency overlay; a smaller box (e.g., `n2-standard-16`) suffices for M0–M3 functional work if the
  latency overlay (M4a) is deferred. Spot pricing; run for the measurement window only (~$12–37).
- Host OS: **Ubuntu 24.04**, kernel **≥ 6.8** with `CONFIG_CXL_BUS`, `CONFIG_CXL_MEM`,
  `CONFIG_CXL_ACPI`, `CONFIG_DEV_DAX_KMEM`; **nested KVM enabled** (`kvm-intel nested=1`).
- Verify 2 NUMA nodes (only needed for M4a): `numactl --hardware` → "available: 2 nodes".

**QEMU (≥ 8.2) guest launch — the CXL + NIC essentials:**
- Machine: `-machine q35,cxl=on -enable-kvm -cpu host,-hypervisor` (the `-hypervisor` bit-clear is
  required for the CXL guest driver path).
- CXL device chain: `pxb-cxl` bus → `cxl-rp` root port → `cxl-type3` with
  `-object memory-backend-file,id=cxl-mem0,share=on,mem-path=<file>,size=2G` and
  `persistent-memdev=cxl-mem0,lsa=cxl-lsa0` (a RAM-backed LSA; **not** `volatile-memdev`, which
  fails to enumerate on 8.2).
- NIC: `virtio-net` on a tap/bridge to the host generator (DPDK/SR-IOV is M4d only).
- Memory/CPU: back guest RAM with hugepages where possible; pin vCPUs.

**Guest CXL bring-up (per boot):**
- `daxctl reconfigure-device --mode=system-ram dax0.0` then `daxctl online-memory dax0.0` → CXL
  appears as **NUMA node 1 (system-RAM)**.
- Allocate the shm ring with `numa_alloc_onnode(size, 1)` (or `shm_open` + `mbind(MPOL_BIND, node1)`).
  **Do not** mmap `/dev/dax*` directly (23 µs/byte path). Verify placement with `get_mempolicy`.
- **CET/shadow-stack caveat (DEV-033):** `numactl --membind=1` may SIGILL under CET; use programmatic
  `mbind`/`numa_alloc_onnode` from the process instead.

**Accelerator (emulation):** PoCL OpenCL ICD (`pocl-opencl-icd`, `ocl-icd-opencl-dev`). Because the
CXL region is now **real system-RAM**, the earlier device-dax SIMD SIGILL (DEV-038/042) should not
occur — verify at M2 and keep the scalar-copy fallback only if it recurs.

**Isolation for cleaner latency numbers:** `isolcpus=` + `nohz_full=` for the PHY and decode
threads; disable turbo/C-states variability if measuring.

### 7.2 Real-hardware tier — config to run the true workload

**O-DU server:**
- CPU: **2× Intel Xeon Sapphire Rapids or Emerald Rapids**, ≥ 32 cores/socket, AVX-512, **CXL
  1.1/2.0** enabled in BIOS.
- Memory: **DDR5-4800+** per socket **+ a CXL Type-3 memory expander** (e.g., Samsung CMM-D,
  Astera Leo, Micron CZ120) brought online as a system-RAM NUMA node — the real version of our
  node 1.
- **Accelerator (choose one):**
  - NVIDIA **H100 / GB200 Grace** (coherent host memory = closest to the CXL.mem-shared-GPU goal), or
  - Intel **ACC100 / ACC200 (vRAN Boost)** inline FEC via DPDK **BBDev**, or
  - AMD/Xilinx **T2 Telco** FPGA.
- NIC/DPU: **NVIDIA ConnectX-7 (200 GbE)** or **BlueField-3 DPU** with **hardware PTP (IEEE 1588)**,
  SyncE, SR-IOV, and eCPRI/O-RAN 7.2x offload.
- Timing: **PTP grandmaster + GNSS** for ±1.5 µs fronthaul sync.

**BIOS / platform:**
- Enable: **VT-d/IOMMU**, SR-IOV, CXL, hugepages (1 GB), sub-NUMA clustering *disabled* (simpler
  NUMA), PTP/SyncE.
- Disable: deep C-states, C1E, and frequency scaling for deterministic latency.

**OS / software:**
- **PREEMPT_RT** (real-time) kernel for the DU; `isolcpus`, `nohz_full`, `rcu_nocbs` for PHY/FEC
  cores; DPDK with 1 GB hugepages; SR-IOV VFs bound to the poll-mode driver.
- FEC offload via **DPDK BBDev** (for ASIC/FPGA) or CUDA/cuPHY-style path (for GPU).

**Latency-fidelity note:** even on this tier, CXL Type-2 (coherent accelerator over CXL.cache) is
not yet generally available; the coherent CPU↔GPU sharing is achievable *today* only via
Grace-Hopper/GB200-class NVLink-C2C, not commodity CXL. Treat "LDPC results shared with the GPU over
CXL.mem coherently" as the research target, and validate memory-placement/traffic arguments with a
latency model (CXLMemSim / Mess) plus the emucxl NUMA sensitivity (§9 M4a).

---

## 8. Honesty constraints (acceptance gates)

Carried from the prior audit; all results must pass these:
- **A — no synthetic numbers:** every latency is measured from code that ran (the 23.4× anchor and
  the GPU 6× are *cited*, clearly labeled as such, never presented as this pipeline's own real-time
  measurement).
- **B — code runs:** PHY and decoder provably execute; outputs depend on inputs.
- **C — no stub kernels:** real srsRAN PHY; no memcpy-equalize, no random LLRs.
- **D — CXL in the path for every codeblock:** LLRs and TBs transit CXL shm every CB; assert a counter.
- **E — sequential vs pipelined reported honestly.**
- **Emulation labels** ("CXL" = system-ram NUMA node; "GPU" = PoCL CPU) appear in the README/results.

---

## 9. Milestones (for the implementer; gates only)

| M | Goal | Accept when |
|---|---|---|
| **M0** | eCPRI loopback | Received RE grid matches transmitted within tolerance, ≥100 slots; loss/reorder handled. |
| **M1** | Real PHY → real LLR | LLRs from srsRAN soft-demapper; hard-decision agreement > 99% at 20 dB. No l1_sim, no random LLR. |
| **M2** | LDPC offload via CXL + oracle | CRC pass & `bit_errors==0` at high SNR (BG1+BG2, Z=384, ≥500 slots); errors rise as SNR drops; CXL on NUMA node 1 verified; every CB transits CXL. |
| **M3** | Sustained e2e + honest results | ≥2000 slots, SNR sweep, real latencies, monotonic BER curve, no ring overflow/replay, self-audit vs A–E. |
| **M4a** | Latency sensitivity (emucxl/Pond) | 2-NUMA host, CXL shm on CPU-less remote socket, real node0-vs-node1 decode-latency delta. |
| **M4b** | Transparent intercept | bpftime uprobe offload trigger on the *real* PHY (no PHY source change). |
| **M4c** | Real GPU | `ldpc_decode.cl` on a real GPU → real offload-speedup number. |
| **M4d** | DPDK/SR-IOV NIC | virtio-net replaced with passed-through NIC for realistic ingress. |

---

## 10. Risks & mitigations

| Risk | Mitigation |
|---|---|
| OFH real-time impossible in QEMU | Non-real-time, backpressured generator (§4). |
| CXL device-dax 23 µs/byte cliff | system-RAM NUMA node, never `/dev/dax` mmap (§7.1). |
| PHY integration effort | Start from `pusch_processor_benchmark` harness, strip to UL RX. |
| PoCL SIMD SIGILL (was device-dax-only) | Real system-RAM backing should remove it; verify at M2, keep scalar fallback. |
| Ring overflow / replay (DEV-045) | Bound producer to ring capacity or size ring ≥ slot count; assert unique-slot count. |
| Cross-clock latency bug | Each interval measured within one process/clock. |
| GPU-coherency half not emulable | Label as research target; validate with CXLMemSim/Mess + emucxl NUMA sensitivity. |
```
