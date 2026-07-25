# Open CXL-Backed GPU L1 Acceleration for 5G NR RAN
## Porting the Sionna Research Kit LDPC Decoder from CUDA/NVIDIA to ROCm + CXL

**Author:** Karthi (Nokia BSP Engineer, Bangalore)
**Status:** Research Proposal — v2 (revised after literature verification)
**Target:** Prof. Arkaprava Basu, IISc Computer Systems Lab

> **v2 changelog (corrections from a verification pass against primary sources):**
> 1. **Problem statement (§1):** the hard lock-in is **CUDA**, not NVLink-C2C. Sionna-RK's LDPC
>    kernel already runs on a *discrete* x86 + RTX 4090; unified/NVLink memory is an
>    *optimization* on integrated platforms, not a hard requirement.
> 2. **Memory mechanism (§3):** dropped the "zero-copy, GPU reads CXL directly" claim. The only
>    demonstrated GPU-over-CXL system (CCCL) uses a **bulk DMA stage (CXL↔VRAM)**; CXL is a
>    streaming path (~20 GB/s, 658 ns latency). CXL is reframed as a *measured staging fabric*, and
>    its honest advantage is (a) eliminating the CPU-side staging copy and (b) multi-host pooling.
> 3. **Prior art (§5):** added **CCCL** (GPU+CXL) and **DecodeX** (LDPC across CPU/GPU/ASIC); the
>    novelty is narrowed to *RAN-specific, non-NVIDIA-GPU, on the OAI/O-RAN-AAL plugin seam.*
> All references in §11 are verified.

---

## 1. Problem Statement

NVIDIA's Sionna Research Kit (Sionna-RK, arXiv:2505.15848, IEEE ICMLCN 2025) demonstrates that
GPU acceleration of 5G NR L1 LDPC decode is practical: an independent characterization
("Six Times to Spare," arXiv:2602.04652) shows GPU LDPC decode staying **within 6–24% of the
500 µs slot budget** at only **+10–15 W**, versus CPU decode that can exceed the slot at moderate
iteration counts. GPU offload of LDPC is real and worthwhile.

But Sionna-RK's LDPC acceleration carries NVIDIA-specific assumptions that block deployment on
open/commodity infrastructure:

**Hard dependency — CUDA (the real vendor lock-in).**
The LDPC decode kernel (`plugins/ldpc_cuda/src/runtime/ldpc_decoder.cu`, Apache-2.0, open source)
is written in CUDA and runs only on NVIDIA GPUs with the NVIDIA runtime. It cannot execute on AMD,
Intel, or any non-NVIDIA accelerator. This is the binding constraint.

**Soft dependency — an NVIDIA coherent/unified memory fabric for the best-case data path.**
On integrated NVIDIA platforms (Jetson AGX Orin/Thor, DGX Spark = Grace+Blackwell via NVLink-C2C),
CPU and GPU share one physical memory pool, so the OAI→kernel LLR handoff needs **no explicit
copy**. On a *commodity server with a discrete GPU*, that unified path does not exist — the same
kernel still runs, but the CPU↔GPU LLR handoff falls back to an **explicit PCIe DMA copy**. So the
"no-copy" advantage is tied to NVIDIA's integrated memory, and it disappears on open hardware.

The result: on commodity servers with non-NVIDIA GPUs, Sionna-RK's GPU L1 path is (a) impossible
(CUDA), and (b) even for NVIDIA discrete GPUs, loses the zero-copy handoff. **No open, non-NVIDIA
GPU L1 LDPC path exists, and no open shared-memory fabric has been evaluated as a substitute for
NVIDIA's unified memory.**

### Research Question

> *Can an open GPU runtime (ROCm/HIP) plus CXL Type-3 shared memory provide a deployable,
> non-NVIDIA path for real-time 5G NR L1 LDPC offload on commodity servers — and how much does the
> CXL staging fabric cost against the 500 µs slot budget compared to NVIDIA's integrated memory?*

---

## 2. Proposed Solution

### Core Idea

Replace Sionna-RK's two NVIDIA-specific pieces with open equivalents, keeping the rest identical.

| Sionna-RK (NVIDIA) | This Work (Open) |
|---|---|
| CUDA LDPC kernel (`ldpc_decoder.cu`) | ROCm/HIP LDPC kernel (hipify port) |
| Unified/NVLink-C2C CPU-GPU memory | CXL Type-3 shared memory (staging fabric) |
| Grace CPU + Blackwell/Orin GPU | Any x86/ARM CPU + any AMD GPU |
| DGX Spark / Jetson | Commodity PCIe server + CXL module |

Unchanged from Sionna-RK:
- OAI as the RAN stack (unmodified).
- The **OAI LDPC coding plugin interface** (`nrLDPC_coding_interface`, `load_nrLDPC_coding_interface`)
  as the sanctioned integration seam — the same mechanism used by real FEC accelerators (Intel
  ACC100, AMD/Xilinx T2) via the O-RAN Acceleration Abstraction Layer (AAL). **No source
  modification, no eBPF/uprobe interception.**
- The same LDPC algorithm (layered normalized min-sum, BG1/BG2).
- The same `rfsim` simulation mode for testing.

### What Changes and What Does Not

```
SIONNA-RK (existing):                    THIS WORK (proposed):

OAI gNB (Grace CPU)                      OAI gNB (any CPU)
  L1 pipeline: FFT, ch.est, eq, LLR        L1 pipeline: FFT, ch.est, eq, LLR
        │                                          │
        │ OAI nrLDPC_coding plugin                 │ OAI nrLDPC_coding plugin
        │ (sanctioned seam, no src change)         │ (sanctioned seam, no src change)
        ▼                                          ▼
  Unified / NVLink-C2C   ←── REPLACE ──→   CXL Type-3 shared memory
  (integrated NVIDIA only,                 (open standard, PCIe-based, any vendor;
   coherent, no copy)                       CPU writes LLR in place; GPU DMA-stages)
        │                                          │
        ▼                                          ▼
  NVIDIA GPU             ←── REPLACE ──→    AMD GPU (MI210 or similar)
  CUDA LDPC kernel                          ROCm/HIP LDPC kernel
  (ldpc_decoder.cu)                         (hipify port of the .cu)
        │                                          │
        ▼                                          ▼
  result back (unified)                     result back via CXL
  OAI continues                             OAI continues
```

---

## 3. How CXL Substitutes for NVIDIA Unified Memory

### What NVIDIA's fabric does
On DGX Spark/Jetson, CPU and GPU share one physical pool (NVLink-C2C / integrated memory). When the
OAI plugin hands the LLR buffer pointer to the CUDA kernel, both sides address the same pages — no
copy, coherent. On a discrete NVIDIA GPU there is no such sharing; the handoff is an explicit PCIe
`cudaMemcpy`.

### What CXL provides instead (honest mechanism — measured, not zero-copy)

CXL Type-3 memory is exposed as a CPU-less NUMA node (system-ram via `daxctl`, or Device-DAX).
The realistic data path — confirmed by the only published GPU-over-CXL system (CCCL,
arXiv:2602.22457) — is:

- **CPU side (real win):** OAI's L1 pipeline allocates its LLR buffers *on the CXL node*
  (`numa_alloc_onnode(size, cxl_node)`). The LLR is **born in the shared region** — this eliminates
  the CPU-side copy into a pinned DMA staging buffer that a plain discrete-GPU path requires.

- **GPU side (a streaming DMA, not zero-copy):** the CXL pages are pinned with
  `hipHostRegister(cxl_ptr, size, ...)` and the kernel is fed the LLR via a **bulk DMA stage
  CXL→VRAM, decode in VRAM, DMA result VRAM→CXL** (`hipMemcpyAsync`). This is *not* in-kernel
  zero-copy: an iterative min-sum decoder doing random reads directly over PCIe-to-CXL would be far
  slower than reading VRAM, so a streaming stage is the correct design. (On AMD parts with XNACK,
  `hipHostRegister` is not required — automatic page migration applies.)

So the OAI plugin still hands the kernel a pointer into the CXL region; the plugin performs the
CXL↔VRAM streaming DMA around the kernel launch. One shared region, no CPU-side staging copy, one
streaming copy each way on the GPU side.

### Latency / bandwidth reality (from CCCL measurements)

| Fabric | CPU→GPU handoff | Notes | Vendor |
|---|---|---|---|
| NVIDIA unified / NVLink-C2C | ~0 copy (integrated) | coherent; integrated platforms only | NVIDIA only |
| **CXL Type-3 + hipHostRegister** | **~20 GB/s GPU DMA; CPU-side CXL access 658 ns (3.1× local DRAM)** | streaming stage; PCIe-Gen5-x8-bound; single DMA engine | Any vendor |
| Plain PCIe (DRAM→pinned→GPU) | ~similar GB/s + a CPU-side staging copy | baseline discrete path | Any vendor |

**Honest takeaway:** CXL does *not* make the GPU-side transfer dramatically faster than PCIe (both
are PCIe-bound at ~20 GB/s). CXL's advantages are (1) the CPU writes LLR **in place** (no CPU-side
staging copy), and (2) **multi-host pooling** (§6). Whether the CXL staging overhead fits the 500 µs
slot budget is an empirical result this work will **measure**, not assume.

---

## 4. Implementation Plan

### Step 1 — Fork Sionna-RK (Week 1)
Fork `NVlabs/sionna-rk` (Apache 2.0). Target files (verified to exist in the public repo):
```
plugins/ldpc_cuda/
  src/runtime/ldpc_decoder.cu      ← CUDA kernel → hipify to HIP
  src/runtime/memory_manager.*     ← CUDA memory mgmt → replace with CXL layer
  include/...                       ← OAI nrLDPC_coding plugin glue (keep interface)
  CMakeLists.txt                    ← CUDA → ROCm
```

### Step 2 — Port CUDA Kernel to HIP (Week 1–3)
`hipify-clang plugins/ldpc_cuda/src/runtime/ldpc_decoder.cu -o .../ldpc_decoder.hip`.
Mechanical for `cudaMalloc→hipMalloc`, `cudaMemcpy→hipMemcpy`, `__global__` unchanged,
`cudaStream_t→hipStream_t`.
**Budget real effort for the hard part:** LDPC min-sum check-node reductions typically use
warp-level intrinsics (shuffle/ballot); AMD CDNA has **wavefront 64** vs NVIDIA warp 32, plus
different LDS banking. This affects correctness and performance and needs re-tuning — treat it as
kernel work, not cleanup. **Fallback:** we already have a bit-exact OpenCL min-sum LDPC decoder
(0 mismatches, BG1/BG2, Z=384/256) that can back-stop or replace the hipified kernel if porting
stalls.

### Step 3 — CXL Memory Layer (Week 3–4)
Replace `memory_manager.cu` with a CXL-aware allocator:
```
memory_manager_cxl.c:
  cxl_init():   detect CXL NUMA node (/sys/bus/cxl/ or daxctl);
                numa_alloc_onnode(pool, cxl_node); hipHostRegister(pool,...)
  cxl_get_llr_buffer(cb):  return pool + cb*CB_STRIDE   // OAI writes LLR here (in place)
  decode(cb):   hipMemcpyAsync(vram, pool+cb*STRIDE, H2D);  launch kernel;
                hipMemcpyAsync(pool+out, vram_out, D2H)     // streaming stage, not zero-copy
  cxl_fini():   hipHostUnregister(pool); numa_free(pool)
```
Note (from CCCL): GPU pinning worked via **Device-DAX** (`/dev/dax0.0`); verify whether the ROCm
path prefers Device-DAX or system-ram NUMA for `hipHostRegister` on real CXL.

### Step 4 — Test with OAI rfsim (Week 4–5)
1. Launch OAI gNB in `rfsim`. 2. Load the ROCm LDPC plugin via `nrLDPC_coding_interface`.
3. OAI allocates LLR from the CXL pool. 4. L1 runs on CPU; at LDPC the plugin stages CXL→VRAM,
decodes on the AMD GPU, stages back. 5. OAI continues (CRC/HARQ/MAC).
**Verify:** bit-exact against OAI's software decoder (`nrLDPC_coding_segment`). **Measure:** per-CB
and per-slot latency, decomposed into CXL-stage vs kernel time.

### Step 5 — Measure and Compare (Week 5–6)
| Metric | CPU baseline | This work (CXL+MI210) | Reference (NVIDIA) |
|---|---|---|---|
| Per-CB LDPC latency | ~488 µs (I=20, AVX2) | measure | Six Times: within 6–24% of slot |
| Per-slot latency | 11,703 µs (23.4× over budget, fixed anchor) | measure | within 500 µs |
| CXL H2D/D2H stage overhead | N/A | **measure (the key result)** | N/A (unified) |
| CPU-side staging copies | — | **0 (LLR born in CXL)** | 0 (unified) |

---

## 5. Novelty and Prior Art

### Prior Art Table (revised)

| Work | GPU runtime | CXL | RAN | Open GPU stack |
|---|---|---|---|---|
| NVIDIA Aerial cuPHY | CUDA | ✗ | 5G NR | ✗ |
| Sionna-RK (2505.15848) | CUDA | ✗ | 5G NR OAI | kernel open, NVIDIA HW |
| Six Times to Spare (2602.04652) | CUDA | ✗ | 5G-style LDPC | ✗ |
| AtlasRAN (2603.14661) | CUDA | ✗ | OAI | ✗ |
| DecodeX (2511.02952) | CUDA/ASIC | ✗ | LDPC (not full RAN) | ✗ |
| **CCCL (2602.22457)** | **CUDA** | **CXL (pooling)** | **✗ (GPU collectives)** | ✗ |
| eGPU (HCDS'25) | OpenCL | CXL (future) | ML only | ✓ |
| **This work** | **ROCm/HIP** | **CXL Type-3** | **5G NR OAI** | **✓** |

### The Specific Gap (narrowed, honest)
GPU-over-CXL now has published art (**CCCL** does GPU collectives over CXL pooled memory), and GPU
5G LDPC is well-covered on CUDA (Sionna-RK, Six Times, DecodeX). **What remains open** is the
intersection: **5G NR L1 LDPC decode on a non-NVIDIA (ROCm/AMD) GPU, integrated through the O-RAN
AAL / OAI plugin seam, using CXL as the CPU↔GPU shared-memory fabric.** No existing work combines
non-NVIDIA GPU + RAN plugin integration + CXL fabric.

### The Honest Limitation
- **CXL Type-3 is not cache-coherent** (unlike NVIDIA unified memory). LDPC's one-directional
  dataflow (CPU writes LLR once → GPU reads once → GPU writes result → CPU reads once) does not
  need coherency, but the **CPU↔GPU handshake still requires explicit cache flush/fence** so each
  side observes the other's writes. This is managed in the plugin, not free.
- **CXL is a streaming fabric, not zero-copy:** the GPU-side transfer is a DMA stage (~20 GB/s),
  not the in-place, coherent access NVLink-C2C provides. CXL is therefore *sufficient* for this
  workload but *architecturally weaker* than NVIDIA's integrated memory; the contribution is
  showing it is *good enough within the slot budget*, not that it is faster.

---

## 6. CXL 3.0 Scalability Extension (the strongest CXL-only argument)

With CXL 3.0 fabric (multi-host pooling; hardware ~2026–2027):
```
CPU node 1 (OAI vDU 1) ─┐
CPU node 2 (OAI vDU 2) ─┤── CXL 3.0 pooled memory ── one GPU
CPU node 3 (OAI vDU 3) ─┤
CPU node N (OAI vDU N) ─┘
```
NVLink is per-chip: Sionna-RK / Aerial require one GPU per gNB. **CXL 3.0 pooling lets one GPU serve
N gNB instances over a shared pool** — a disaggregation/consolidation capability NVLink-C2C cannot
replicate. This is the forward-looking reason to prefer CXL over both NVLink and plain PCIe.

---

## 7. Hardware Requirements

### Simulation (today)
- Any x86 server with QEMU CXL emulation (Type-3 → NUMA node via `daxctl`).
- PoCL as an OpenCL CPU stand-in — proves the integration layer, **not** latency.
- No real GPU needed for functional validation.

### Full Validation (IISc HACC)
| Component | What | Why |
|---|---|---|
| CPU | Sapphire Rapids or Genoa | CXL 1.1/2.0 root port |
| CXL module | Samsung / Micron / Astera Type-3 DRAM | Real CXL latency (~200–660 ns) |
| GPU | AMD MI210 (ROCm 5.x+) | `hipHostRegister`, HIP kernel |
| OAI | recent, with `nrLDPC_coding` plugin API | the RAN stack |

**Key questions for Prof. Basu:** (1) Does IISc HACC have a CXL-capable CPU **and** a CXL Type-3
module **and** an MI210 on the *same* server? (2) For the GPU pin path, is CXL exposed as Device-DAX
or system-ram NUMA — and does ROCm `hipHostRegister` accept it? (These two answers gate full HW
validation.)

---

## 8. Relationship to Ongoing PoC Work

| PoC Result | Relevance | Honest status |
|---|---|---|
| Bit-exact OpenCL LDPC kernel (0 mismatches, BG1/BG2, Z=384/256) | Confirms the algorithm is correct in OpenCL → portable to ROCm; can back-stop the hipify port | **Verified** |
| CXL Type-3 kernel driver path (QEMU, verify checks) | Confirms the Linux CXL stack + NUMA-node bring-up works | **Verified** |
| 23.4× CPU baseline (11,703 µs/slot, fixed anchor) | Quantifies the gap GPU closes | **Verified** |
| OpenCL reading from CXL-backed memory | Intended data path | **NOT yet demonstrated on real path** — the prior sentinel run read from a stack buffer, not CXL, due to a QEMU device-memory SIMD limitation. Demonstrating true OpenCL/HIP-from-CXL is a goal of this work, not a completed result. |

The PoC previously explored a bpftime-uprobe integration; **this proposal drops that** in favor of
OAI's sanctioned `nrLDPC_coding` plugin seam — the same interface real FEC accelerators use — which
is cleaner, deterministic, and needs no source modification or interception.

---

## 9. Contribution Plan

**Code:** Fork `NVlabs/sionna-rk` → `open-cxl-ran/sionna-rk-rocm` (Apache 2.0).
- `plugins/ldpc_rocm/` — ROCm/HIP port of the LDPC plugin.
- `plugins/ldpc_rocm/memory_manager_cxl.c` — CXL allocator + streaming stage.
- `cmake/FindROCm.cmake` — ROCm build.
- Tutorial: "GPU-Accelerated LDPC Decoding on AMD GPU via CXL."
- **Upstream:** MR to OAI adding a ROCm LDPC backend alongside the CUDA one — closes the "CUDA-only"
  gap in OAI's accelerator ecosystem.

**Publication:**
- **Primary:** IEEE Networking Letters / Communications Letters (4 pp) — "CXL-Backed GPU
  Acceleration of 5G NR LDPC Decode: An Open, Non-NVIDIA Alternative."
- **Secondary:** O-RAN Alliance WG6 (AAL) technical contribution (Nokia is a WG6 member) —
  an open AAL FEC-offload implementation.

---

## 10. Next Steps

1. **Read the actual `plugins/ldpc_cuda/` source** — confirm the `nrLDPC_coding_interface` glue and
   the exact `memory_manager` handoff (unified-memory vs explicit copy branches).
2. **Fork + hipify** `ldpc_decoder.cu`; assess CDNA wavefront-64 re-tuning effort.
3. **CXL memory manager** — write `memory_manager_cxl.c`; validate on QEMU CXL + PoCL first, then
   real HW.
4. **IISc meeting** — present v2; confirm CXL-module + MI210 co-location; request MI210 time.
5. **Measure + publish** — per-CB latency decomposed into CXL-stage vs kernel; compare to the 23.4×
   CPU baseline and the Six-Times GPU envelope; report the CXL staging overhead as the core result.

---

## 11. References (verified)

1. Cammerer, Marcus, Zirr, Aït Aoudia, Maggi, Hoydis, Keller, **"Sionna Research Kit: A
   GPU-Accelerated Research Platform for AI-RAN,"** arXiv:2505.15848 (IEEE ICMLCN 2025 demo).
   Fork base (Apache 2.0). https://arxiv.org/abs/2505.15848 —
   repo: https://github.com/NVlabs/sionna-rk ·
   LDPC tutorial: https://nvlabs.github.io/sionna/rk/tutorials/ldpc_cuda/index.html ·
   runs on discrete x86 + RTX 4090: https://github.com/NVlabs/sionna/discussions/1094
2. **"Six Times to Spare: Characterizing GPU-Accelerated 5G LDPC Decoding for Edge-RSU
   Communications,"** arXiv:2602.04652 (2026). GPU LDPC within 6–24% of slot, +10–15 W. Motivation.
   https://arxiv.org/abs/2602.04652
3. **CCCL: "Node-Spanning GPU Collectives with CXL Memory Pooling,"** arXiv:2602.22457. Demonstrates
   GPU DMA over PCIe to CXL via `cudaHostRegister` (~20 GB/s; CPU-side CXL 658 ns). Establishes the
   GPU-over-CXL mechanism *and* is nearest prior art. https://arxiv.org/html/2602.22457v1
4. **DecodeX: "Exploring and Benchmarking LDPC Decoding across CPU, GPU, and ASIC Platforms,"**
   arXiv:2511.02952. LDPC across accelerators. https://arxiv.org/html/2511.02952v1
5. Li et al., **"Pond: CXL-Based Memory Pooling Systems for Cloud Platforms,"** ASPLOS'23,
   arXiv:2203.00241. CXL emulation methodology / latency. https://arxiv.org/abs/2203.00241
6. Yang et al., **"CXLMemSim: A Pure-Software Simulated CXL.mem for Performance Characterization,"**
   arXiv:2303.06153. Latency injection tool. https://arxiv.org/abs/2303.06153
7. **"InterfO-RAN: Real-Time In-band Cellular Uplink Interference Detection with GPU-Accelerated
   dApps,"** arXiv:2507.23177. Adjacent GPU/O-RAN work. https://arxiv.org/pdf/2507.23177
8. **eGPU: Transparent GPU Offload via eBPF,** ACM HCDS Workshop 2025. eBPF + OpenCL + CXL
   (this work delivers a RAN instance of that direction, via the OAI plugin instead of eBPF).
9. **AMD ROCm Documentation** — GPU memory, `hipHostRegister`, HIP programming guide.
   https://rocm.docs.amd.com/en/docs-6.2.2/conceptual/gpu-memory.html
10. **3GPP TS 38.212**, "NR; Multiplexing and channel coding." LDPC base graphs; C=24 derivation.
11. **NVlabs/sionna-rk** (Apache 2.0) — fork base. https://github.com/NVlabs/sionna-rk
12. **OpenAirInterface** — upstream for the plugin MR.
    https://gitlab.eurecom.fr/oai/openairinterface5g
