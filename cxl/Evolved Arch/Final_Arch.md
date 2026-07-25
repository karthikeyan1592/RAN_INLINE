# Open CXL-Backed GPU L1 Acceleration for 5G NR RAN
## Porting Sionna Research Kit from NVLink/CUDA to CXL/ROCm

**Author:** Karthi (Nokia BSP Engineer, Bangalore)
**Status:** Research Proposal
**Target:** Prof. Arkaprava Basu, IISc Computer Systems Lab

---

## 1. Problem Statement

NVIDIA's Sionna Research Kit (arXiv:2505.15848) proves that GPU acceleration
of 5G NR L1 LDPC decode is feasible in real-time, achieving 6× speedup over
CPU and bringing per-slot latency within the 500 µs 5G NR deadline.

However, Sionna-RK has two hard dependencies that prevent deployment on
commodity infrastructure:

**Dependency 1 — NVLink-C2C:**
The memory handoff between the OAI L1 pipeline (running on Grace CPU) and the
LDPC decode kernel (running on Blackwell GPU) relies on NVLink-C2C — a
proprietary chip-to-chip interconnect available only on NVIDIA DGX Spark and
Jetson AGX Orin. It provides coherent shared memory between CPU and GPU at
~273 GB/s with nanosecond latency. No equivalent exists on commodity PCIe
servers without NVIDIA hardware.

**Dependency 2 — CUDA:**
The LDPC decode kernel (`plugins/ldpc_cuda/src/runtime/ldpc_decoder.cu`) is
written in CUDA. It requires NVIDIA GPU hardware and the NVIDIA CUDA runtime.
It cannot run on AMD, Intel, or any non-NVIDIA GPU.

The result: Sionna-RK's GPU L1 acceleration is architecturally sound but
**hardware-vendor-locked end to end**. No open alternative exists.

---

## 2. Proposed Solution

### Core Idea

Replace Sionna-RK's two NVIDIA-specific components with open equivalents:

| Sionna-RK (NVIDIA) | This Work (Open) |
|---|---|
| NVLink-C2C unified memory | CXL Type-3 shared memory |
| CUDA LDPC kernel | ROCm/HIP LDPC kernel (hipify port) |
| Grace CPU + Blackwell GPU | Any x86/ARM CPU + any AMD GPU |
| DGX Spark / Jetson Orin | Commodity PCIe server + CXL module |

Everything else in Sionna-RK stays identical:
- OAI as the RAN stack (unmodified)
- OAI plugin API as the integration mechanism
- The same LDPC algorithm (min-sum, BG1/BG2)
- The same rfsim simulation mode for testing

### What Changes and What Does Not

```
SIONNA-RK (existing):                    THIS WORK (proposed):

OAI gNB (Grace CPU)                      OAI gNB (any CPU)
  runs L1 pipeline                          runs L1 pipeline
  FFT, ch.est, eq, LLR                      FFT, ch.est, eq, LLR
        │                                          │
        │ OAI plugin API                           │ OAI plugin API
        │ (no source change)                       │ (no source change)
        ▼                                          ▼
  NVLink-C2C          ←── REPLACE ──→      CXL Type-3 shared memory
  (proprietary,                            (open standard, PCIe-based,
   NVIDIA-only,                             any vendor, NUMA node on host)
   coherent)                                      │
        │                                          │
        ▼                                          ▼
  Blackwell GPU       ←── REPLACE ──→      AMD GPU (MI210 or similar)
  CUDA LDPC kernel                         ROCm/HIP LDPC kernel
  (ldpc_decoder.cu)                        (hipify port of .cu file)
        │                                          │
        ▼                                          ▼
  result back via NVLink                   result back via CXL
  OAI continues                            OAI continues
```

---

## 3. How CXL Replaces NVLink-C2C

### What NVLink-C2C Does

On DGX Spark, Grace CPU and Blackwell GPU share the same 128 GB physical
memory pool via NVLink-C2C. When OAI's plugin hands the LLR buffer pointer
to the CUDA kernel, both sides address the same physical pages — no DMA
transfer, no copy, coherent at cache-line granularity. Latency is ~nanoseconds.

### What CXL Provides Instead

CXL Type-3 memory is exposed as a CPU-less NUMA node (system-ram mode via
daxctl). Both the CPU and the GPU can access the same physical CXL pages:

- **CPU side:** normal load/store instructions to NUMA node 1 address range.
  OAI's L1 pipeline allocates its LLR buffers on the CXL node via
  `numa_alloc_onnode(size, cxl_node)`. No copy needed — LLR is born in CXL.

- **GPU side:** `hipHostRegister(cxl_ptr, size, hipHostRegisterDefault)`
  registers the CXL-backed pages as GPU-accessible host memory. The GPU DMA
  engine reads directly from CXL over PCIe. No copy to GPU VRAM needed.

The OAI plugin function receives an LLR pointer that already points into the
CXL region. It passes this directly to the ROCm kernel. The kernel reads from
CXL, decodes, writes result back to CXL. OAI reads the result. One region,
shared by both sides, no intermediate copies.

### Latency Comparison

| Memory fabric | CPU→GPU handoff | Coherency | Vendor |
|---|---|---|---|
| NVLink-C2C | ~nanoseconds, zero copy | Hardware coherent | NVIDIA only |
| CXL Type-3 + hipHostRegister | ~200-400 ns (real HW) | Non-coherent (streaming fine) | Any vendor |
| PCIe clEnqueueWriteBuffer | ~5-10 µs (DMA copy) | N/A | Any vendor |

CXL is slower than NVLink-C2C but orders of magnitude faster than a
conventional PCIe DMA copy. For LDPC decode latency in the context of the
500 µs slot budget, 200-400 ns CXL overhead is negligible.

---

## 4. Implementation Plan

### Step 1 — Fork Sionna-RK (Week 1)

Fork `NVlabs/sionna-rk` (Apache 2.0 license, fork permitted):

```
github.com/your-org/sionna-rk-rocm
```

Identify all files that reference CUDA, NVLink, or NVIDIA-specific APIs.
The primary targets are in `plugins/ldpc_cuda/`:

```
plugins/ldpc_cuda/
  src/runtime/ldpc_decoder.cu      ← CUDA kernel, port to HIP
  src/runtime/memory_manager.cu    ← CUDA memory management, replace with CXL
  include/ldpc_plugin.h            ← plugin API, keep unchanged
  CMakeLists.txt                   ← replace CUDA with ROCm cmake
```

### Step 2 — Port CUDA Kernel to HIP (Week 1-2)

Use `hipify-clang` for mechanical translation:

```bash
hipify-clang plugins/ldpc_cuda/src/runtime/ldpc_decoder.cu \
  -o plugins/ldpc_rocm/src/runtime/ldpc_decoder.hip
```

This handles ~90% of the translation automatically:
- `cudaMalloc` → `hipMalloc`
- `cudaMemcpy` → `hipMemcpy`
- `__global__` kernels → unchanged (HIP uses same syntax)
- `cudaStream_t` → `hipStream_t`

Manual review needed for: warp-level intrinsics (shuffle, ballot), shared
memory bank conflict patterns tuned for Ampere/Hopper, any Blackwell-specific
optimizations. These require re-tuning for CDNA3 (MI210) architecture —
different warp size (64 on AMD vs 32 on NVIDIA), different shared memory
organization.

### Step 3 — Replace Memory Manager with CXL Layer (Week 2-3)

Replace `memory_manager.cu` (which allocates CUDA unified memory relying on
NVLink-C2C) with a CXL-aware allocator:

```
memory_manager_cxl.c:
  cxl_init():
    detect CXL NUMA node (from /sys/bus/cxl/devices/)
    allocate LLR buffer pool on CXL node via numa_alloc_onnode()
    hipHostRegister(cxl_pool, size) → makes GPU-accessible
    return cxl_pool base pointer

  cxl_get_llr_buffer(cb_index):
    return cxl_pool + (cb_index * CB_BUFFER_SIZE)
    → OAI writes LLR here, GPU reads from here, same pointer

  cxl_fini():
    hipHostUnregister(cxl_pool)
    numa_free(cxl_pool, size)
```

The OAI plugin API function receives this CXL pointer as the LLR buffer.
It passes it unchanged to the HIP LDPC kernel. The kernel reads from CXL
directly (GPU DMA over PCIe to CXL physical address range).

### Step 4 — Test with OAI rfsim (Week 3-4)

OAI rfsim generates synthetic IQ samples without real radio hardware.
This is the same test mode Sionna-RK uses for validation.

Test sequence:
1. Launch OAI gNB with rfsim mode
2. Load the ROCm LDPC plugin (replaces the CUDA plugin)
3. OAI allocates LLR buffers from CXL pool (via plugin init)
4. OAI runs L1 pipeline: FFT, ch.est, equalization on CPU
5. At LDPC decode: plugin API calls HIP kernel on MI210
6. GPU reads LLR from CXL, decodes, writes bits back to CXL
7. OAI reads result, continues with CRC/HARQ/MAC

Verify: bit-exact decode (compare against OAI's software decoder output).
Measure: per-CB and per-slot latency through the full plugin path.

### Step 5 — Measure and Compare (Week 4-5)

Key measurements:

| Metric | CPU baseline | This work (CXL+MI210) | Sionna-RK (NVLink+Blackwell) |
|---|---|---|---|
| Per-CB LDPC latency | ~488 µs (I=20, AVX2) | measure | ~30-120 µs (cited) |
| Per-slot latency | 11,703 µs (23.4× over budget) | measure | within 500 µs |
| CXL handoff overhead | N/A | measure | N/A (NVLink) |
| Memory copies | 0 (no offload) | 0 (CXL zero-copy) | 0 (NVLink zero-copy) |

---

## 5. Why This Is Novel

### Prior Art Table

| Work | GPU | CXL | RAN | Open GPU runtime |
|---|---|---|---|---|
| NVIDIA Aerial cuPHY | CUDA | ✗ | 5G NR | ✗ |
| Sionna-RK (2505.15848) | CUDA | ✗ | 5G NR OAI | ✗ |
| AtlasRAN (2603.14661) | CUDA | ✗ | OAI | ✗ |
| Six Times to Spare (2602.04652) | CUDA | ✗ | OAI | ✗ |
| eGPU (HCDS'25) | OpenCL | CXL (future) | ML only | ✓ |
| **This work** | **ROCm/HIP** | **CXL Type-3** | **5G NR OAI** | **✓** |

### The Specific Gap

Every existing GPU-accelerated 5G NR L1 paper uses CUDA and NVIDIA hardware.
The Sionna-RK authors explicitly target Jetson and DGX Spark.
No work has demonstrated GPU L1 acceleration on a non-NVIDIA GPU
with a non-NVLink memory fabric.

### Research Question

> "Can CXL Type-3 shared memory replace NVLink-C2C as the CPU-GPU memory
> fabric for real-time 5G NR L1 LDPC offload, enabling deployment on
> commodity x86 servers with AMD GPUs without hardware vendor lock-in?"

### The Honest Limitation

NVLink-C2C provides hardware cache coherency. CXL Type-3 does not —
it is a streaming memory path. For LDPC offload (one-directional data flow:
CPU writes LLR once, GPU reads once, GPU writes result once, CPU reads once)
coherency is not required. This makes CXL Type-3 sufficient for this specific
workload, even though it is architecturally weaker than NVLink-C2C.

---

## 6. CXL 3.0 Scalability Extension (Future Work)

With CXL 3.0 fabric (multi-host pooling, hardware available ~2026-2027):

```
CPU node 1 (OAI vDU instance 1) ─┐
CPU node 2 (OAI vDU instance 2) ─┤── CXL 3.0 pooled memory ── one GPU
CPU node 3 (OAI vDU instance 3) ─┤
CPU node N (OAI vDU instance N) ─┘
```

Sionna-RK / Aerial require one GPU per gNB instance (NVLink is per-chip).
CXL 3.0 pooling enables one GPU to serve N gNB instances sharing one memory
pool. This is the scalability argument for CXL that NVLink cannot replicate.

---

## 7. Hardware Requirements

### Simulation (today)

- Any x86 server with QEMU CXL emulation
- PoCL as OpenCL CPU stand-in (proves integration layer, not latency)
- No real GPU needed for functional validation

### Full Validation (IISc HACC)

| Component | What | Why |
|---|---|---|
| CPU | Sapphire Rapids or Genoa | CXL 2.0 root port needed |
| CXL module | Samsung or Micron Type-3 DRAM | Real CXL latency (200-400 ns) |
| GPU | AMD MI210 (ROCm 5.x+) | hipHostRegister, HIP kernel |
| OAI | Any recent version with plugin API | The RAN stack |

**Key question for Prof. Basu:** Does IISc HACC have a CXL-capable CPU
and a CXL Type-3 DRAM module on the same server as the MI210?
If yes, all four components exist for full validation.

---

## 8. Relationship to Ongoing PoC Work

The current PoC (v4-v7, GCP + QEMU) proves several components relevant
to this proposal:

| PoC Result | Relevance to This Proposal |
|---|---|
| Bit-exact OpenCL LDPC kernel (0 mismatches, BG1/BG2 Z=384/256) | Confirms the algorithm ports correctly to OpenCL/ROCm |
| CXL zero-copy via CL_MEM_USE_HOST_PTR (sentinel test passed) | Confirms OpenCL can read from CXL-backed memory |
| 23.4× CPU baseline (11,703 µs/slot, fixed anchor) | Quantifies the gap that GPU closes |
| QEMU CXL Type-3 kernel path (5/5 verify checks) | Confirms Linux CXL driver stack works |

The PoC was built with a different integration mechanism (bpftime uprobe,
no OAI source modification). This proposal replaces that with the cleaner
OAI plugin API that Sionna-RK already defines — sanctioned hooks, simpler
than uprobe interception, same data path.

---

## 9. Contribution Plan

### Code Contribution

Fork `NVlabs/sionna-rk` → `open-cxl-ran/sionna-rk-rocm` (Apache 2.0)

Deliverables:
- `plugins/ldpc_rocm/` — ROCm/HIP port of the CUDA LDPC plugin
- `plugins/ldpc_rocm/memory_manager_cxl.c` — CXL allocator replacing NVLink
- `cmake/FindROCm.cmake` — build system for ROCm
- Tutorial: "GPU-Accelerated LDPC Decoding on AMD GPU via CXL"

Upstream contribution: MR to OAI GitLab adding the ROCm LDPC plugin
as an alternative to the CUDA plugin. This closes the "CUDA-only" gap
in OAI's accelerator ecosystem.

### Publication

**Primary:** IEEE Networking Letters or IEEE Communications Letters (4 pages)
Title: "CXL-Backed GPU Acceleration of 5G NR LDPC Decode: An Open Alternative
to NVLink-C2C"

**Secondary:** O-RAN Alliance WG6 (Acceleration Abstraction Layer) technical
contribution — Nokia is a WG6 member. Demonstrates open AAL implementation.

---

## 10. Next Steps

1. **Study Sionna-RK plugin API** — read `plugins/ldpc_cuda/` source,
   understand `oai_nrLDPCdecoder_t` function pointer interface. This defines
   exactly what the ROCm plugin must implement.

2. **Fork and hipify** — fork `NVlabs/sionna-rk`, run `hipify-clang` on
   `ldpc_decoder.cu`, assess manual porting effort for AMD CDNA3 architecture.

3. **IISc meeting** — present this proposal, ask about CXL module availability,
   request MI210 time for kernel benchmarking.

4. **CXL memory manager** — write `memory_manager_cxl.c`, test with QEMU
   CXL emulation + PoCL first, then real hardware.

5. **Measure and publish** — per-CB latency through CXL + MI210, comparison
   to CPU baseline (23.4×) and Sionna-RK projected numbers (6× speedup).

---

## 11. References

1. Cammerer et al., "Sionna Research Kit: A GPU-Accelerated Research
   Platform for AI-RAN," arXiv:2505.15848, May 2025. ← primary prior art,
   fork base (Apache 2.0)

2. "Six Times to Spare: LDPC Acceleration on DGX Spark for AI-Native Open
   RAN," arXiv:2602.04652, Feb 2026. ← 6× GPU speedup, motivation

3. Li et al., "Pond: CXL-Based Memory Pooling Systems for Cloud Platforms,"
   ASPLOS'23, arXiv:2203.00241. ← CXL emulation methodology, latency numbers

4. Yang et al., "CXLMemSim: A Pure Software Simulated CXL.mem for Performance
   Characterization," arXiv:2303.06153. ← latency injection tool

5. "eGPU: Transparent GPU Offload via eBPF," ACM HCDS Workshop 2025.
   ← eBPF + OpenCL + CXL future work (this work delivers that future work)

6. 3GPP TS 38.212: "NR; Multiplexing and channel coding."
   ← LDPC base graphs, C=24 derivation

7. AMD ROCm Documentation: hipHostRegister, HIP programming guide.
   ← GPU-accessible host memory mechanism

8. NVlabs/sionna-rk: https://github.com/NVlabs/sionna-rk (Apache 2.0)
   ← fork base

9. OpenAirInterface: https://gitlab.eurecom.fr/oai/openairinterface5g
   ← upstream for plugin MR contribution