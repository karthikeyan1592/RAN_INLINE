# Open Inline — FROZEN Architecture

**Status:** 🔒 FROZEN (2026-07-05). This is the single source of truth. Do not re-litigate the
decisions in §8; build to them.
**Author:** Karthi (Nokia BSP Engineer, Bangalore)
**Target:** Prof. Arkaprava Basu, IISc Computer Systems Lab
**Research base:** `/root/linux_env/cxl/open_inline/`

---

## 1. Thesis (one line)

> **An open, vendor-portable GPU FEC/L1 for O-RAN:** a **SYCL/OpenCL LDPC decoder** exposed
> through the **open DPDK-BBDev / O-RAN-AAL seam**, with **DMA-BUF/RoCE** for NIC→GPU ingress and
> **CXL** for CPU↔GPU sharing/pooling — an open-standards alternative to NVIDIA's fully-proprietary
> Aerial (CUDA + GPUDirect + DOCA).

---

## 2. Problem (corrected framing)

NVIDIA proved GPU 5G-L1 works (Aerial/cuPHY; Sionna-RK; "Six Times to Spare" → GPU LDPC within
6–24% of the 500 µs slot). NVIDIA even **open-sourced cuPHY** (Apache 2.0). But the deployable
path is still **proprietary lock-in end to end**:

- **Compute:** CUDA (NVIDIA-only).
- **NIC→GPU datapath:** GPUDirect + DOCA GPUNetIO/GDAKIN (NVIDIA-only).
- **CPU↔GPU link:** NVLink-C2C (NVIDIA-only).

"Open" here does **not** mean "no vendor" (every GPU/NIC has a driver). It means **open standards +
open source + multi-vendor portability**, avoiding *proprietary* lock-in. Porting to AMD/HIP would
just swap NVIDIA lock-in for AMD lock-in — so we target **portable open standards**, not one vendor.

---

## 3. The frozen open-standards architecture

```
 Fronthaul (eCPRI / Ethernet)
        │
        ▼
 RoCE NIC ──(DMA-BUF P2P, open, any vendor)──►  GPU VRAM (NVIDIA / AMD / Intel)
        ▲                                             │
   CPU control path (DPDK, open;                      ▼  compute in SYCL/OpenCL (portable):
   open GPU-initiated networking = future track)   ┌─ LDPC decode (our OpenCL kernel) ─┐
                                                    │  … more cuPHY-ported L1 kernels   │
                                                    └───────────────┬───────────────────┘
                                                                    │ result
        DPDK-BBDev / O-RAN-AAL seam ◄───────── plugged in via ──────┘
        (OAI/srsRAN call the GPU decoder through the standard AAL interface)
                                                                    │
        CXL (open standard) ── CPU↔GPU shared memory + multi-vDU/GPU pooling
```

Every arrow is an **open standard**: RoCE (IBTA), DMA-BUF (Linux mainline), SYCL/OpenCL (Khronos),
DPDK-BBDev / O-RAN-AAL (DPDK + O-RAN Alliance), CXL (CXL Consortium). Nothing proprietary.

---

## 4. Component decisions (frozen)

| Layer | Choice | Why |
|---|---|---|
| GPU compute | **SYCL / OpenCL** (NOT CUDA, NOT HIP) | Khronos open standards; run on NVIDIA+AMD+Intel. We already own a bit-exact **OpenCL** LDPC kernel. |
| Offload seam | **DPDK-BBDev / O-RAN-AAL** | Standardized open FEC-offload interface OAI/srsRAN already use. NOT eBPF/uprobe. |
| NIC→GPU ingress | **DMA-BUF + RoCE** | Mainline, vendor-neutral. This — not CXL — is the correct tool for NIC→GPU. |
| CPU↔GPU / pooling | **CXL Type-3** (system-ram NUMA) | Open standard; used for shared memory + multi-vDU pooling (its real strength). |
| GPU-initiated networking | **UCCL-style** (future track) | Open, vendor-agnostic; today built for AI collectives, not RAN — adaptation is Track 2. |
| CPU RAN stack | **OAI** (`nrLDPC_coding` / AAL), srsRAN | Open; the AAL seam is the plug-in point. |
| GPU L1 reference | open **cuPHY** (Apache 2.0, CUDA) | Reference to port kernels from. |

---

## 5. Reuse map vs. novelty (what exists open vs. the gaps)

**Reuse (already open — do not rebuild):** SYCL (AdaptiveCpp, Intel DPC++), OpenCL (PoCL),
DMA-BUF, DPDK-BBDev + O-RAN-SC `o-du-phy` AAL, OAI/srsRAN, UCCL (GPU-initiated networking),
open cuPHY (CUDA reference), our existing OpenCL LDPC kernel (0-mismatch, BG1/BG2, Z=384/256).

**Gaps = our contribution:**
1. **Open, vendor-portable GPU 5G-L1 (LDPC) in SYCL/OpenCL** — none exists (all CUDA). We have the seed.
2. **A GPU BBDev / AAL driver** — all BBDev drivers today are FPGA/ASIC (ACC100/ACC200/T2). **No GPU BBDev PMD.** ← cleanest novelty.
3. **RAN-specific open GPU-initiated fronthaul ingest** — UCCL/GIN exist for AI, not eCPRI. (Hard; Track 2.)

---

## 6. Scope & tracks (frozen)

- **Track 1 — TRACTABLE (the deliverable):** GPU FEC behind the open AAL/BBDev seam.
  LDPC first (lookaside), roadmap to more cuPHY-ported L1 kernels. CXL for CPU↔GPU sharing.
- **Track 2 — RESEARCH (future/hard):** open, vendor-portable **GPU-initiated fronthaul datapath**
  (NIC→GPU with GPU controlling the NIC, CPU out of the control path) — the open GDAKIN equivalent.
- **Inline full-L1 on GPU** (Aerial/ARC-Pro model) = long-term vision, reachable by porting more of
  open cuPHY to SYCL/OpenCL over time. NOT the first deliverable.

---

## 7. First deliverable (concrete)

**A GPU BBDev driver wrapping the OpenCL LDPC kernel**, loaded by OAI/srsRAN via the standard O-RAN
AAL seam, benchmarked against the CPU BBDev software decoder and the ACC100 — on any GPU vendor.
- Validates: portable GPU FEC through the open seam, correctness (bit-exact vs CPU), latency vs CPU
  baseline (our fixed 23.4× / 11,703 µs anchor) and vs the ~6× GPU envelope (Six Times to Spare).
- Upstreamable: MR proposing a GPU BBDev PMD; O-RAN WG6 (AAL) contribution (Nokia is a member).

---

## 8. Frozen decisions & corrections (do NOT re-litigate)

1. **Compute = SYCL/OpenCL, not CUDA and not HIP/ROCm.** HIP = AMD lock-in; we want portability.
   Reuse the existing OpenCL LDPC kernel.
2. **cuPHY is OPEN (Apache 2.0)** — the "no open GPU RAN exists" premise is dead. We differentiate
   on *portable + open-standards*, not "first open GPU RAN."
3. **CXL is NOT the NIC→GPU path.** NIC→GPU = **DMA-BUF/RoCE**. CXL = CPU↔GPU sharing + pooling.
4. **No "zero-copy" claims.** GPU access to CXL is a **streaming DMA stage** (~20 GB/s, ~660 ns per
   CCCL), not in-kernel zero-copy. CXL's wins are pooling/disaggregation/capacity, not latency.
5. **"Open" = open standards + open source + multi-vendor**, not "vendorless." State this explicitly.
6. **Offload via the AAL/BBDev seam (or OAI `nrLDPC_coding`), not eBPF/uprobe.** eBPF is dropped
   from the core (it was scaffolding; there is nothing to sniff — we own the seam).
7. **The one irreducible proprietary gap = GPU-initiated networking (GDAKIN/GPUNetIO).** Either
   accept CPU-in-control-path (open, works today) or make "open GDAKIN for RAN" the Track-2 research.
8. **PRIMARY_CONFIG anchor unchanged:** CPU LDPC = 11,703 µs/slot = 23.4× over the 500 µs budget.
9. **Honesty gates carried forward:** real measurements only (no synthetic numbers), code provably
   runs, no stub kernels, real bit-exact oracle, sequential-vs-pipelined reported honestly.

---

## 9. Validation environments

- **Now (functional):** WSL / QEMU CXL emulation + PoCL (OpenCL on CPU). Proves the integration
  layer and correctness — NOT latency. CXL = system-ram NUMA node (never `/dev/dax` mmap).
- **Latency-sensitivity (emulated):** 2-NUMA host (GCP `n2-standard-128` or bare metal), CPU-less
  remote node (Pond/emucxl method).
- **Hardware (real):** IISc HACC — needs a **CXL-capable CPU + CXL Type-3 module + a GPU** on one
  server, and DMA-BUF/RoCE for the ingress path. (Key questions for Prof. Basu.)

---

## 10. Publication & positioning

- **Primary:** IEEE Networking / Communications Letters (4 pp) — "An Open, Vendor-Portable GPU FEC
  for O-RAN via DPDK-BBDev/AAL."
- **Secondary:** O-RAN Alliance WG6 (AAL) technical contribution — open AAL GPU implementation.
- **Positioning vs prior art:** NVIDIA Aerial/cuPHY (CUDA, proprietary datapath); Intel/AAL FEC
  (FPGA/ASIC only); Sionna-RK (CUDA + neural); CCCL (GPU+CXL, not RAN); DecodeX (LDPC across
  platforms, CUDA/ASIC). **Gap we fill:** portable (SYCL/OpenCL) GPU behind the open AAL/BBDev seam.

---

## 11. References (verified)

- Open cuPHY (Apache 2.0): https://github.com/NVIDIA/aerial-cuda-accelerated-ran
- Sionna-RK (arXiv:2505.15848): https://arxiv.org/abs/2505.15848
- Six Times to Spare (arXiv:2602.04652): https://arxiv.org/abs/2602.04652
- CCCL — GPU over CXL pooling (arXiv:2602.22457): https://arxiv.org/html/2602.22457v1
- DecodeX — LDPC CPU/GPU/ASIC (arXiv:2511.02952): https://arxiv.org/html/2511.02952v1
- UCCL — open vendor-agnostic GPU-initiated networking: https://github.com/uccl-project/uccl
- GPU-Initiated Networking for NCCL (arXiv:2511.15076): https://arxiv.org/html/2511.15076v1
- DPDK BBDev in O-RAN-SC o-du-phy: https://deepwiki.com/o-ran-sc/o-du-phy/4.5-dpdk-bbdev
- DPDK BBDev programming guide: https://doc.dpdk.org/guides-24.11/prog_guide/bbdev.html
- GPUDirect RDMA → DMA-BUF (vendor-neutral): https://kubernetes.recipes/recipes/networking/switch-gpudirect-rdma-dma-buf/
- Hardware Acceleration for Open RAN — overview (arXiv:2305.09588): https://arxiv.org/pdf/2305.09588
- emucxl (IIT Bombay, arXiv:2404.08311): https://arxiv.org/abs/2404.08311
- Pond (ASPLOS'23, arXiv:2203.00241): https://arxiv.org/abs/2203.00241
- SYCL: AdaptiveCpp (https://github.com/AdaptiveCpp/AdaptiveCpp), Intel DPC++/oneAPI
- OpenAirInterface: https://gitlab.eurecom.fr/oai/openairinterface5g
- 3GPP TS 38.212 (LDPC base graphs)

---

## 12. Superseded documents (do NOT use)

- `../Evolved Arch/Final_Arch.md` — SUPERSEDED (cuPHY-closed premise wrong; NVLink/zero-copy errors).
- `../Evolved Arch/Final_Arch_v2.md` — SUPERSEDED (still assumed cuPHY closed; pre-open-cuPHY).
- `../E2E_ARCH_SPEC.md`, `../E2E_HLD.md`, `../E2E_LLD.md`, `../EBPF_OFFLOAD_ARCH.md`,
  `../IMPLEMENTER_PROMPT.md` — SUPERSEDED (eCPRI-into-QEMU + eBPF direction; replaced by AAL/BBDev
  + SYCL/OpenCL here). Keep for history only.

**This file (`open_inline/ARCHITECTURE.md`) supersedes all of the above.**

---

> **Note:** the phased amendment of this document (per `research/feasibility_research.md`, which
> demotes CXL and adds Phase-0 gates) lives in `ARCHITECTURE_v2.md`. This v1 is retained unchanged.
