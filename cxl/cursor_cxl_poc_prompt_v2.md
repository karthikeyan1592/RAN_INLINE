# CXL + GPU + L1 RAN Offload PoC — Cursor Agent Prompt

## ROLE
You are an expert Linux kernel engineer and systems programmer. You will build a complete, working, paper-ready proof-of-concept demonstrating open AI-RAN L1 workload offload using Linux CXL emulation, eBPF interception, and a GPU accelerator daemon. Work autonomously in a loop. When something fails, debug and fix it. Never stop and ask — try, fail, fix, retry.

## PROJECT GOAL
Demonstrate that 5G NR L1 compute workloads (LDPC decode, FFT) can be transparently offloaded from CPU to a GPU/accelerator through:
1. eBPF uprobe interception at the L1 function boundary
2. CXL emulated memory as shared zero-copy buffer
3. A GPU accelerator daemon (CPU-based simulation of GPU compute)
4. Linux HMM (Heterogeneous Memory Management) for memory coherence

This is the open alternative to NVIDIA Aerial + CUDA — vendor-neutral, kernel-native, no proprietary runtime.

## RESEARCH FINDINGS — READ BEFORE BUILDING ANYTHING

This section contains results from a comprehensive feasibility study. These are ground truth constraints. Every decision in this build must respect them.

### Finding 1 — QEMU CXL: Functional Only, NOT Timing Accurate

QEMU CXL Type-3 is production-ready and used by Linux kernel CXL developers themselves. It correctly exercises `drivers/cxl/`, `cxl list`, `daxctl`, and kernel memory hotplug. Use it.

**CRITICAL CONSTRAINT:** QEMU does NOT model CXL latency or coherency protocol. The QEMU docs explicitly state cache operations are "mostly irrelevant to QEMU emulation as QEMU is not emulating a coherency protocol." This means:
- All latency numbers from QEMU are meaningless as CXL latency — they reflect QEMU host memory
- CXL Type-2 (coherent accelerator with CXL.cache) does NOT exist in mainline QEMU — only RFC patches
- The PoC architecture claim is valid; the performance claim requires NUMA latency modeling

**Action:** Frame all results as "functional architecture validation." For any latency sensitivity claims, layer NUMA emulation (Pond-style: CPU-less NUMA node) to inject realistic CXL latency (~140–255 ns range based on Pond's measurements: 142 ns remote, up to 255 ns in lab servers). Real CXL.mem load-to-use is ~150–175 ns.

**QEMU version:** Use 9.x or 10.x. CXL Type-3 volatile support merged in QEMU 8.1. Best stability is 9.x+.

**Kernel version:** Use 6.6+. Must enable: CONFIG_CXL_BUS, CONFIG_CXL_MEM, CONFIG_CXL_PORT, CONFIG_CXL_ACPI, CONFIG_DEV_DAX, CONFIG_ZONE_DEVICE, CONFIG_MEMORY_HOTPLUG, CONFIG_BPF_SYSCALL, CONFIG_UPROBE_EVENTS, CONFIG_BPF_MAP_TYPE_RINGBUF (needs kernel 5.8+), BPF_MAP_TYPE_USER_RINGBUF (needs kernel 6.1+).

### Finding 2 — GPU Simulation: Use CPU OpenCL Daemon, Not GPU Simulator

Full GPU simulators (GPGPU-Sim, MGPUSim) are too slow to run alongside a RAN stack. AMD virtio-gpu DRM native context was upstreamed in QEMU (May-June 2025, v12/v13) but requires a REAL host GPU — it is passthrough, not simulation. Do NOT attempt to run ROCm inside QEMU without a physical GPU on host.

**Recommended approach (no GPU hardware required):**
Use **PoCL (Portable Computing Language)** or **Intel oneAPI CPU runtime** to run OpenCL/SYCL kernels on the CPU, presenting them as "accelerator" compute. This is honest and practical:
- PoCL v7.x: CPU as OpenCL device, no GPU needed
- Intel oneAPI: `ONEAPI_DEVICE_SELECTOR=opencl:cpu` with TBB backend

The GPU daemon simulates the accelerator data path (CXL shared-memory in → compute → CXL shared-memory out) while running compute on CPU. This proves the ARCHITECTURE. Label it clearly: "CPU-based accelerator daemon simulating GPU offload path."

**For paper GPU performance projections:** Cite the "Six Times to Spare" (arXiv:2602.04652, Feb 2026) paper which measured real GPU LDPC on DGX Spark: ~6× speedup over CPU, per-codeword latency well within the 0.5 ms slot vs CPU decode reaching ~0.71 ms at 20 iterations. Use these numbers as projected speedup with real GPU hardware.

### Finding 3 — L1 RAN Workload: Use srsRAN Standalone Benchmarks First

**Best interception target:** srsRAN Project standalone benchmarks:
- `ldpc_decoder_benchmark` — exercises BG1/BG2 LDPC directly, no RAN stack
- `pusch_processor_benchmark -m latency` — full RX chain including LDPC

These are the cleanest uprobe targets. Build and validate against these before integrating full OAI softmodem.

**Latency budget (CRITICAL for architecture design):**
- 5G NR slot = 0.5 ms (µ=1, 30 kHz SCS)
- srsRAN per-TB latency: ~16.6 µs (small) to ~427–450 µs (large, 270 RB, 16QAM)
- CPU LDPC decode: ~0.71 ms at 20 iterations for large TB — EXCEEDS slot budget
- This is the entire motivation for GPU offload

**OAI RFSIM:** Works inside VMs (OAI's own CI uses `runTestOnVM.sh`). Use `--rfsim --phy-test` for non-real-time functional testing. LDPC decoder is a hot-swappable shared library — the natural uprobe point.

### Finding 4 — eBPF Overhead: ~1670 ns Per Uprobe Intercept

**Measured overhead:** 1670–1833 ns per uprobe-based eBPF intercept (Cloudflare ebpf_exporter benchmark, Linux 6.7-rc3, BenchmarkUprobeWithSimpleMap).

**Architectural consequence (CRITICAL):**
- Intercept at TRANSPORT BLOCK granularity (1–4 per slot) — overhead ~1.7–6.8 µs per slot ✅
- Do NOT intercept per-iteration or per-bit — would blow 0.5 ms budget
- Each slot has 14 OFDM symbols → 14 FFTs → intercept once at slot/TB level, not per-symbol

**Ring buffer:** Use BPF_MAP_TYPE_RINGBUF (kernel 5.8+) for kernel→userspace events. Use BPF_MAP_TYPE_USER_RINGBUF (kernel 6.1+) for userspace→kernel control. Epoll-based wakeup is sub-millisecond. Ring buffer handles slot-rate signaling comfortably.

**eBPF uprobes inside QEMU/KVM:** Confirmed working (Cloudflare ran the benchmark under QEMU on M1).

### Finding 5 — Prior Art: Complete Combination Map

**Every two-way combination has been done. The four-way has not.**

```
Combination                          Done?  Paper / Where
────────────────────────────────────────────────────────────────────
CXL + GPU (memory tiering)           ✅     NeoMem (MICRO'24)
                                            CCCL (ICS'25)
                                            LLM KV cache papers

CXL + GPU (accelerator offload)      ✅     UDON (Arm, arXiv:2404.02868)
                                            Offloading to CXL Comp Mem
                                            (arXiv:2512.04449)

eBPF + GPU (instrumentation)         ✅     eGPU (ACM HCDS'25)
                                            Auto-instrumentation GPU
                                            via eBPF (FOSDEM'25)

eBPF + CXL (profiling)              ✅     CXLAimPod (arXiv:2508.15980)
                                            Uses uprobes for CXL bandwidth
                                            measurement

GPU + L1 RAN (with CUDA/NVIDIA)      ✅     AtlasRAN (arXiv:2603.14661)
                                            Six Times to Spare
                                            (arXiv:2602.04652)
                                            NVIDIA Aerial SDK

CXL + L1 RAN                         ❌     NOBODY HAS DONE THIS
eBPF + L1 RAN                         ❌     NOBODY HAS DONE THIS
CXL + GPU + L1 RAN                    ❌     NOBODY HAS DONE THIS
CXL + GPU + L1 RAN + eBPF            ❌     YOUR WORK — unpublished
```

**The eGPU paper explicitly says your work is future work:**
eGPU (ACM HCDS Workshop 2025) explicitly says in their conclusion:
"Going forward, we're looking at broader device contexts and
memory-sharing methods — especially on upcoming Grace Hopper GPUs
and with CXL memory pools — to expand how and where this approach
can be applied."
→ They said "CXL memory pools" is future work. You are building that future work.
→ With 5G NR L1 timing constraint (0.5 ms) they never studied.

**Papers differentiated precisely:**

1. **AtlasRAN** (arXiv:2603.14661, Feb 2026):
   RAN + GPU offload — OAI RFSim + Sionna on DGX Spark.
   **Their approach:** Modified OAI to call CUDA GPU directly.
   **Your approach:** Unmodified L1 application — eBPF intercepts transparently.
   **Key difference:** Zero-change deployment (critical for operators).

2. **eGPU** (ACM HCDS Workshop 2025):
   eBPF + GPU + future CXL memory pools.
   **Their approach:** bpftime (userspace eBPF) instruments GPU-side kernels.
   **Your approach:** Kernel uprobes intercept CPU-side L1 application.
   **Key difference:** CPU application is unmodified; their GPU app is instrumented.
   **Code implication:** Use kernel uprobe path, NOT bpftime — simpler, no GPU hooks needed.

3. **CXLAimPod** (arXiv:2508.15980, 2024):
   eBPF + CXL — uses uprobes for CXL bandwidth profiling.
   **Their approach:** eBPF as an observation tool for CXL memory.
   **Your approach:** eBPF as an interception and offload routing mechanism.
   **Key difference:** They measure, you route compute.

4. **UDON** (arXiv:2404.02868, Arm):
   NUMA-emulated CXL Type-2, near-memory CPU offload.
   **Their approach:** CPU cores near CXL memory, no GPU, no eBPF.
   **Your approach:** GPU accelerator + eBPF transparent interception.
   **Key difference:** GPU accelerator vs near-memory CPU; eBPF transparency.

5. **"Six Times to Spare"** (arXiv:2602.04652, Feb 2026):
   GPU LDPC acceleration for Open RAN.
   **Their approach:** CUDA kernels directly called from RAN application, NVIDIA DGX hardware.
   **Your approach:** Any GPU via open stack, eBPF transparent, no CUDA.
   **Key difference:** Vendor lock-in vs vendor neutral; transparent vs explicit.
   **CODE NOTE:** Their CPU baseline ~0.71 ms per large TB = your calibration target.

**Your precise novel contribution:**
"Transparent eBPF-uprobe interception of an UNMODIFIED 5G NR L1
application (no source changes required), routing LDPC/FFT compute
through CXL Type-3 shared memory to an open accelerator daemon,
analyzed within the 0.5 ms 5G NR slot timing constraint,
using only Linux kernel primitives — no CUDA, no proprietary runtime,
no application modification."

**Why the 0.5 ms constraint makes this uniquely interesting:**
- General compute offload: budget = seconds to minutes
- LLM inference: budget = tens of milliseconds
- 5G NR L1 slot: budget = 0.5 MILLISECONDS
Nobody has asked: can CXL + eBPF offload work within a 0.5 ms hard deadline?
The answer is non-obvious. That's a research question worth answering.

### Finding 6 — Paper Validation Standard

NUMA-based CXL emulation is accepted at ASPLOS, USENIX ATC, OSDI, EuroSys, HPCA. **Pond (ASPLOS '23, Microsoft/VT/Intel), TPP (ASPLOS '23, Meta), emucxl (IIT Bombay), HybridTier** all used CPU-less NUMA socket emulation. This is your precedent.

**Reviewer expectations:**
1. Explicitly state QEMU does not model CXL timing/coherency
2. Add NUMA-emulated latency sensitivity analysis (~140–255 ns range)
3. Distinguish functional correctness claims from performance claims
4. State hardware requirements for real deployment (CXL Type-3 module ~$1500, Type-2 AMD MI300X ~$15-20K)

**Minimum viable paper result set:**
- Functional: correct LDPC output through offload path (bit-for-bit match)
- Overhead: measured eBPF interception cost (~1.7 µs per call, confirmed)
- Latency breakdown: eBPF intercept + IPC + compute + CXL shared-mem write
- Sensitivity: NUMA-latency sweep (50 ns, 142 ns, 255 ns) showing effect on slot deadline
- Comparison: CPU-only baseline vs offload path
- Projection: expected speedup with real GPU (cite Six Times to Spare ~6×)

### Finding 7 — Hardware Requirements

**Minimum for full stack:** 32 GB RAM, 8+ physical CPU cores, workstation (not laptop).
- QEMU VM: 8 GB RAM minimum for kernel + CXL + OAI
- CXL backing file: 2 GB
- srsRAN build: needs 16 GB RAM during compilation

**Laptop feasibility:** Reduced functional demo only (4 GB VM, synthetic LDPC microbenchmark, no full OAI).

**For NUMA latency emulation (Pond-style):** Dual-socket server ideal. Single-socket: use QEMU with multiple NUMA nodes and artificial latency injection, or use CXLMemSim.

**Recommended CXL latency tool:** CXLMemSim (UCSC, arXiv:2303.06153) — pure-software latency injector, no hardware needed, orders of magnitude faster than gem5, runs on standard x86.

### IMPLEMENTATION CHANGES FROM RESEARCH

Based on research, these specific changes apply to the build described below:

1. **CXL shared memory:** Use `share=on` file-backed QEMU Type-3 device surfaced as NUMA node via `cxl create-region` + `daxctl reconfigure-device -m system-ram`. This is the established path that exercises real `drivers/cxl/`.

2. **GPU daemon:** Use PoCL OpenCL CPU runtime instead of bare C. Install: `sudo apt install pocl-opencl-icd`. Run LDPC/FFT as OpenCL kernels on CPU — same API as real GPU, honest about being CPU.

3. **L1 workload:** Build srsRAN standalone benchmarks FIRST (`ldpc_decoder_benchmark`, `pusch_processor_benchmark`). Integrate OAI softmodem SECOND. This de-risks the build.

4. **eBPF granularity:** Intercept at TRANSPORT BLOCK entry (once per TTI for LDPC, once per slot for FFT batch), NOT per code block or per symbol. Justification: ~1.7 µs overhead per intercept vs 0.5 ms slot budget.

5. **Latency claims:** Add a NUMA latency sweep script that uses `numactl --cpunodebind=0 --membind=1` to force memory allocation to the CXL-emulating NUMA node. Run baseline vs offload at NUMA latency 0 (local), 142 ns (Pond CXL), 255 ns (Pond max). This is your latency sensitivity analysis for the paper.

6. **Results framing:** Every result CSV must include a column `emulation_mode` with values: `qemu-cxl-type3`, `numa-emulated-cxl-142ns`, `numa-emulated-cxl-255ns`. Never mix emulation modes in the same figure without labeling.

## TARGET ENVIRONMENT
- Host OS: Ubuntu 22.04 LTS or later
- QEMU version: **9.x or 10.x** (CXL Type-3 volatile, most stable)
- Linux kernel: **6.6 or later** (BPF_MAP_TYPE_USER_RINGBUF needs 6.1+, CXL stable at 6.6+)
- Languages: C (kernel/eBPF/userspace), OpenCL C (GPU daemon kernels), Python (measurement/plotting)
- Build system: Make + CMake
- Root access required inside QEMU VM
- **PoCL installed on host:** `sudo apt install pocl-opencl-icd clinfo` (verify: `clinfo | grep CPU`)
- **CXLMemSim installed (optional):** for latency injection — clone from github.com/SlugLab/CXLMemSim

## ARCHITECTURE OVERVIEW

```
┌─────────────────────────────────────────────────────────┐
│                    QEMU Virtual Machine                   │
│                                                           │
│  ┌─────────────────────────────────────────────────┐    │
│  │           L1 RAN Simulator (ran_l1_sim)          │    │
│  │  - ldpc_decode(input, output, len)               │    │
│  │  - fft_process(samples, len)                     │    │
│  │  - Generates synthetic 5G NR slot data           │    │
│  │  - Measures end-to-end latency                   │    │
│  └──────────────────┬──────────────────────────────┘    │
│                     │ function calls                      │
│  ┌──────────────────▼──────────────────────────────┐    │
│  │           eBPF Interception Layer                 │    │
│  │  - uprobe on ldpc_decode()                       │    │
│  │  - uprobe on fft_process()                       │    │
│  │  - BPF ring buffer → signals GPU daemon          │    │
│  │  - BPF map: tracks offload state                 │    │
│  │  - Returns result from CXL memory to caller      │    │
│  └──────────────────┬──────────────────────────────┘    │
│                     │ BPF ring buffer event              │
│  ┌──────────────────▼──────────────────────────────┐    │
│  │           GPU Accelerator Daemon (gpu_daemon)    │    │
│  │  - Receives work from eBPF ring buffer           │    │
│  │  - Reads input from CXL memory region            │    │
│  │  - Computes LDPC/FFT (CPU simulating GPU)        │    │
│  │  - Writes result to CXL memory region            │    │
│  │  - Signals completion via BPF map                │    │
│  └──────────────────┬──────────────────────────────┘    │
│                     │ shared memory                       │
│  ┌──────────────────▼──────────────────────────────┐    │
│  │           CXL Memory Region (2GB emulated)       │    │
│  │  - /dev/dax0.0 or NUMA node from CXL device      │    │
│  │  - Zero-copy: both L1 and GPU daemon map it      │    │
│  │  - Managed by Linux drivers/cxl/ subsystem       │    │
│  │  - HMM tracks page mappings                      │    │
│  └─────────────────────────────────────────────────┘    │
│                                                           │
│  CXL Type-3 emulated device (QEMU built-in)             │
└─────────────────────────────────────────────────────────┘
```

## DIRECTORY STRUCTURE TO CREATE

```
cxl_ran_poc/
├── README.md
├── Makefile                    # Top-level build
├── scripts/
│   ├── setup_qemu.sh          # QEMU boot script with CXL
│   ├── build_kernel.sh        # Kernel build with right configs
│   ├── setup_cxl.sh           # CXL device setup inside VM
│   └── run_poc.sh             # Run complete experiment
├── kernel/
│   ├── kernel_config_fragment  # Required kernel config options
│   └── README.md
├── l1_sim/
│   ├── ldpc.c                  # LDPC encoder/decoder
│   ├── ldpc.h
│   ├── fft.c                   # FFT implementation
│   ├── fft.h
│   ├── ran_l1_sim.c            # Main L1 simulator
│   ├── ran_l1_sim.h
│   └── Makefile
├── ebpf/
│   ├── l1_intercept.bpf.c     # eBPF kernel program
│   ├── l1_intercept.h         # Shared structs (kernel + user)
│   ├── l1_intercept_loader.c  # Userspace eBPF loader
│   └── Makefile
├── gpu_daemon/
│   ├── gpu_daemon.c           # GPU accelerator daemon
│   ├── gpu_compute.c          # Compute functions (LDPC, FFT)
│   ├── gpu_compute.h
│   ├── cxl_memory.c           # CXL memory management
│   ├── cxl_memory.h
│   └── Makefile
├── measurement/
│   ├── measure.c              # Latency measurement harness
│   ├── plot_results.py        # Generate paper figures
│   └── Makefile
└── paper/
    ├── results/               # Generated CSV data
    ├── figures/               # Generated plots
    └── notes.md               # Key findings for paper
```

## PHASE 0: ENVIRONMENT SETUP

### Step 0.1: Verify host dependencies
Install on HOST machine:
```bash
sudo apt-get install -y \
    qemu-system-x86 \
    build-essential \
    flex bison \
    libelf-dev libssl-dev \
    bc libncurses-dev \
    clang llvm \
    libbpf-dev bpftool \
    linux-headers-$(uname -r) \
    python3-pip \
    git wget curl

pip3 install matplotlib numpy pandas scipy
```

Verify QEMU CXL support:
```bash
qemu-system-x86_64 -M q35,help 2>&1 | grep -i cxl
# Must show: cxl=on option available
```

### Step 0.2: QEMU Boot Script
Create `scripts/setup_qemu.sh`:
```bash
#!/bin/bash
# CXL-enabled QEMU VM boot script

KERNEL=${1:-"bzImage"}
INITRD=${2:-"initrd.img"}
MEM_PATH="/tmp/cxl_mem_file"

# Create CXL backing file
truncate -s 2G ${MEM_PATH}

qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 4 \
    -m 4G,slots=8,maxmem=16G \
    -M q35,accel=kvm,cxl=on \
    \
    -kernel ${KERNEL} \
    -initrd ${INITRD} \
    -append "console=ttyS0 root=/dev/sda rw nokaslr kasan_force_panic=on" \
    \
    -drive file=disk.img,if=virtio,format=qcow2 \
    \
    -object memory-backend-file,id=cxl-mem0,\
share=on,mem-path=${MEM_PATH},size=2G,align=256M \
    -device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52,uid=0,len-window-base=0,window-base[0]=0x4c00000000,window-size[0]=0x20000000 \
    -device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 \
    -device cxl-type3,bus=rp0,memdev=cxl-mem0,id=cxl-mem0,size=2G,num-LSA=1 \
    -M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G,cxl-fmw.0.interleave-ways=1 \
    \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    \
    -nographic \
    -serial mon:stdio
```

### Step 0.3: Kernel Configuration Fragment
Create `kernel/kernel_config_fragment`:
```
CONFIG_CXL_BUS=y
CONFIG_CXL_MEM=y
CONFIG_CXL_PORT=y
CONFIG_CXL_ACPI=y
CONFIG_CXL_PMEM=y
CONFIG_CXL_MEM_RAW_COMMANDS=y
CONFIG_DEV_DAX=y
CONFIG_DEV_DAX_PMEM=y
CONFIG_ZONE_DEVICE=y
CONFIG_MEMORY_HOTPLUG=y
CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE=y
CONFIG_NUMA=y
CONFIG_ACPI_HMAT=y
CONFIG_MEMORY_FAILURE=y
CONFIG_HMM_MIRROR=y
CONFIG_DEVICE_PRIVATE=y
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_UPROBE_EVENTS=y
CONFIG_UPROBES=y
CONFIG_PERF_EVENTS=y
CONFIG_KASAN=y
CONFIG_KASAN_GENERIC=y
CONFIG_DEBUG_KERNEL=y
CONFIG_DYNAMIC_DEBUG=y
CONFIG_CXL_DEBUG=y
```

## PHASE 1: L1 RAN SIMULATOR

### Step 1.1: LDPC Implementation
Create `l1_sim/ldpc.h`:
```c
#ifndef LDPC_H
#define LDPC_H

#include <stdint.h>
#include <stddef.h>

/* 5G NR LDPC parameters */
#define LDPC_MAX_CB_SIZE    8448    /* Maximum code block size bits */
#define LDPC_MAX_MSG_SIZE   8192    /* Maximum message size bits */
#define LDPC_LIFTING_SIZE   384     /* Maximum lifting size Z */

typedef struct {
    uint8_t *input;
    uint8_t *output;
    size_t   input_len;     /* bytes */
    size_t   output_len;    /* bytes */
    int      base_graph;    /* 1 or 2 */
    int      lifting_size;  /* Z value */
    float    code_rate;     /* 1/3, 1/2, 2/3, 3/4, etc */
} ldpc_params_t;

/* Initialize LDPC with 5G NR Base Graph 1 parameters */
int ldpc_init(void);

/* Encode: message → codeword */
int ldpc_encode(const ldpc_params_t *params);

/* Decode: received LLRs → decoded bits */
int ldpc_decode(const ldpc_params_t *params);

/* Generate synthetic received LLR samples (test data) */
void ldpc_gen_test_input(uint8_t *buf, size_t len, float snr_db);

/* Verify decoded output correctness */
int ldpc_verify(const uint8_t *encoded, const uint8_t *decoded, size_t len);

void ldpc_cleanup(void);

#endif /* LDPC_H */
```

Create `l1_sim/ldpc.c`:
Implement a simplified but functionally representative LDPC decoder:
- Use min-sum algorithm (standard 5G NR decoder)
- Implement for Base Graph 1, lifting size Z=384
- Input: soft LLR values (int8_t approximation)
- Output: decoded bits (uint8_t)
- Must be realistic compute load (not trivially fast)
- Target: ~100-500 microseconds on modern CPU for one code block
- IMPORTANT: The function signature MUST be:
  `int ldpc_decode(const ldpc_params_t *params)`
  This is the function eBPF will intercept.

### Step 1.2: FFT Implementation  
Create `l1_sim/fft.h`:
```c
#ifndef FFT_H
#define FFT_H

#include <complex.h>
#include <stddef.h>

typedef struct {
    float complex *input;
    float complex *output;
    size_t         N;          /* FFT size: 512, 1024, 2048, 4096 */
    int            direction;  /* 1=forward, -1=inverse */
    int            normalized; /* normalize output */
} fft_params_t;

int fft_init(void);
/* IMPORTANT: This signature is intercepted by eBPF */
int fft_process(const fft_params_t *params);
void fft_cleanup(void);

/* Generate OFDM test signal */
void fft_gen_ofdm_signal(float complex *buf, size_t N, int num_subcarriers);

#endif /* FFT_H */
```

Implement Cooley-Tukey radix-2 FFT in `l1_sim/fft.c`.
Target compute time: ~50-200 microseconds for 4096-point FFT.

### Step 1.3: Main L1 Simulator
Create `l1_sim/ran_l1_sim.c`:

The simulator must:
1. Allocate input/output buffers in CXL memory (passed as argument)
2. Generate synthetic 5G NR slot data (14 OFDM symbols, 106 RBs)
3. Run the following per slot (0.5ms budget):
   - FFT on each received OFDM symbol (14x FFT)
   - LDPC decode on each code block (~2-8 CBs per slot)
4. Measure and report:
   - CPU-only path latency (baseline)
   - Offload path latency (via eBPF → GPU daemon)
   - Slot success rate (met 0.5ms deadline?)
5. Write results to shared CXL memory for GPU daemon to process
6. Signal GPU daemon via a mechanism that eBPF sets up

Key functions that MUST exist with EXACT signatures:
```c
/* These are the eBPF probe points */
int ldpc_decode(const ldpc_params_t *params);
int fft_process(const fft_params_t *params);

/* CXL memory interface */
void *cxl_alloc(size_t size);
void  cxl_free(void *ptr);
int   cxl_map_region(const char *dev_path, size_t size);
```

L1 simulator main loop:
```c
void run_l1_simulation(int num_slots, int use_offload) {
    for (int slot = 0; slot < num_slots; slot++) {
        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        
        /* Step 1: Generate received samples */
        generate_rx_samples(rx_buf, slot);
        
        /* Step 2: FFT (intercepted by eBPF if offload enabled) */
        for (int sym = 0; sym < 14; sym++) {
            fft_process(&fft_params[sym]);
        }
        
        /* Step 3: Channel estimation, equalization */
        channel_estimate_and_equalize(rx_buf, eq_buf);
        
        /* Step 4: LDPC decode (intercepted by eBPF if offload enabled) */
        for (int cb = 0; cb < num_code_blocks; cb++) {
            ldpc_decode(&ldpc_params[cb]);
        }
        
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        
        /* Record latency */
        uint64_t latency_us = timespec_diff_us(&t_start, &t_end);
        record_latency(slot, latency_us, use_offload);
    }
}
```

## PHASE 2: CXL MEMORY SUBSYSTEM

### Step 2.1: CXL Memory Manager
Create `gpu_daemon/cxl_memory.h`:
```c
#ifndef CXL_MEMORY_H
#define CXL_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* CXL memory region layout */
#define CXL_REGION_TOTAL_SIZE    (2UL * 1024 * 1024 * 1024)  /* 2GB */
#define CXL_REGION_L1_INPUT_OFF  0x00000000   /* L1 writes input here */
#define CXL_REGION_L1_INPUT_SZ   (256 * 1024 * 1024)   /* 256MB */
#define CXL_REGION_GPU_OUTPUT_OFF 0x10000000  /* GPU writes result here */
#define CXL_REGION_GPU_OUTPUT_SZ  (256 * 1024 * 1024)  /* 256MB */
#define CXL_REGION_CTRL_OFF      0x20000000   /* Control/sync area */
#define CXL_REGION_CTRL_SZ       (1 * 1024 * 1024)     /* 1MB */

/* Control block in CXL memory — shared between L1 and GPU daemon */
typedef struct __attribute__((packed)) {
    volatile uint32_t l1_ready;      /* L1 wrote input, GPU should process */
    volatile uint32_t gpu_done;      /* GPU wrote output, L1 can read */
    volatile uint32_t work_type;     /* 1=LDPC, 2=FFT */
    volatile uint32_t input_offset;  /* offset from CXL base */
    volatile uint32_t input_len;     /* bytes */
    volatile uint32_t output_offset; /* offset from CXL base */
    volatile uint32_t output_len;    /* bytes */
    volatile uint64_t timestamp_l1;  /* when L1 submitted work */
    volatile uint64_t timestamp_gpu; /* when GPU completed */
    uint8_t           _pad[64];      /* cache line align */
} cxl_ctrl_block_t;

/* CXL memory context */
typedef struct {
    void    *base;          /* mmap'd CXL region base */
    size_t   size;          /* total mapped size */
    int      fd;            /* /dev/dax0.0 fd */
    int      numa_node;     /* NUMA node of CXL memory */
    cxl_ctrl_block_t *ctrl; /* pointer into ctrl region */
} cxl_ctx_t;

int  cxl_init(cxl_ctx_t *ctx, const char *dev_path);
void cxl_fini(cxl_ctx_t *ctx);

void *cxl_get_input_buf(cxl_ctx_t *ctx);
void *cxl_get_output_buf(cxl_ctx_t *ctx);
cxl_ctrl_block_t *cxl_get_ctrl(cxl_ctx_t *ctx);

/* Fallback: use NUMA node memory to emulate CXL if no /dev/dax */
int  cxl_init_numa_emulation(cxl_ctx_t *ctx, int numa_node);

#endif /* CXL_MEMORY_H */
```

Create `gpu_daemon/cxl_memory.c`:
Implement CXL memory initialization:
1. First try to open `/dev/dax0.0` (real CXL Type-3 device)
2. If not available, fall back to NUMA node 1 memory via mbind()
3. If NUMA not available, fall back to regular mmap with MAP_SHARED + MAP_ANONYMOUS
4. MUST work in all three scenarios — the code adapts
5. mmap with MAP_SHARED so both processes see same physical memory

```c
int cxl_init(cxl_ctx_t *ctx, const char *dev_path) {
    /* Try real CXL device first */
    ctx->fd = open(dev_path, O_RDWR);
    if (ctx->fd >= 0) {
        /* Real CXL Type-3 device */
        ctx->base = mmap(NULL, CXL_REGION_TOTAL_SIZE,
                        PROT_READ|PROT_WRITE,
                        MAP_SHARED, ctx->fd, 0);
        if (ctx->base != MAP_FAILED) {
            printf("[CXL] Using real CXL device: %s\n", dev_path);
            goto success;
        }
        close(ctx->fd);
    }
    
    /* Fallback: NUMA node emulation */
    return cxl_init_numa_emulation(ctx, 1);
    
success:
    ctx->ctrl = (cxl_ctrl_block_t *)
        ((uint8_t *)ctx->base + CXL_REGION_CTRL_OFF);
    memset(ctx->ctrl, 0, sizeof(cxl_ctrl_block_t));
    return 0;
}
```

## PHASE 3: eBPF INTERCEPTION LAYER

### Step 3.1: eBPF Kernel Program
Create `ebpf/l1_intercept.h` (shared between kernel and userspace):
```c
#ifndef L1_INTERCEPT_H
#define L1_INTERCEPT_H

#include <stdint.h>

#define WORK_TYPE_LDPC  1
#define WORK_TYPE_FFT   2
#define WORK_TYPE_DONE  3

/* Event sent from eBPF to GPU daemon via ring buffer */
struct offload_event {
    uint32_t work_type;     /* WORK_TYPE_LDPC or WORK_TYPE_FFT */
    uint32_t pid;           /* PID of L1 process */
    uint64_t input_addr;    /* virtual address of input buffer */
    uint64_t output_addr;   /* virtual address of output buffer */
    uint32_t input_len;     /* bytes */
    uint32_t output_len;    /* bytes */
    uint64_t timestamp_ns;  /* submission time */
};

/* Completion notification from GPU daemon to L1 via BPF map */
struct completion_info {
    uint32_t pid;
    uint32_t work_type;
    uint64_t result_addr;   /* CXL address where result is written */
    uint64_t latency_ns;    /* GPU processing time */
    int      retcode;       /* 0 = success */
};

/* Statistics tracked in BPF map */
struct stats {
    uint64_t ldpc_offloads;
    uint64_t fft_offloads;
    uint64_t total_bytes;
    uint64_t total_latency_ns;
    uint64_t errors;
};

#endif /* L1_INTERCEPT_H */
```

Create `ebpf/l1_intercept.bpf.c`:
```c
// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "l1_intercept.h"

/* Ring buffer for sending events to GPU daemon */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);  /* 256KB ring buffer */
} offload_ringbuf SEC(".maps");

/* Hash map: pid → completion info (GPU daemon writes, L1 reads) */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, u32);
    __type(value, struct completion_info);
} completion_map SEC(".maps");

/* Global stats */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, struct stats);
} stats_map SEC(".maps");

/* Control flag: is offloading enabled? */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);
} offload_enabled SEC(".maps");

/* uprobe on ldpc_decode() entry */
SEC("uprobe/ldpc_decode")
int BPF_UPROBE(intercept_ldpc_decode, const void *params)
{
    u32 key = 0;
    u32 *enabled = bpf_map_lookup_elem(&offload_enabled, &key);
    if (!enabled || !*enabled)
        return 0;  /* Offloading disabled — let CPU handle it */
    
    struct offload_event *event;
    event = bpf_ringbuf_reserve(&offload_ringbuf,
                                 sizeof(*event), 0);
    if (!event)
        return 0;
    
    event->work_type = WORK_TYPE_LDPC;
    event->pid = bpf_get_current_pid_tgid() >> 32;
    event->timestamp_ns = bpf_ktime_get_ns();
    
    /* Read input/output addresses from ldpc_params_t */
    /* struct ldpc_params_t: first field is input ptr */
    bpf_probe_read_user(&event->input_addr,
                        sizeof(event->input_addr),
                        params);
    /* offset 8 is output ptr */
    bpf_probe_read_user(&event->output_addr,
                        sizeof(event->output_addr),
                        (char *)params + 8);
    /* offset 16 is input_len */
    bpf_probe_read_user(&event->input_len,
                        sizeof(event->input_len),
                        (char *)params + 16);
    
    bpf_ringbuf_submit(event, 0);
    
    /* Update stats */
    struct stats *s = bpf_map_lookup_elem(&stats_map, &key);
    if (s) {
        __sync_fetch_and_add(&s->ldpc_offloads, 1);
        __sync_fetch_and_add(&s->total_bytes, event->input_len);
    }
    
    return 0;
}

/* uprobe on fft_process() entry */
SEC("uprobe/fft_process")
int BPF_UPROBE(intercept_fft_process, const void *params)
{
    u32 key = 0;
    u32 *enabled = bpf_map_lookup_elem(&offload_enabled, &key);
    if (!enabled || !*enabled)
        return 0;
    
    struct offload_event *event;
    event = bpf_ringbuf_reserve(&offload_ringbuf,
                                 sizeof(*event), 0);
    if (!event)
        return 0;
    
    event->work_type = WORK_TYPE_FFT;
    event->pid = bpf_get_current_pid_tgid() >> 32;
    event->timestamp_ns = bpf_ktime_get_ns();
    
    /* Read FFT size from fft_params_t */
    bpf_probe_read_user(&event->input_addr,
                        sizeof(event->input_addr),
                        params);
    /* N is at offset 16 */
    size_t N = 0;
    bpf_probe_read_user(&N, sizeof(N), (char *)params + 16);
    event->input_len = N * 8;  /* complex float = 8 bytes */
    
    bpf_ringbuf_submit(event, 0);
    
    u32 skey = 0;
    struct stats *s = bpf_map_lookup_elem(&stats_map, &skey);
    if (s)
        __sync_fetch_and_add(&s->fft_offloads, 1);
    
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
```

### Step 3.2: eBPF Loader (Userspace)
Create `ebpf/l1_intercept_loader.c`:

Must do:
1. Load and verify the BPF program
2. Find the binary offset of `ldpc_decode` and `fft_process` in the L1 simulator binary
3. Attach uprobes to both functions
4. Start ring buffer polling thread
5. On event: forward to GPU daemon via UNIX socket or shared pipe
6. Provide API to enable/disable offloading at runtime
7. Print live stats periodically

Key implementation:
```c
/* Find function offset in ELF binary */
uint64_t find_symbol_offset(const char *binary, const char *sym_name) {
    /* Use libelf or parse /proc/pid/maps */
    /* Return file offset for uprobe attachment */
}

/* Attach uprobe */
int attach_uprobe(struct bpf_program *prog,
                  const char *binary, uint64_t offset) {
    /* bpf_program__attach_uprobe() */
}

/* Ring buffer callback */
int handle_offload_event(void *ctx, void *data, size_t size) {
    struct offload_event *event = data;
    /* Forward to GPU daemon */
    /* Write to UNIX socket */
    return 0;
}
```

## PHASE 4: GPU ACCELERATOR DAEMON

### Step 4.1: GPU Daemon Main
Create `gpu_daemon/gpu_daemon.c`:

The daemon must:
1. Initialize CXL memory (map shared region)
2. Connect to eBPF loader via UNIX socket
3. Main loop: receive work events, process, signal completion
4. Implement "GPU" compute as optimized CPU code
5. Track and report processing latency

```c
#define GPU_DAEMON_SOCKET "/tmp/gpu_daemon.sock"
#define MAX_QUEUE_DEPTH   64

typedef struct {
    int              sock_fd;
    cxl_ctx_t        cxl;
    pthread_t        worker_thread;
    pthread_mutex_t  queue_mutex;
    pthread_cond_t   queue_cond;
    
    /* Work queue */
    struct offload_event queue[MAX_QUEUE_DEPTH];
    int              queue_head;
    int              queue_tail;
    int              queue_count;
    
    /* Stats */
    uint64_t         processed_ldpc;
    uint64_t         processed_fft;
    uint64_t         total_latency_ns;
} gpu_daemon_ctx_t;

/* Main processing loop */
void *gpu_worker_thread(void *arg) {
    gpu_daemon_ctx_t *ctx = arg;
    
    while (1) {
        struct offload_event event;
        
        /* Dequeue work */
        pthread_mutex_lock(&ctx->queue_mutex);
        while (ctx->queue_count == 0)
            pthread_cond_wait(&ctx->queue_cond, &ctx->queue_mutex);
        event = ctx->queue[ctx->queue_head];
        ctx->queue_head = (ctx->queue_head + 1) % MAX_QUEUE_DEPTH;
        ctx->queue_count--;
        pthread_mutex_unlock(&ctx->queue_mutex);
        
        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        
        /* Process work */
        if (event.work_type == WORK_TYPE_LDPC) {
            gpu_compute_ldpc(&ctx->cxl, &event);
            ctx->processed_ldpc++;
        } else if (event.work_type == WORK_TYPE_FFT) {
            gpu_compute_fft(&ctx->cxl, &event);
            ctx->processed_fft++;
        }
        
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        uint64_t lat = timespec_diff_ns(&t_start, &t_end);
        ctx->total_latency_ns += lat;
        
        /* Signal completion via BPF map */
        /* (write to completion_map) */
        signal_completion(&event, lat);
    }
    return NULL;
}
```

### Step 4.2: GPU Compute Functions
Create `gpu_daemon/gpu_compute.c`:

Implement compute functions that:
1. READ input from CXL memory (not from process memory)
2. COMPUTE the result (same algorithms as L1 sim)
3. WRITE result to CXL memory output region
4. This simulates what a GPU would do via DMA

```c
/* GPU LDPC decode: reads from CXL, writes result to CXL */
int gpu_compute_ldpc(cxl_ctx_t *cxl,
                     const struct offload_event *event) {
    /* Input is in CXL memory at event->input_addr offset */
    void *input  = (uint8_t *)cxl->base + (event->input_addr & 0x1FFFFFFF);
    void *output = (uint8_t *)cxl->base + CXL_REGION_GPU_OUTPUT_OFF;
    
    /* Run LDPC decode on the data */
    ldpc_params_t params = {
        .input      = input,
        .output     = output,
        .input_len  = event->input_len,
        .output_len = event->output_len,
        .base_graph = 1,
        .lifting_size = 384,
    };
    
    /* This is the actual compute — same algorithm as CPU path */
    /* In a real GPU: this would be a CUDA/ROCm kernel launch */
    /* Here: CPU executes it, proving the architecture */
    return ldpc_decode_internal(&params);
}

int gpu_compute_fft(cxl_ctx_t *cxl,
                    const struct offload_event *event) {
    void *input  = (uint8_t *)cxl->base + (event->input_addr & 0x1FFFFFFF);
    void *output = (uint8_t *)cxl->base + CXL_REGION_GPU_OUTPUT_OFF;
    
    fft_params_t params = {
        .input     = input,
        .output    = output,
        .N         = event->input_len / 8,  /* complex float */
        .direction = -1,   /* inverse FFT for receiver */
    };
    
    return fft_process_internal(&params);
}
```

## PHASE 5: MEASUREMENT FRAMEWORK

### MEASUREMENT GROUND RULES (Read Before Writing Any Measurement Code)

These rules come directly from prior art analysis. Violating them produces results
a reviewer will reject immediately.

**Rule 1 — The Calibration Check (MANDATORY):**
Before claiming any result, verify your experimental setup is correct by reproducing
the "Six Times to Spare" (arXiv:2602.04652) CPU baseline:
- Large TB (270 RB, 16QAM, 20 LDPC iterations) CPU decode = ~0.71 ms
- This EXCEEDS the 0.5 ms slot budget → motivates GPU offload
If your CPU baseline is far from this number, your LDPC implementation
is wrong or misconfigured. Fix it before measuring offload.
Write this to: `paper/results/calibration_check.txt`
Format: "CPU LDPC large-TB baseline: X.XX ms (target: ~0.71 ms)"

**Rule 2 — Differentiation from AtlasRAN:**
AtlasRAN (arXiv:2603.14661) measured OAI offload to NVIDIA GPU with CUDA.
Your differentiator is TRANSPARENT INTERCEPTION — the L1 application is NOT modified.
Measure and report: "zero lines of L1 application code changed"
This is a qualitative claim that must be explicitly stated in results.

**Rule 3 — Differentiation from eGPU:**
eGPF used bpftime (userspace eBPF) targeting GPU-side kernels.
Your uprobe targets the CPU-side L1 application.
Measure and report: uprobe attachment success/failure rate and overhead
at the CPU application function boundary — not inside GPU driver.

**Rule 4 — The 0.5 ms Hard Deadline Analysis:**
Every result table must include a "slot_deadline_miss_rate" column.
Definition: miss if total end-to-end latency (including offload path) > 500 µs.
This is the unique research question nobody else has answered:
"Can CXL + eBPF offload stay within a 0.5 ms hard real-time constraint?"

### Step 5.1: Latency Measurement
Create `measurement/measure.c`:

Must measure and record:
```
1. Calibration (run first — verify setup is correct):
   - CPU LDPC decode for large TB (270 RB, 20 iterations)
   - Target: ~0.71 ms (Six Times to Spare baseline)
   - If >2x or <0.1x this value: STOP, fix LDPC implementation
   - Write to: paper/results/calibration_check.txt

2. Baseline (CPU-only, no eBPF):
   - Per-slot latency (microseconds)
   - LDPC decode time per code block (µs)
   - FFT time per symbol (µs)
   - Slot deadline miss rate (>500 µs = miss)
   - emulation_mode column: "cpu-baseline"

3. Offload path (eBPF + GPU daemon + CXL):
   - eBPF interception overhead (ns) — isolated measurement
   - BPF ring buffer latency (ns) — kernel→userspace
   - GPU daemon scheduling latency (µs)
   - GPU compute time (CXL read + compute + CXL write) (µs)
   - Total offload round-trip latency (µs)
   - Slot deadline miss rate (>500 µs = miss)
   - emulation_mode column: "ebpf-cxl-offload"

4. eBPF overhead isolated (no actual offload — just intercept + return):
   - Attach uprobe, fire, return immediately without offloading
   - Measure pure eBPF cost
   - Target: ~1.7 µs (Cloudflare benchmark baseline)
   - emulation_mode column: "ebpf-overhead-only"

5. Component breakdown:
   t_ebpf_probe:     uprobe fires → ring buffer write
   t_ringbuf:        ring buffer write → daemon wakes
   t_daemon_sched:   daemon wakeup → computation starts
   t_compute:        CXL read + LDPC/FFT compute + CXL write
   t_signal:         completion signal back to L1
   t_total:          sum of all above
   Verify: t_total = t_ebpf_probe + t_ringbuf + t_daemon_sched
           + t_compute + t_signal (within 5% tolerance)

6. CXL memory characterization:
   - Sequential read bandwidth (GB/s) from CXL region
   - Sequential write bandwidth (GB/s) to CXL region
   - Random access latency (ns) for 64-byte cache lines
   - Compare to local DRAM (numactl --membind=0)
   - emulation_mode column: "qemu-cxl-type3"

Output format: CSV files in paper/results/
  calibration_check.txt
  baseline_latency.csv        (columns: slot,latency_us,deadline_miss,emulation_mode)
  offload_latency.csv         (columns: slot,latency_us,deadline_miss,breakdown_json,emulation_mode)
  ebpf_overhead_only.csv      (columns: call_id,overhead_ns,emulation_mode)
  breakdown.csv               (columns: component,mean_us,p50,p95,p99,max,emulation_mode)
  cxl_bandwidth.csv           (columns: op,size_kb,bandwidth_gbps,latency_ns,emulation_mode)
```

Run 10,000 slots for each configuration.
Report: mean, p50, p95, p99, max latency, slot deadline miss rate.

### Step 5.2: Results Plotting
Create `measurement/plot_results.py`:

Generate publication-quality figures:
```python
# Figure 1: Latency CDF comparison (THE KEY FIGURE)
#   X: latency (microseconds, log scale)
#   Y: CDF (0-1)
#   Lines: CPU-only baseline, eBPF+GPU offload, eBPF overhead only
#   Red vertical line at 500us (0.5ms slot budget — THE deadline)
#   Annotation: "Six Times to Spare CPU baseline: 710µs" marker
#   This figure directly answers: "does offload fit within 0.5ms?"

# Figure 2: Latency breakdown stacked bar
#   X: experiment mode (CPU, eBPF overhead, full offload)
#   Y: latency (microseconds)
#   Stacks: eBPF probe, ring buffer, daemon sched, compute, signal
#   Shows: where time is spent, which component to optimize

# Figure 3: Slot deadline miss rate
#   X: emulation mode (0ns, 142ns, 255ns CXL latency)
#   Y: deadline miss rate (%)
#   Lines: CPU-only (dashed), eBPF offload (solid)
#   This is the NUMA sensitivity figure — required for paper

# Figure 4: CXL memory bandwidth vs local DRAM
#   X: transfer size (KB)
#   Y: bandwidth (GB/s)
#   Lines: local DRAM, QEMU CXL Type-3
#   Shows: CXL bandwidth is sufficient for LDPC input/output sizes

# Figure 5: Component breakdown timeline
#   Waterfall chart showing t_ebpf → t_ringbuf → t_daemon → t_compute → t_signal
#   Shows the critical path through the offload stack
#   Highlights which component dominates

# All figures: save as PDF and PNG in paper/figures/
# Use matplotlib with IEEE two-column paper style (3.5 inch width)
# Font: Times New Roman or similar serif for IEEE
# DPI: 300 minimum for PDF submission
```

### Step 5.3: Auto-generate Paper Notes
Create `measurement/generate_notes.py`:

After all measurements complete, auto-generate `paper/notes.md`:
```python
"""
Generate paper/notes.md with key findings for the paper.
This is the raw material for the evaluation section.
"""

template = """
## Auto-generated Measurement Summary
Generated: {timestamp}
Platform: {uname}
Emulation: {emulation_mode}

### Calibration Check
CPU LDPC large-TB baseline: {cpu_ldpc_large_ms:.3f} ms
Target (Six Times to Spare): ~0.710 ms
Status: {calibration_status}

### Key Results

#### eBPF Overhead (isolated)
Mean overhead per intercept: {ebpf_mean_ns:.0f} ns
Expected (~1670 ns per Cloudflare benchmark): ±30% = OK
Status: {ebpf_overhead_status}

#### CPU-only Baseline
Mean per-slot latency: {cpu_mean_us:.1f} µs
Slot deadline (500µs) miss rate: {cpu_miss_rate:.1f}%

#### eBPF + CXL Offload Path
Mean per-slot latency: {offload_mean_us:.1f} µs
Slot deadline (500µs) miss rate: {offload_miss_rate:.1f}%

#### Latency Breakdown (offload path)
eBPF probe:          {t_ebpf_us:.2f} µs
Ring buffer:         {t_ringbuf_us:.2f} µs
Daemon scheduling:   {t_daemon_us:.2f} µs
Compute (LDPC/FFT):  {t_compute_us:.2f} µs
Completion signal:   {t_signal_us:.2f} µs
TOTAL:               {t_total_us:.2f} µs

#### CXL Memory (QEMU Type-3)
Sequential read bandwidth:  {cxl_read_bw:.1f} GB/s
Sequential write bandwidth: {cxl_write_bw:.1f} GB/s
Random 64B latency:         {cxl_latency_ns:.0f} ns

#### NUMA Sensitivity (CXL latency emulation)
0 ns (local DRAM):    deadline miss rate = {miss_0ns:.1f}%
142 ns (Pond CXL):    deadline miss rate = {miss_142ns:.1f}%
255 ns (Pond max):    deadline miss rate = {miss_255ns:.1f}%

### Projected Results with Real GPU
Based on arXiv:2602.04652 (Six Times to Spare):
Real GPU LDPC = ~6x faster than CPU
CPU baseline large-TB: ~710µs → GPU: ~118µs
With eBPF overhead ({ebpf_mean_ns:.0f} ns) + CXL memory:
Projected total offload latency: {projected_gpu_us:.0f} µs
Projected slot deadline compliance: {projected_compliance}%

### Novelty Evidence
- L1 application lines changed: 0 (zero — transparent interception)
- eBPF intercept location: ldpc_decode() CPU-side function boundary
- CXL path: QEMU Type-3 → /dev/dax0.0 → NUMA node (system-ram mode)
- Accelerator: CPU-based OpenCL daemon (architecture proof)
- Real GPU: drop-in replacement via same eBPF+CXL path
"""
```


## PHASE 6: INTEGRATION AND VERIFICATION

### Step 6.1: Integration Script
Create `scripts/run_poc.sh`:
```bash
#!/bin/bash
set -e

echo "=== CXL RAN PoC Integration Test ==="

# 1. Setup CXL device
echo "[1/6] Setting up CXL memory..."
./scripts/setup_cxl.sh
ls /dev/dax* 2>/dev/null || echo "No DAX device — using NUMA emulation"

# 2. Build all components
echo "[2/6] Building..."
make all

# 3. Start GPU daemon
echo "[3/6] Starting GPU accelerator daemon..."
./gpu_daemon/gpu_daemon --cxl-path /dev/dax0.0 \
    --socket /tmp/gpu_daemon.sock \
    --verbose &
GPU_DAEMON_PID=$!
sleep 1

# 4. Load eBPF programs
echo "[4/6] Loading eBPF interception..."
./ebpf/l1_intercept_loader \
    --binary ./l1_sim/ran_l1_sim \
    --socket /tmp/gpu_daemon.sock \
    --enable-offload &
EBPF_PID=$!
sleep 1

# 5. Run baseline (CPU only)
echo "[5/6] Running baseline measurement (CPU only)..."
./measurement/measure --mode baseline \
    --slots 10000 \
    --output paper/results/baseline_latency.csv

# 6. Run offload measurement
echo "[6/6] Running offload measurement (eBPF + GPU + CXL)..."
./measurement/measure --mode offload \
    --slots 10000 \
    --output paper/results/offload_latency.csv

# Cleanup
kill $GPU_DAEMON_PID $EBPF_PID 2>/dev/null

# Plot results
python3 measurement/plot_results.py \
    --baseline paper/results/baseline_latency.csv \
    --offload paper/results/offload_latency.csv \
    --output-dir paper/figures/

echo "=== Done. Results in paper/results/, Figures in paper/figures/ ==="
```

### Step 6.15: NUMA Latency Sweep (Required for Paper Credibility)
Create `scripts/numa_sweep.sh`:
```bash
#!/bin/bash
# Emulate CXL latency using NUMA node memory binding
# Based on Pond (ASPLOS'23) methodology: CPU-less NUMA node as CXL proxy
# Real CXL.mem latency range: 140-255 ns (Pond measurements)

# Check NUMA nodes available
NUMA_NODES=$(numactl --hardware | grep "available:" | awk '{print $2}')
echo "NUMA nodes available: ${NUMA_NODES}"

if [ "${NUMA_NODES}" -lt 2 ]; then
    echo "WARN: Single NUMA node — using numactl membind with delay injection"
    echo "      Install CXLMemSim for accurate latency modeling"
    echo "      Proceeding with local memory (0ns CXL latency only)"
    
    # Run with local memory only (0ns case)
    numactl --cpunodebind=0 --membind=0 \
        ./measurement/measure --mode offload \
        --slots 10000 \
        --output paper/results/numa_latency_0ns.csv \
        --label "local-dram-0ns"
    
    echo "WARNING: Cannot emulate CXL latency without 2+ NUMA nodes" \
        >> paper/results/emulation_mode.txt
    echo "Latency sensitivity analysis skipped — single NUMA node host" \
        >> paper/results/emulation_mode.txt
    exit 0
fi

# 0ns case: local DRAM baseline
echo "[1/3] Running 0ns (local DRAM baseline)..."
numactl --cpunodebind=0 --membind=0 \
    ./measurement/measure --mode offload \
    --slots 10000 \
    --output paper/results/numa_latency_0ns.csv \
    --label "local-dram-0ns"

# 142ns case: Pond-measured CXL remote NUMA latency
echo "[2/3] Running 142ns (Pond CXL emulation — remote NUMA)..."
numactl --cpunodebind=0 --membind=1 \
    ./measurement/measure --mode offload \
    --slots 10000 \
    --output paper/results/numa_latency_142ns.csv \
    --label "remote-numa-142ns-cxl-proxy"

# 255ns case: Pond max CXL latency (lab server)
# Inject additional 113ns via CXLMemSim if available
if command -v cxlmemsim &>/dev/null; then
    echo "[3/3] Running 255ns (CXLMemSim latency injection)..."
    cxlmemsim --latency=255 -- \
        numactl --cpunodebind=0 --membind=1 \
        ./measurement/measure --mode offload \
        --slots 10000 \
        --output paper/results/numa_latency_255ns.csv \
        --label "cxlmemsim-255ns"
else
    echo "[3/3] CXLMemSim not available — using remote NUMA as 255ns proxy..."
    echo "      (Remote NUMA is typically ~142ns; 255ns requires CXLMemSim)" 
    numactl --cpunodebind=0 --membind=1 \
        ./measurement/measure --mode offload \
        --slots 10000 \
        --output paper/results/numa_latency_255ns.csv \
        --label "remote-numa-255ns-approx"
fi

echo "NUMA sweep complete. Results:"
echo "  0ns:   paper/results/numa_latency_0ns.csv"
echo "  142ns: paper/results/numa_latency_142ns.csv"
echo "  255ns: paper/results/numa_latency_255ns.csv"

# Measure actual NUMA latency for calibration
numactl --cpunodebind=0 --membind=0 \
    mlc --latency_matrix 2>/dev/null || \
    numactl --cpunodebind=0 --membind=0 \
    lmbench lat_mem_rd 512m 128 2>/dev/null || \
    echo "Install mlc or lmbench to measure actual NUMA latency"
```

Also add to `measurement/plot_results.py` a new figure:
```python
# Figure 5: NUMA Latency Sensitivity (the key paper figure)
#   X: CXL emulated latency (0 ns, 142 ns, 255 ns)
#   Y: Slot deadline miss rate (%) and mean offload latency (us)
#   Title: "Impact of CXL Memory Latency on Slot Deadline Compliance"
#   Annotation: "0.5ms TTI budget" horizontal line
#   Source labels: "Local DRAM", "Pond CXL", "Pond Max CXL"
#   This directly addresses: "how does CXL latency affect RAN feasibility?"
```

### Step 6.2: Verification Tests
Create `scripts/verify.sh`:

Must verify:
1. CXL device or emulation is accessible
2. eBPF programs load without error
3. GPU daemon starts and connects
4. A single LDPC decode offload completes successfully
5. CXL memory is coherent (both processes see same data)
6. Latency numbers are physically plausible (not zero, not infinite)
7. No kernel panics or BPF verifier rejections

## PHASE 7: PAPER DOCUMENTATION

### Step 7.1: README
Create `README.md` with:
- Overview of the architecture and its significance
- How it differs from NVIDIA Aerial (open vs proprietary)
- Hardware requirements (what works with QEMU, what needs real HW)
- Build and run instructions (3 commands should be enough)
- Expected results (latency numbers, what they mean)
- Limitations and future work (real GPU, real CXL hardware)

### Step 7.2: Paper Notes
Create `paper/notes.md`:

Document automatically after measurement runs:
```markdown
## Measurement Results

### Environment
- Kernel version: $(uname -r)
- CXL: real device / NUMA emulation / mmap emulation
- CPU: $(lscpu | grep "Model name")

### Key Results
- Baseline LDPC per code block: X ± Y us
- Offload LDPC per code block: X ± Y us
- eBPF interception overhead: X ns
- CXL memory bandwidth: X GB/s
- Slot deadline (500us) miss rate baseline: X%
- Slot deadline miss rate offload: X%

### Interpretation
[Generate automatically based on numbers]
```

## AGENT EXECUTION LOOP

Execute these phases in order. After each phase:
1. Run the verification step for that phase
2. If verification fails: debug, fix, re-run
3. If verification passes: commit to git, move to next phase
4. Log progress to `build.log`

```
while not all_phases_complete:
    result = execute_phase(current_phase)
    if result == SUCCESS:
        git_commit(f"Phase {current_phase} complete")
        current_phase += 1
    else:
        debug_and_fix(result.error)
        retry_count += 1
        if retry_count > 5:
            log_blocker(current_phase, result.error)
            try_alternative_approach()
```

## FALLBACK STRATEGIES

If any component cannot be built exactly as specified, use these fallbacks in order. **Always log which fallback was used in `paper/results/emulation_mode.txt`.**

### CXL Device Fallback (in order)
1. QEMU CXL Type-3 via `/dev/dax0.0` → daxctl → system-ram NUMA node ← **preferred**
   Verify: `cxl list` shows device, `numactl --hardware` shows new NUMA node
2. QEMU NUMA node 1 as CXL proxy (no CXL device, just remote NUMA memory)
   Use: `numactl --membind=1` for all CXL allocations
   Note in paper: "CXL emulated via NUMA-remote memory (Pond methodology)"
3. MAP_SHARED file-backed mmap on tmpfs (shared memory, no NUMA latency)
   Use: `mmap(NULL, size, PROT_RW, MAP_SHARED, fd, 0)` on `/tmp/cxl_shm`
   Note in paper: "Shared memory with software coherency, no latency modeling"
4. MAP_SHARED anonymous mmap (last resort — no persistence, no NUMA)
   Log: "No CXL emulation — shared anonymous memory only"

### GPU / Accelerator Fallback (in order)
1. PoCL OpenCL CPU runtime with OpenCL LDPC/FFT kernels ← **preferred**
   Install: `sudo apt install pocl-opencl-icd`
   Verify: `clinfo | grep "CPU"` shows a device
2. Intel oneAPI CPU runtime via `ONEAPI_DEVICE_SELECTOR=opencl:cpu`
   Install: Intel oneAPI Base Toolkit
3. Separate C process via UNIX socket (pure CPU, no OpenCL)
   This is architectural proof without any GPU API surface
4. Thread within same process (minimum — proves data flow only)

### eBPF Fallback (in order)
1. eBPF uprobe on function name in dynamic library ← **preferred**
   For srsRAN LDPC shared lib: probe `ldpc_decoder_hw_impl`
2. eBPF uprobe on binary offset (use `objdump -d` to find offset)
3. LD_PRELOAD wrapper interposition (if eBPF verifier fails)
   Intercept `ldpc_decode` via `dlsym(RTLD_NEXT, ...)`, forward to daemon
4. Explicit instrumentation: modify benchmark to call offload API directly
   (Weakest — no longer "transparent" but proves data path)

### L1 Workload Fallback (in order)
1. srsRAN `ldpc_decoder_benchmark` standalone ← **preferred, build this first**
   Repo: github.com/srsran/srsRAN_Project
   Build: `cmake -DENABLE_TESTING=ON .. && make ldpc_decoder_benchmark`
2. OAI `nr-softmodem --rfsim --phy-test` (full softmodem, rfsim mode)
   Repo: gitlab.eurecom.fr/oai/openairinterface5g
3. Standalone LDPC implementation from OpenAirInterface
   File: `openair1/PHY/CODING/nrLDPC_decoder/` — extract just the decoder
4. Custom LDPC min-sum implementation with realistic timing
   Must take ≥100 µs per code block to be meaningful

### Latency Modeling Fallback (in order)
1. QEMU CXL Type-3 + dual NUMA node Pond-style emulation ← **preferred**
2. CXLMemSim latency injection (github.com/SlugLab/CXLMemSim)
3. QEMU NUMA only (single NUMA, no latency injection)
   Note: can only demonstrate 0 ns CXL latency case
4. No latency modeling — functional only
   Note prominently in paper: "Latency sensitivity analysis requires NUMA/CXLMemSim"

## QUALITY REQUIREMENTS

Code must:
- Compile with `-Wall -Wextra -Werror` on GCC 11+
- Pass `sparse` static analysis for kernel BPF code
- Produce reproducible results (seeded random for test data)
- Handle SIGINT cleanly (shutdown all components gracefully)
- Print meaningful error messages with errno
- Include comments explaining WHY, not WHAT

Results must:
- Be statistically valid (10,000+ samples)
- Include standard deviation, not just mean
- Be reproducible across runs (±10%)

## SUCCESS CRITERIA

The PoC is complete when ALL of the following are generated:

### Stage 1 — Functional Spine (MUST PASS before Stage 2)
1. `make all` succeeds without errors
2. `scripts/verify.sh` passes all checks including:
   - CXL device appears in `cxl list` output
   - eBPF programs load without verifier errors
   - GPU daemon starts and connects
   - Single LDPC decode offload completes with bit-for-bit correct output
   - `paper/results/functional_correctness.txt` records PASS/FAIL
3. **Go/no-go threshold:** eBPF uprobe overhead measured < 5 µs per intercept
   (expected ~1.7 µs per research findings)

### Stage 2 — Full Measurement
4. `scripts/run_poc.sh` completes and generates:
   - `paper/results/baseline_latency.csv` (10K rows, CPU-only path)
   - `paper/results/offload_latency.csv` (10K rows, eBPF+daemon+CXL path)
   - `paper/results/ebpf_overhead.csv` (uprobe cost isolation)
   - `paper/results/cxl_bandwidth.csv` (shared memory throughput)
   - `paper/results/emulation_mode.txt` (records which CXL fallback was used)

### Stage 3 — Latency Sensitivity (Required for Paper)
5. `scripts/numa_sweep.sh` completes and generates:
   - `paper/results/numa_latency_0ns.csv` (local DRAM baseline)
   - `paper/results/numa_latency_142ns.csv` (Pond CXL emulation)
   - `paper/results/numa_latency_255ns.csv` (Pond max CXL latency)
   This is the paper's latency sensitivity section — mandatory for reviewer credibility

### Stage 4 — Figures
6. `measurement/plot_results.py` generates all figures:
   - `paper/figures/latency_cdf.pdf`
   - `paper/figures/latency_breakdown.pdf`
   - `paper/figures/numa_sensitivity.pdf`
   - `paper/figures/architecture_diagram.pdf`
7. The README clearly explains open vs NVIDIA Aerial with the six prior art papers

## WHAT THIS PROVES FOR THE PAPER

### Primary Claim (Functional — fully provable in this PoC)
"5G NR L1 compute workloads (LDPC decode, FFT) can be transparently
intercepted and offloaded to an accelerator via eBPF uprobe + CXL
Type-3 shared memory, WITHOUT modifying the L1 application, using
ONLY open Linux kernel primitives — no CUDA, no proprietary runtime."

### Secondary Claims (Measurable in this PoC)
- eBPF uprobe interception overhead is ~1.7 µs per call (bounded, predictable)
- CXL Type-3 shared memory enables zero-copy between L1 and accelerator daemon
- Linux kernel CXL subsystem (`drivers/cxl/`) is exercised end-to-end
- Architecture generalizes to any accelerator replacing the CPU daemon
- NUMA latency sensitivity: slot deadline compliance varies predictably with CXL latency

### What We Do NOT Claim (Honest Fidelity Statement)
- QEMU does NOT model CXL timing or coherency — absolute latency numbers are
  NUMA-emulated or projected, not from CXL hardware
- CXL Type-2 coherent accelerator behavior is approximated via Type-3 + shared memory
- Real-time RAN L1 deadlines (0.5 ms) are NOT validated — functional correctness only
- GPU speedup numbers are projected from "Six Times to Spare" (arXiv:2602.04652),
  not measured — real GPU testing requires AMD MI300X or similar hardware

### Differentiation from Prior Art (Must State in Paper)
- vs AtlasRAN: we use eBPF transparent interception (no app modification) + CXL emulation
  instead of real NVIDIA hardware + CUDA
- vs eGPU: we target 5G NR L1 timing domain (0.5 ms TTI budget analysis) with
  kernel uprobes instead of bpftime userspace eBPF
- vs UDON: we demonstrate RAN workload offload specifically with 5G NR timing analysis
- vs "Six Times to Spare": we use open stack (no CUDA) with CXL memory path instead
  of NVIDIA-only DGX hardware

## FINAL NOTE TO AGENT

### From the Research Findings — The Three Non-Negotiables

1. **Never claim QEMU provides accurate CXL latency.** It does not. Every latency figure must be labeled with its source: `qemu-functional`, `numa-emulated-142ns`, `numa-emulated-255ns`, or `cxlmemsim-injected`. A result without this label is invalid for the paper.

2. **Intercept at transport-block granularity, not per-symbol or per-iteration.** eBPF uprobe costs ~1.7 µs. With 0.5 ms slot budget, you can afford ~10 intercepts per slot. Finer than that blows the budget. This is a hard constraint from measurements, not a preference.

3. **srsRAN standalone benchmarks first, OAI full softmodem second.** The research confirms srsRAN ships `ldpc_decoder_benchmark` and `pusch_processor_benchmark` as standalone binaries. These are the cleanest interception targets. Build against them first. OAI integration comes after the architecture is validated.

### When something cannot be built exactly as specified:
- Don't stop
- Don't ask
- Use the fallback strategy in order
- Log what fallback was used and why in `paper/results/emulation_mode.txt`
- Continue to next component

### The Honest Goal
A working, measurable, documented system that proves the ARCHITECTURE of transparent eBPF-intercepted L1 offload through CXL shared memory.

Not a performance claim. Not a real-time RAN claim. An architectural proof that this open stack CAN work — and a quantitative analysis of what real hardware would need to deliver.

**A working system with honest caveats > a broken system that's architecturally pure.**

Build it. Run it. Measure it. Document it honestly.
