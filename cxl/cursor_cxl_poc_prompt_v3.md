# Open AI-RAN PoC — Cursor Agent Prompt v3
# Uses proper open-source simulators. Publication-grade.

## AGENT ROLE

You are a senior Linux kernel and systems engineer. Build a
paper-quality PoC demonstrating transparent L1 RAN workload offload
via eBPF + CXL shared memory. Work autonomously. Never stop to ask.
When something fails — debug, fix, retry. Log every decision.

## WHY THIS VERSION EXISTS

Version 2 of this prompt used synthetic/dummy implementations:
custom LDPC decoder, custom FFT, bare-C GPU functions. A professor
reviewing that work would immediately reject it. This version uses
real open-source tools throughout:

  L1 workload:   srsRAN Project (real 5G NR BG1/BG2 LDPC)
  GPU API:       PoCL + OpenCL (real GPU API surface, CPU backend)
  CXL memory:    QEMU 9.x + daxctl + drivers/cxl/ (real kernel path)
  eBPF loader:   libbpf + CO-RE (production-grade eBPF)
  Latency model: NUMA emulation (Pond methodology, ASPLOS'23)

Every component must be traceable to a real upstream project.
No synthetic stubs that produce fake numbers.

---

## RESEARCH CONTEXT

### The Novel Contribution (what no prior paper has done)

```
Prior work:
  AtlasRAN (2603.14661):   GPU + RAN — CUDA, modified OAI, real DGX
  eGPU (HCDS'25):         eBPF + GPU — ML workloads, no RAN, no CXL
  UDON (2404.02868):       CXL + offload — CPU near-memory, no eBPF
  Six Times to Spare:      GPU LDPC — CUDA only, no CXL, no eBPF
  CXLAimPod (2508.15980):  eBPF + CXL — profiling only, no compute

This work:
  eBPF uprobe transparent interception of UNMODIFIED srsRAN
  application, routing LDPC/FFT compute through CXL Type-3
  shared memory to an OpenCL accelerator daemon, analyzed
  within the 0.5 ms 5G NR slot constraint, open stack only.
  eGPU's own authors called CXL memory pools "future work."
  This is that future work, applied to 5G NR L1.
```

### Hard Constraints From Feasibility Study

```
QEMU CXL:     Functional only — does NOT model latency or
              coherency. Never claim QEMU gives real CXL timing.

eBPF overhead: ~1670 ns per uprobe (Cloudflare benchmark).
              Intercept at transport-block level only.
              Per-symbol interception would blow 0.5ms budget.

CPU baseline:  srsRAN large TB LDPC ~0.71 ms at 20 iterations
              (Six Times to Spare, arXiv:2602.04652).
              Reproduce this number first. If you can't, your
              setup is wrong. Fix it before measuring offload.

NUMA latency:  Pond (ASPLOS'23) = accepted emulation methodology.
              142 ns remote = CXL proxy. 255 ns = max CXL.
              Run sweep: 0 ns / 142 ns / 255 ns.
              Results without this sweep are not publishable.
```

---

## REAL TOOLS — VERSIONS AND SOURCES

### Tool 1: srsRAN Project (L1 RAN Workload)

```
Repo:    https://github.com/srsran/srsRAN_Project
Commit:  latest main branch (do NOT use srsRAN4G — different project)
License: AGPL-3.0

Purpose: Provides standalone LDPC and PUSCH benchmarks that exercise
         real 5G NR Base Graph 1 and Base Graph 2 LDPC coding.
         These are the interception targets for eBPF uprobes.

Key binaries to build:
  ldpc_decoder_benchmark
  ldpc_encoder_benchmark
  pusch_processor_benchmark

Build instructions:
  git clone https://github.com/srsran/srsRAN_Project.git
  cd srsRAN_Project
  mkdir build && cd build
  cmake -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_TESTING=ON \
        -DENABLE_AVX2=ON \
        -DENABLE_AVX512=OFF \
        -DBUILD_TESTS=ON \
        ..
  make -j$(nproc) ldpc_decoder_benchmark \
                  ldpc_encoder_benchmark \
                  pusch_processor_benchmark

Verification:
  ./apps/examples/phy/ldpc_decoder_benchmark --help
  ./apps/examples/phy/ldpc_decoder_benchmark \
    --nof_repetitions=1000 \
    --nof_codeblocks=1 \
    --bg=1 \
    --Rv=0 \
    --nof_iterations=20
  
  Expected output: throughput in Mbps and latency in microseconds.
  Record this as your CPU baseline.

Symbol to probe (find with nm/objdump):
  The LDPC decode entry point. Run:
    nm ./apps/examples/phy/ldpc_decoder_benchmark | grep -i decode
  OR:
    objdump -t ./apps/examples/phy/ldpc_decoder_benchmark | \
      grep -i "ldpc.*decode\|decode.*ldpc"
  
  Note: srsRAN is C++ — symbols are mangled. Use:
    nm --demangle ./apps/examples/phy/ldpc_decoder_benchmark | \
      grep -i decode | head -20
  
  The target is typically something containing:
    srsran::ldpc_decoder or ldpc_decoder_impl::decode
  
  Get the mangled symbol name for uprobe attachment.
  Save to: paper/results/srsran_probe_symbol.txt
```

### Tool 2: PoCL (GPU OpenCL Runtime)

```
Package: pocl-opencl-icd (Ubuntu/Debian)
Version: 4.x or later (Ubuntu 22.04 ships 1.8, use PPA for newer)
Source:  https://portablecl.org / https://github.com/pocl/pocl
License: MIT

Install:
  sudo apt-get install -y pocl-opencl-icd ocl-icd-opencl-dev \
    opencl-headers clinfo

Verify CPU device is visible:
  clinfo | grep -i "device name\|device type"
  Should show: Device Type: CPU

Purpose: Exposes the CPU as an OpenCL device. The GPU daemon runs
         real OpenCL kernels (LDPC/FFT compute) against this device.
         When real GPU hardware is available, swap pocl for the
         AMD ROCm or NVIDIA CUDA OpenCL ICD — zero code changes.
         This is the "drop-in GPU" property.

The GPU daemon must:
  - Call clGetPlatformIDs() / clGetDeviceIDs() at startup
  - Select CL_DEVICE_TYPE_GPU first, fall back to CL_DEVICE_TYPE_CPU
  - Compile LDPC/FFT OpenCL kernels at runtime (JIT)
  - This is publishable as "CPU-based OpenCL runtime (PoCL)"
    with explicit note that real GPU replaces PoCL transparently
```

### Tool 3: QEMU 9.x with CXL Type-3

```
Version: QEMU 9.0 or later (CXL Type-3 volatile stable since 8.1)
Install:
  sudo apt-get install -y qemu-system-x86

Verify CXL support:
  qemu-system-x86_64 -M q35,cxl=on,help 2>&1 | grep -i cxl

CXL device bring-up sequence inside VM:
  1. Boot with CXL device (see setup script below)
  2. modprobe cxl_acpi cxl_pmem cxl_port cxl_mem
  3. cxl list                         ← verify device appears
  4. cxl create-region -d mem0 -m 0   ← create region
  5. daxctl reconfigure-device --mode=system-ram dax0.0
  6. numactl --hardware               ← verify new NUMA node
  7. ls /sys/bus/cxl/devices/         ← verify sysfs entries

This exercises the real Linux drivers/cxl/ subsystem.
Record: cxl list output to paper/results/cxl_device_info.txt
```

### Tool 4: libbpf + CO-RE (eBPF Loader)

```
Package: libbpf-dev, clang, llvm, linux-tools-$(uname -r)
Install:
  sudo apt-get install -y libbpf-dev clang llvm bpftool \
    linux-headers-$(uname -r) linux-tools-$(uname -r) \
    linux-tools-common

Purpose: libbpf with CO-RE (Compile Once Run Everywhere) produces
         eBPF programs that work across kernel versions without
         recompilation. This is the production-grade approach.

vmlinux.h generation (required for CO-RE):
  bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

Uprobe attachment approach:
  bpf_program__attach_uprobe() from libbpf
  Target: srsRAN ldpc_decoder_benchmark binary
  Symbol offset: from nm output above

Verify eBPF works in VM:
  bpftool prog list       ← should work
  bpftool map list        ← should work
  Check dmesg for BPF verifier errors after loading
```

### Tool 5: CXLMemSim (Latency Injection)

```
Repo:    https://github.com/SlugLab/CXLMemSim
Paper:   arXiv:2303.06153 (YArch'23)
Purpose: Injects CXL-realistic memory latency on commodity hardware
         without real CXL devices. Pure software.

Install:
  git clone https://github.com/SlugLab/CXLMemSim.git
  cd CXLMemSim
  mkdir build && cd build
  cmake ..
  make -j$(nproc)

Usage for latency sweep:
  ./cxlmemsim --latency=142 -- ./measurement/measure \
    --mode offload --slots 10000 \
    --output paper/results/cxl_142ns.csv

If CXLMemSim build fails: fall back to numactl NUMA emulation.
Log which method was used in paper/results/emulation_mode.txt.
```

---

## DIRECTORY STRUCTURE

```
cxl_ran_poc/
├── README.md
├── Makefile
├── build.log                       ← agent logs every decision here
├── paper/
│   ├── results/
│   │   ├── emulation_mode.txt      ← which CXL/GPU fallback used
│   │   ├── srsran_probe_symbol.txt ← mangled symbol name for uprobe
│   │   ├── cxl_device_info.txt     ← cxl list output
│   │   ├── calibration_check.txt   ← CPU LDPC baseline vs target
│   │   ├── baseline_latency.csv
│   │   ├── offload_latency.csv
│   │   ├── ebpf_overhead.csv
│   │   ├── numa_0ns.csv
│   │   ├── numa_142ns.csv
│   │   ├── numa_255ns.csv
│   │   └── breakdown.csv
│   └── figures/
│       ├── latency_cdf.pdf
│       ├── latency_breakdown.pdf
│       ├── numa_sensitivity.pdf
│       └── ebpf_overhead.pdf
├── scripts/
│   ├── 00_install_deps.sh
│   ├── 01_build_srsran.sh
│   ├── 02_setup_qemu.sh
│   ├── 03_setup_cxl_inside_vm.sh
│   ├── 04_find_probe_symbol.sh
│   ├── 05_run_calibration.sh
│   ├── 06_run_poc.sh
│   ├── 07_numa_sweep.sh
│   └── 08_generate_figures.sh
├── third_party/
│   ├── srsRAN_Project/             ← git clone here
│   └── CXLMemSim/                  ← git clone here
├── ebpf/
│   ├── vmlinux.h                   ← bpftool btf dump
│   ├── l1_intercept.bpf.c          ← CO-RE eBPF kernel program
│   ├── l1_intercept.h              ← shared structs
│   ├── l1_intercept_loader.c       ← libbpf userspace loader
│   └── Makefile
├── gpu_daemon/
│   ├── gpu_daemon.c                ← main daemon
│   ├── opencl_kernels.cl           ← LDPC/FFT OpenCL kernels
│   ├── cxl_memory.c                ← CXL shared memory manager
│   ├── cxl_memory.h
│   └── Makefile
├── measurement/
│   ├── measure.c                   ← latency measurement harness
│   ├── plot_results.py
│   └── Makefile
└── qemu/
    ├── boot.sh                     ← QEMU launch with CXL
    ├── kernel_config_fragment      ← required kernel configs
    └── setup_vm.sh                 ← in-VM setup script
```

---

## PHASE 0: ENVIRONMENT SETUP

### Script: scripts/00_install_deps.sh

```bash
#!/bin/bash
set -e
echo "[deps] Installing all dependencies..."

# Core build tools
sudo apt-get update -qq
sudo apt-get install -y \
  build-essential cmake ninja-build \
  git wget curl \
  flex bison \
  libelf-dev libssl-dev bc libncurses-dev \
  pkg-config

# eBPF tools (libbpf CO-RE approach)
sudo apt-get install -y \
  clang-14 llvm-14 \
  libbpf-dev \
  bpftool \
  linux-headers-$(uname -r) \
  linux-tools-$(uname -r) \
  linux-tools-common

# OpenCL (PoCL CPU runtime for GPU daemon)
sudo apt-get install -y \
  pocl-opencl-icd \
  ocl-icd-opencl-dev \
  opencl-headers \
  clinfo

# QEMU with CXL support
sudo apt-get install -y qemu-system-x86

# srsRAN dependencies
sudo apt-get install -y \
  libfftw3-dev \
  libboost-all-dev \
  libconfig++-dev \
  libyaml-cpp-dev \
  libgtest-dev \
  libzmq3-dev

# Measurement and plotting
sudo apt-get install -y \
  numactl \
  python3-pip \
  python3-matplotlib \
  python3-numpy \
  python3-pandas \
  python3-scipy

pip3 install matplotlib numpy pandas scipy

# Verify critical tools
echo "[verify] clinfo:"
clinfo | grep "Device Type" || echo "WARNING: No OpenCL device found"
echo "[verify] QEMU CXL:"
qemu-system-x86_64 -M q35,cxl=on,help 2>&1 | grep -i cxl | head -3
echo "[verify] bpftool:"
bpftool version
echo "[deps] Done."
```

### Script: scripts/01_build_srsran.sh

```bash
#!/bin/bash
set -e
echo "[srsran] Building srsRAN Project..."

# Clone if not present
if [ ! -d "third_party/srsRAN_Project" ]; then
  git clone https://github.com/srsran/srsRAN_Project.git \
    third_party/srsRAN_Project
fi

cd third_party/srsRAN_Project
git pull --ff-only || true  # update if already cloned

mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_TESTING=ON \
      -DENABLE_AVX2=ON \
      -DENABLE_AVX512=OFF \
      -DBUILD_TESTS=ON \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      ..

make -j$(nproc) \
  ldpc_decoder_benchmark \
  ldpc_encoder_benchmark \
  pusch_processor_benchmark

echo "[srsran] Build complete."
echo "[srsran] Binaries:"
ls -la apps/examples/phy/ldpc_decoder_benchmark \
        apps/examples/phy/pusch_processor_benchmark
```

### Script: scripts/04_find_probe_symbol.sh

```bash
#!/bin/bash
set -e
BINARY="third_party/srsRAN_Project/build/apps/examples/phy/ldpc_decoder_benchmark"

if [ ! -f "$BINARY" ]; then
  echo "ERROR: srsRAN not built. Run 01_build_srsran.sh first."
  exit 1
fi

echo "[symbol] Searching for LDPC decode symbols in srsRAN binary..."
echo "=== Demangled symbols containing 'decode' ===" | tee paper/results/srsran_probe_symbol.txt
nm --demangle "$BINARY" | grep -i "decode\|ldpc" | grep -v "^U" | \
  tee -a paper/results/srsran_probe_symbol.txt

echo ""
echo "=== Raw mangled symbols (for uprobe attachment) ===" | \
  tee -a paper/results/srsran_probe_symbol.txt
nm "$BINARY" | grep -i "decode\|ldpc" | grep -v "^U" | \
  tee -a paper/results/srsran_probe_symbol.txt

echo ""
echo "=== Function offsets for uprobe ===" | \
  tee -a paper/results/srsran_probe_symbol.txt
objdump -t "$BINARY" | grep -i "decode\|ldpc" | grep " F " | \
  tee -a paper/results/srsran_probe_symbol.txt

# Extract the most likely decode symbol offset
DECODE_OFFSET=$(objdump -t "$BINARY" | grep " F " | \
  grep -i "decode" | grep -i "ldpc\|impl" | head -1 | awk '{print $1}')

if [ -n "$DECODE_OFFSET" ]; then
  echo ""
  echo "PRIMARY PROBE OFFSET: 0x${DECODE_OFFSET}" | \
    tee -a paper/results/srsran_probe_symbol.txt
  echo "Use this offset for bpf_program__attach_uprobe()" | \
    tee -a paper/results/srsran_probe_symbol.txt
else
  echo "WARNING: Could not auto-detect offset. Manual inspection needed."
  echo "Run: objdump -d $BINARY | grep -i decode | head -40"
fi

echo "[symbol] Symbol file written to paper/results/srsran_probe_symbol.txt"
```

### Script: scripts/05_run_calibration.sh

```bash
#!/bin/bash
set -e
BENCHMARK="third_party/srsRAN_Project/build/apps/examples/phy/ldpc_decoder_benchmark"

echo "[calibrate] Running srsRAN LDPC CPU baseline calibration..."
echo "Target from Six Times to Spare (arXiv:2602.04652): ~0.71 ms for large TB"
echo ""

# Run at parameters matching Six Times to Spare conditions:
# Large TB = 270 RBs, max code rate, 20 iterations
echo "=== Large TB (BG1, 20 iterations, high rate) ===" | \
  tee paper/results/calibration_check.txt

$BENCHMARK \
  --nof_repetitions=1000 \
  --nof_codeblocks=1 \
  --bg=1 \
  --Rv=0 \
  --nof_iterations=20 \
  2>&1 | tee -a paper/results/calibration_check.txt

# Also run small TB for range
echo ""
echo "=== Small TB (BG1, 5 iterations) ===" | \
  tee -a paper/results/calibration_check.txt

$BENCHMARK \
  --nof_repetitions=1000 \
  --nof_codeblocks=1 \
  --bg=1 \
  --Rv=0 \
  --nof_iterations=5 \
  2>&1 | tee -a paper/results/calibration_check.txt

echo ""
echo "[calibrate] Check paper/results/calibration_check.txt"
echo "[calibrate] If large TB latency is NOT in range 0.3ms-2.0ms:"
echo "[calibrate]   → Your CPU is much faster/slower than DGX Spark"
echo "[calibrate]   → Note this discrepancy explicitly in the paper"
echo "[calibrate]   → Do NOT fabricate numbers to match the target"
```

---

## PHASE 1: QEMU + CXL SETUP

### Script: qemu/boot.sh

```bash
#!/bin/bash
CXL_MEM_FILE="/tmp/cxl_poc_mem"
KERNEL=${1:-"bzImage"}
DISK=${2:-"disk.img"}

# Create CXL backing file (2GB)
if [ ! -f "$CXL_MEM_FILE" ]; then
  truncate -s 2G "$CXL_MEM_FILE"
fi

exec qemu-system-x86_64 \
  -enable-kvm \
  -cpu host \
  -smp 8 \
  -m 8G,slots=8,maxmem=32G \
  -M q35,accel=kvm,cxl=on \
  \
  -kernel "$KERNEL" \
  -drive file="$DISK",if=virtio,format=qcow2 \
  -append "console=ttyS0 root=/dev/vda rw nokaslr \
           cxl_acpi.dyndbg=+p" \
  \
  -object memory-backend-file,id=cxl-mem0,\
share=on,mem-path=${CXL_MEM_FILE},\
size=2G,align=256M \
  -device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52,\
uid=0,len-window-base=0,\
window-base[0]=0x4c00000000,\
window-size[0]=0x20000000 \
  -device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 \
  -device cxl-type3,bus=rp0,memdev=cxl-mem0,\
id=cxl-pmem0,size=2G,num-LSA=1 \
  -M cxl-fmw.0.targets.0=cxl.0,\
cxl-fmw.0.size=4G,\
cxl-fmw.0.interleave-ways=1 \
  \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  \
  -nographic \
  -serial mon:stdio
```

### Script: qemu/setup_vm.sh (run INSIDE the VM)

```bash
#!/bin/bash
set -e
echo "[vm-setup] Setting up CXL device inside VM..."

# Load CXL kernel modules
modprobe cxl_acpi
modprobe cxl_pmem
modprobe cxl_port
modprobe cxl_mem
sleep 2

# Verify CXL device appeared
echo "[vm-setup] CXL device list:"
cxl list | tee /tmp/cxl_device_info.txt

# Check if device is present
if ! cxl list | grep -q "mem0\|cxl"; then
  echo "ERROR: No CXL device found. Check QEMU boot parameters."
  exit 1
fi

# Create CXL region
echo "[vm-setup] Creating CXL memory region..."
cxl create-region -d mem0 -m 0 || \
  echo "WARN: create-region may already exist"

# Online the DAX device as system RAM (NUMA node)
echo "[vm-setup] Onlining CXL memory as system RAM..."
daxctl reconfigure-device --mode=system-ram dax0.0 || \
  echo "WARN: May already be in system-ram mode"

sleep 1

# Verify new NUMA node
echo "[vm-setup] NUMA topology:"
numactl --hardware

# Find the CXL NUMA node number
CXL_NODE=$(numactl --hardware | grep "node distances" -A 10 | \
  tail -n +2 | head -2 | tail -1 | awk '{print NR}')
echo "[vm-setup] CXL NUMA node: ${CXL_NODE}"
echo "${CXL_NODE}" > /tmp/cxl_numa_node.txt

# Verify DAX device
ls -la /dev/dax* 2>/dev/null || echo "Note: No /dev/dax — using NUMA node only"

# Verify sysfs
ls /sys/bus/cxl/devices/

echo "[vm-setup] CXL device online. Ready for PoC."
```

---

## PHASE 2: eBPF INTERCEPTION (CO-RE, libbpf)

### File: ebpf/l1_intercept.h

```c
/* Shared between eBPF kernel prog and userspace loader */
#ifndef L1_INTERCEPT_H
#define L1_INTERCEPT_H

#include <stdint.h>

#define WORK_LDPC   1
#define WORK_FFT    2

/* Event: eBPF → GPU daemon via ring buffer */
struct offload_event {
    __u32 work_type;
    __u32 pid;
    __u64 input_addr;   /* userspace virtual address of input */
    __u32 input_len;    /* bytes */
    __u64 timestamp_ns;
    __u32 slot_id;      /* slot sequence number */
};

/* Completion: GPU daemon writes, L1 reads via BPF map */
struct completion {
    __u32 pid;
    __u32 work_type;
    __u64 result_addr;  /* CXL memory offset of result */
    __u64 latency_ns;
    __s32 retcode;
};

/* Stats in BPF array map (index 0) */
struct bpf_stats {
    __u64 ldpc_intercepts;
    __u64 fft_intercepts;
    __u64 offload_errors;
    __u64 total_overhead_ns;
};

#endif
```

### File: ebpf/l1_intercept.bpf.c (CO-RE)

```c
// SPDX-License-Identifier: GPL-2.0
/* CO-RE eBPF program — attaches to srsRAN ldpc_decode uprobe */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "l1_intercept.h"

/* Map 1: Ring buffer — kernel → GPU daemon */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 512 * 1024);  /* 512KB */
} ringbuf SEC(".maps");

/* Map 2: Hash — pid → completion (GPU daemon → L1) */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 256);
    __type(key, __u32);
    __type(value, struct completion);
} completions SEC(".maps");

/* Map 3: Array — global stats (index 0) */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct bpf_stats);
} stats SEC(".maps");

/* Map 4: Array — offload enable flag (index 0) */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} offload_flag SEC(".maps");

static __u64 slot_counter = 0;

/*
 * Uprobe on srsRAN LDPC decode entry.
 *
 * srsRAN C++ ldpc_decoder_impl::decode() signature:
 *   decode(span<uint8_t> output,
 *          span<const log_likelihood_ratio> input,
 *          unsigned nof_iterations)
 *
 * At uprobe entry: pt_regs holds C++ "this" in RDI, args in RSI/RDX/RCX
 * We read input buffer address and length from function arguments.
 *
 * NOTE: For C++ methods, first arg (RDI) is 'this' pointer.
 *       Second arg (RSI) is first explicit parameter (input span).
 *       A span is typically {ptr, len} — 16 bytes.
 */
SEC("uprobe/ldpc_decode")
int BPF_UPROBE(intercept_ldpc_decode)
{
    __u32 key = 0;
    __u32 *enabled = bpf_map_lookup_elem(&offload_flag, &key);
    if (!enabled || !*enabled)
        return 0;

    struct offload_event *ev = bpf_ringbuf_reserve(&ringbuf,
                                                    sizeof(*ev), 0);
    if (!ev)
        return 0;

    ev->work_type    = WORK_LDPC;
    ev->pid          = bpf_get_current_pid_tgid() >> 32;
    ev->timestamp_ns = bpf_ktime_get_ns();
    ev->slot_id      = __sync_fetch_and_add(&slot_counter, 1);

    /*
     * Try to read input span pointer from RSI register.
     * span<const log_likelihood_ratio> is {data_ptr, size}.
     * RSI holds pointer to the span struct on stack or in reg.
     */
    void *rsi = (void *)PT_REGS_PARM2_CORE(ctx);
    __u64 data_ptr = 0;
    __u64 data_len = 0;
    bpf_probe_read_user(&data_ptr, sizeof(data_ptr), rsi);
    bpf_probe_read_user(&data_len, sizeof(data_len),
                         (char *)rsi + sizeof(__u64));

    ev->input_addr = data_ptr;
    ev->input_len  = (__u32)(data_len & 0xFFFFFFFF);

    bpf_ringbuf_submit(ev, 0);

    /* Update stats */
    struct bpf_stats *s = bpf_map_lookup_elem(&stats, &key);
    if (s)
        __sync_fetch_and_add(&s->ldpc_intercepts, 1);

    return 0;
}

/*
 * Uprobe overhead measurement:
 * A second uprobe that does NOTHING except timestamp entry/exit.
 * Attach to the same function to measure pure probe cost.
 * This gives us the ~1.7µs baseline from Cloudflare benchmark.
 */
SEC("uprobe/ldpc_decode_overhead_only")
int BPF_UPROBE(measure_overhead_only)
{
    __u64 t = bpf_ktime_get_ns();
    /* Store in per-CPU array for overhead measurement */
    /* User reads this to compute: exit_ts - entry_ts = overhead */
    (void)t;
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
```

### File: ebpf/l1_intercept_loader.c

libbpf-based loader. Must:
1. Generate vmlinux.h via bpftool (in Makefile, not at runtime)
2. Open and load the BPF object file
3. Find the srsRAN binary and the decode function offset
   (read from paper/results/srsran_probe_symbol.txt)
4. Attach uprobe to the running srsRAN benchmark process OR
   to the binary path (auto-attaches when binary is executed)
5. Start ring buffer polling thread
6. On event: write to UNIX socket → GPU daemon
7. Expose CLI flags:
   --enable-offload     turn on offloading
   --disable-offload    measure eBPF overhead only (no actual offload)
   --binary PATH        path to srsRAN binary
   --pid PID            attach to running process (optional)
   --stats-interval N   print stats every N seconds

Key implementation detail for C++ symbols:
```c
/* Read the mangled symbol offset from file */
static uint64_t read_probe_offset(const char *symbol_file) {
    FILE *f = fopen(symbol_file, "r");
    if (!f) return 0;
    
    char line[256];
    uint64_t offset = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Look for "PRIMARY PROBE OFFSET: 0x..." */
        if (strstr(line, "PRIMARY PROBE OFFSET:")) {
            sscanf(line, "PRIMARY PROBE OFFSET: 0x%lx", &offset);
            break;
        }
    }
    fclose(f);
    return offset;
}

/* Attach uprobe using file offset (works for mangled C++ symbols) */
static struct bpf_link *attach_uprobe_by_offset(
    struct bpf_program *prog,
    const char *binary,
    uint64_t offset)
{
    return bpf_program__attach_uprobe(
        prog,
        false,      /* not uretprobe */
        -1,         /* all PIDs */
        binary,
        offset
    );
}
```

---

## PHASE 3: GPU DAEMON (OpenCL via PoCL)

### File: gpu_daemon/opencl_kernels.cl

```opencl
/* Real OpenCL kernels for LDPC/FFT compute.
 * Run via PoCL on CPU (drop-in for real GPU).
 * These are simplified but realistic compute kernels.
 */

/*
 * LDPC min-sum check node update kernel.
 * In a real GPU implementation this would be one kernel
 * per half-iteration. For the PoC we do a simplified version
 * that demonstrates the memory access pattern.
 */
__kernel void ldpc_check_node_update(
    __global const float *llr_in,   /* input LLRs from CXL */
    __global float *llr_out,        /* output LLRs to CXL */
    __global const int *parity,     /* parity check matrix (sparse) */
    const int n_check_nodes,
    const int n_var_nodes)
{
    int cn = get_global_id(0);
    if (cn >= n_check_nodes) return;
    
    /* Min-sum: find minimum magnitude and sign product */
    float min1 = 1e9f, min2 = 1e9f;
    int sign = 1;
    
    for (int i = 0; i < n_var_nodes; i++) {
        if (parity[cn * n_var_nodes + i] == 0) continue;
        float val = llr_in[i];
        float mag = fabs(val);
        sign *= (val >= 0) ? 1 : -1;
        if (mag < min1) { min2 = min1; min1 = mag; }
        else if (mag < min2) { min2 = mag; }
    }
    
    /* Write result back to output buffer (in CXL memory) */
    for (int i = 0; i < n_var_nodes; i++) {
        if (parity[cn * n_var_nodes + i] == 0) continue;
        float val = llr_in[i];
        float mag = (fabs(val) == min1) ? min2 : min1;
        int s = sign * ((val >= 0) ? 1 : -1);
        llr_out[i] = (float)s * mag * 0.75f;  /* scaled min-sum */
    }
}

/*
 * FFT butterfly kernel (radix-2 Cooley-Tukey).
 * Demonstrates FFT memory access pattern for OFDM.
 * In production: use vendor FFT library (rocFFT, cuFFT, FFTW).
 */
__kernel void fft_butterfly(
    __global float2 *data,  /* complex float in CXL memory */
    const int N,
    const int stage,
    const int dir)          /* 1=forward, -1=inverse */
{
    int tid = get_global_id(0);
    int half = N >> (stage + 1);
    if (tid >= half * (1 << stage)) return;
    
    int block = tid / half;
    int pair  = tid % half;
    int i0 = block * (half * 2) + pair;
    int i1 = i0 + half;
    
    float angle = (float)dir * -2.0f * M_PI_F *
                  (float)pair / (float)(half * 2);
    float2 w = (float2)(cos(angle), sin(angle));
    
    float2 a = data[i0];
    float2 b = data[i1];
    float2 wb = (float2)(w.x*b.x - w.y*b.y, w.x*b.y + w.y*b.x);
    
    data[i0] = a + wb;
    data[i1] = a - wb;
}
```

### File: gpu_daemon/gpu_daemon.c

The daemon must:
1. Initialize OpenCL via PoCL:
   ```c
   cl_platform_id platform;
   cl_device_id device;
   clGetPlatformIDs(1, &platform, NULL);
   /* Try GPU first, fall back to CPU (PoCL) */
   cl_int err = clGetDeviceIDs(platform,
                               CL_DEVICE_TYPE_GPU, 1,
                               &device, NULL);
   if (err != CL_SUCCESS) {
       err = clGetDeviceIDs(platform,
                            CL_DEVICE_TYPE_CPU, 1,
                            &device, NULL);
       printf("[gpu] Using CPU OpenCL device (PoCL)\n");
       fprintf(logfile, "gpu_backend: pocl-cpu\n");
   } else {
       printf("[gpu] Using GPU OpenCL device\n");
       fprintf(logfile, "gpu_backend: real-gpu\n");
   }
   ```

2. Map CXL memory region as OpenCL buffer using CL_MEM_USE_HOST_PTR:
   ```c
   /* Map CXL memory — zero copy when GPU daemon and L1 share same region */
   void *cxl_base = mmap(NULL, CXL_SIZE,
                         PROT_READ|PROT_WRITE,
                         MAP_SHARED, cxl_fd, 0);
   
   /* Create OpenCL buffer backed by CXL memory */
   cl_mem cl_input_buf = clCreateBuffer(ctx,
       CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
       INPUT_SIZE, cxl_input_ptr, &err);
   cl_mem cl_output_buf = clCreateBuffer(ctx,
       CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
       OUTPUT_SIZE, cxl_output_ptr, &err);
   ```
   Note: CL_MEM_USE_HOST_PTR is the zero-copy path.
   PoCL will use the CXL memory directly without copying.
   This is the key claim: CXL memory is visible to OpenCL kernel.

3. Receive work events from eBPF loader via UNIX socket

4. For each event:
   a. Record t_start = clock_gettime(CLOCK_MONOTONIC)
   b. Write input data to CXL input buffer
   c. Enqueue OpenCL kernel (ldpc_check_node_update)
   d. clFinish() — wait for kernel completion
   e. Record t_end
   f. Latency = t_end - t_start
   g. Signal completion via BPF map update

5. Periodically flush stats to paper/results/breakdown.csv

---

## PHASE 4: CXL SHARED MEMORY

### File: gpu_daemon/cxl_memory.c

```c
/*
 * CXL memory manager.
 * Handles the three modes:
 *   1. Real /dev/dax0.0 (QEMU CXL Type-3 fully online)
 *   2. NUMA node 1 (Pond emulation — CPU-less socket or numactl)
 *   3. Fallback: MAP_SHARED tmpfs file
 *
 * Always log which mode to paper/results/emulation_mode.txt
 */

#define CXL_SIZE        (2UL * 1024 * 1024 * 1024)
#define CXL_INPUT_OFF   0
#define CXL_INPUT_SZ    (256 * 1024 * 1024)
#define CXL_OUTPUT_OFF  (256 * 1024 * 1024)
#define CXL_OUTPUT_SZ   (256 * 1024 * 1024)
#define CXL_CTRL_OFF    (512 * 1024 * 1024)
#define CXL_CTRL_SZ     (1 * 1024 * 1024)

typedef struct {
    void    *base;
    size_t   size;
    int      mode;      /* 1=dax, 2=numa, 3=tmpfs */
    int      numa_node; /* CXL NUMA node number */
    int      fd;
} cxl_ctx_t;

int cxl_init(cxl_ctx_t *ctx, const char *emulation_mode_file) {
    FILE *log = fopen(emulation_mode_file, "a");
    
    /* Mode 1: Real CXL DAX device */
    ctx->fd = open("/dev/dax0.0", O_RDWR);
    if (ctx->fd >= 0) {
        ctx->base = mmap(NULL, CXL_SIZE,
                        PROT_READ|PROT_WRITE,
                        MAP_SHARED, ctx->fd, 0);
        if (ctx->base != MAP_FAILED) {
            ctx->mode = 1;
            if (log) fprintf(log, "cxl_mode: qemu-cxl-type3-dax\n");
            fclose(log);
            return 0;
        }
        close(ctx->fd);
    }
    
    /* Mode 2: NUMA node (Pond methodology) */
    int numa_node = -1;
    FILE *f = fopen("/tmp/cxl_numa_node.txt", "r");
    if (f) { fscanf(f, "%d", &numa_node); fclose(f); }
    
    if (numa_node > 0) {
        /* Allocate on CXL NUMA node via mbind */
        ctx->base = mmap(NULL, CXL_SIZE,
                        PROT_READ|PROT_WRITE,
                        MAP_SHARED|MAP_ANONYMOUS, -1, 0);
        if (ctx->base != MAP_FAILED) {
            unsigned long nodemask = 1UL << numa_node;
            long ret = syscall(SYS_mbind, ctx->base, CXL_SIZE,
                               MPOL_BIND, &nodemask,
                               sizeof(nodemask)*8 + 1, 0);
            if (ret == 0) {
                ctx->mode = 2;
                ctx->numa_node = numa_node;
                if (log) {
                    fprintf(log, "cxl_mode: numa-emulation-node%d\n",
                            numa_node);
                    fprintf(log, "paper_note: CXL emulated via CPU-less "
                            "NUMA node (Pond ASPLOS23 methodology)\n");
                }
                fclose(log);
                return 0;
            }
            munmap(ctx->base, CXL_SIZE);
        }
    }
    
    /* Mode 3: MAP_SHARED tmpfs fallback */
    ctx->fd = open("/tmp/cxl_shm", O_RDWR|O_CREAT, 0600);
    ftruncate(ctx->fd, CXL_SIZE);
    ctx->base = mmap(NULL, CXL_SIZE,
                    PROT_READ|PROT_WRITE,
                    MAP_SHARED, ctx->fd, 0);
    ctx->mode = 3;
    if (log) {
        fprintf(log, "cxl_mode: tmpfs-shared-memory-fallback\n");
        fprintf(log, "paper_note: No CXL device or NUMA node available. "
                "Shared memory only. Latency not representative of CXL.\n");
    }
    fclose(log);
    return (ctx->base == MAP_FAILED) ? -1 : 0;
}
```

---

## PHASE 5: MEASUREMENT

### Script: scripts/06_run_poc.sh

```bash
#!/bin/bash
set -e
RESULTS="paper/results"
SRSRAN_BIN="third_party/srsRAN_Project/build/apps/examples/phy"

echo "=== CXL + eBPF + srsRAN PoC ==="

# 1. Verify setup
test -f "$RESULTS/calibration_check.txt" || \
  { echo "Run calibration first: scripts/05_run_calibration.sh"; exit 1; }
test -f "$RESULTS/srsran_probe_symbol.txt" || \
  { echo "Run symbol finder first: scripts/04_find_probe_symbol.sh"; exit 1; }

# 2. Start GPU daemon
echo "[poc] Starting OpenCL GPU daemon..."
./gpu_daemon/gpu_daemon \
  --cxl-path /dev/dax0.0 \
  --emulation-log "$RESULTS/emulation_mode.txt" \
  --socket /tmp/gpu_daemon.sock \
  --stats-output "$RESULTS/breakdown.csv" &
GPU_PID=$!
sleep 2

# 3. Load eBPF programs
echo "[poc] Loading eBPF uprobe..."
./ebpf/l1_intercept_loader \
  --binary "$SRSRAN_BIN/ldpc_decoder_benchmark" \
  --symbol-file "$RESULTS/srsran_probe_symbol.txt" \
  --socket /tmp/gpu_daemon.sock \
  --disable-offload &  # First run: overhead only
EBPF_PID=$!
sleep 1

# 4. Measure eBPF overhead only (no offload, just intercept)
echo "[poc] Measuring eBPF overhead only..."
"$SRSRAN_BIN/ldpc_decoder_benchmark" \
  --nof_repetitions=10000 \
  --bg=1 \
  --nof_iterations=20 \
  2>&1 | tee "$RESULTS/ebpf_overhead.csv"

# 5. Run baseline (no eBPF at all)
echo "[poc] Running CPU baseline (no eBPF)..."
kill $EBPF_PID 2>/dev/null; sleep 1
./measurement/measure \
  --mode baseline \
  --binary "$SRSRAN_BIN/ldpc_decoder_benchmark" \
  --slots 10000 \
  --output "$RESULTS/baseline_latency.csv"

# 6. Run with full offload
echo "[poc] Running full offload (eBPF + CXL + OpenCL)..."
./ebpf/l1_intercept_loader \
  --binary "$SRSRAN_BIN/ldpc_decoder_benchmark" \
  --symbol-file "$RESULTS/srsran_probe_symbol.txt" \
  --socket /tmp/gpu_daemon.sock \
  --enable-offload &
EBPF_PID=$!
sleep 1

./measurement/measure \
  --mode offload \
  --binary "$SRSRAN_BIN/ldpc_decoder_benchmark" \
  --slots 10000 \
  --output "$RESULTS/offload_latency.csv"

kill $EBPF_PID $GPU_PID 2>/dev/null

echo "[poc] Results in $RESULTS/"
```

### Script: scripts/07_numa_sweep.sh

```bash
#!/bin/bash
set -e
RESULTS="paper/results"
SRSRAN="third_party/srsRAN_Project/build/apps/examples/phy/ldpc_decoder_benchmark"

echo "=== NUMA Latency Sweep (Pond ASPLOS'23 methodology) ==="
echo "Emulates CXL latency: 0ns / 142ns / 255ns"

NUMA_NODES=$(numactl --hardware 2>/dev/null | \
  grep "^available" | awk '{print $2}')
echo "NUMA nodes available: ${NUMA_NODES}"

# Case 1: Local memory (0ns CXL latency)
echo "[sweep] 0ns — local DRAM..."
numactl --cpunodebind=0 --membind=0 \
  ./measurement/measure \
    --mode offload \
    --binary "$SRSRAN" \
    --slots 10000 \
    --label "local-dram-0ns" \
    --output "$RESULTS/numa_0ns.csv"

if [ "${NUMA_NODES:-1}" -lt 2 ]; then
  echo "WARNING: Only 1 NUMA node — cannot emulate CXL latency"
  echo "142ns and 255ns cases will use same node (no latency diff)"
  echo "cxl_latency_emulation: single-numa-no-latency" >> \
    "$RESULTS/emulation_mode.txt"
else
  # Case 2: Remote NUMA node (142ns — Pond measured CXL latency)
  echo "[sweep] 142ns — Pond CXL emulation (remote NUMA)..."
  numactl --cpunodebind=0 --membind=1 \
    ./measurement/measure \
      --mode offload \
      --binary "$SRSRAN" \
      --slots 10000 \
      --label "cxl-pond-142ns" \
      --output "$RESULTS/numa_142ns.csv"
fi

# Case 3: CXLMemSim injection (255ns) or remote NUMA fallback
if command -v ./third_party/CXLMemSim/build/cxlmemsim &>/dev/null; then
  echo "[sweep] 255ns — CXLMemSim latency injection..."
  ./third_party/CXLMemSim/build/cxlmemsim --latency=255 -- \
    numactl --cpunodebind=0 --membind=1 \
    ./measurement/measure \
      --mode offload \
      --binary "$SRSRAN" \
      --slots 10000 \
      --label "cxlmemsim-255ns" \
      --output "$RESULTS/numa_255ns.csv"
  echo "cxl_255ns_method: cxlmemsim" >> "$RESULTS/emulation_mode.txt"
else
  echo "[sweep] 255ns — CXLMemSim not available, using NUMA fallback..."
  numactl --cpunodebind=0 --membind=1 \
    ./measurement/measure \
      --mode offload \
      --binary "$SRSRAN" \
      --slots 10000 \
      --label "numa-approx-255ns" \
      --output "$RESULTS/numa_255ns.csv"
  echo "cxl_255ns_method: numa-approx (CXLMemSim not installed)" >> \
    "$RESULTS/emulation_mode.txt"
fi

echo "[sweep] Done. Check $RESULTS/numa_*.csv"
```

### File: measurement/plot_results.py

Generate IEEE two-column quality figures. Must produce:

```python
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import os

# IEEE two-column style
plt.rcParams.update({
    'figure.figsize':   (3.5, 2.6),
    'font.size':        8,
    'font.family':      'serif',
    'axes.labelsize':   8,
    'xtick.labelsize':  7,
    'ytick.labelsize':  7,
    'legend.fontsize':  7,
    'lines.linewidth':  1.2,
    'figure.dpi':       300,
    'savefig.dpi':      300,
    'savefig.bbox':     'tight',
    'savefig.pad_inches': 0.02,
})

RESULTS = "paper/results"
FIGURES = "paper/figures"
os.makedirs(FIGURES, exist_ok=True)

# ── Figure 1: Latency CDF (key figure) ──────────────────────
def plot_latency_cdf():
    fig, ax = plt.subplots()
    
    baseline = pd.read_csv(f"{RESULTS}/baseline_latency.csv")
    offload  = pd.read_csv(f"{RESULTS}/offload_latency.csv")
    
    def cdf(data):
        s = np.sort(data)
        p = np.arange(1, len(s)+1) / len(s)
        return s, p
    
    x, y = cdf(baseline['latency_us'])
    ax.plot(x, y, '-',  label='CPU only', color='#444')
    
    x, y = cdf(offload['latency_us'])
    ax.plot(x, y, '--', label='eBPF+CXL+OpenCL', color='#1f77b4')
    
    # 0.5ms deadline
    ax.axvline(500, color='red', linewidth=0.8,
               linestyle=':', label='0.5 ms slot budget')
    
    # Six Times to Spare baseline annotation
    ax.axvline(710, color='gray', linewidth=0.6,
               linestyle='--', alpha=0.5)
    ax.text(715, 0.3, '710µs\n(CPU ref,\nSix×Spare)',
            fontsize=5.5, color='gray', va='center')
    
    ax.set_xlabel('Latency (µs)')
    ax.set_ylabel('CDF')
    ax.set_xlim(0, max(900, offload['latency_us'].max() * 1.05))
    ax.legend(loc='lower right')
    ax.grid(True, alpha=0.3, linewidth=0.5)
    
    fig.savefig(f"{FIGURES}/latency_cdf.pdf")
    fig.savefig(f"{FIGURES}/latency_cdf.png")
    plt.close(fig)
    print(f"Saved: {FIGURES}/latency_cdf.pdf")

# ── Figure 2: Latency breakdown ──────────────────────────────
def plot_breakdown():
    if not os.path.exists(f"{RESULTS}/breakdown.csv"):
        print("SKIP: breakdown.csv not found")
        return
    
    df = pd.read_csv(f"{RESULTS}/breakdown.csv")
    components = ['t_ebpf_us', 't_ringbuf_us', 't_daemon_sched_us',
                  't_compute_us', 't_signal_us']
    labels = ['eBPF probe', 'Ring buf', 'Daemon sched',
              'OpenCL compute', 'Signal']
    colors = ['#aec7e8', '#1f77b4', '#ffbb78', '#ff7f0e', '#98df8a']
    
    fig, ax = plt.subplots()
    
    bottom = np.zeros(1)
    means = [df[c].mean() if c in df.columns else 0 for c in components]
    
    for label, mean, color in zip(labels, means, colors):
        ax.bar(['Offload path'], [mean], bottom=bottom,
               label=f'{label} ({mean:.1f}µs)', color=color)
        bottom += mean
    
    ax.set_ylabel('Latency (µs)')
    ax.legend(bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=6)
    ax.grid(True, axis='y', alpha=0.3)
    
    fig.savefig(f"{FIGURES}/latency_breakdown.pdf")
    plt.close(fig)
    print(f"Saved: {FIGURES}/latency_breakdown.pdf")

# ── Figure 3: NUMA latency sensitivity ───────────────────────
def plot_numa_sensitivity():
    files = {
        '0 ns\n(local)':    f"{RESULTS}/numa_0ns.csv",
        '142 ns\n(Pond CXL)': f"{RESULTS}/numa_142ns.csv",
        '255 ns\n(Pond max)': f"{RESULTS}/numa_255ns.csv",
    }
    
    labels, miss_rates = [], []
    for label, fpath in files.items():
        if not os.path.exists(fpath):
            print(f"SKIP: {fpath} not found")
            continue
        df = pd.read_csv(fpath)
        if 'latency_us' not in df.columns:
            continue
        miss = (df['latency_us'] > 500).mean() * 100
        labels.append(label)
        miss_rates.append(miss)
    
    if not labels:
        print("SKIP: No NUMA sweep files found")
        return
    
    fig, ax = plt.subplots()
    bars = ax.bar(labels, miss_rates, color='#ff7f0e', width=0.5)
    
    # Label bars
    for bar, val in zip(bars, miss_rates):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.5,
                f'{val:.1f}%', ha='center', va='bottom', fontsize=7)
    
    ax.set_ylabel('Slot deadline miss rate (%)')
    ax.set_title('CXL latency vs. 5G NR slot compliance', fontsize=8)
    ax.set_ylim(0, max(miss_rates + [5]) * 1.2)
    ax.grid(True, axis='y', alpha=0.3, linewidth=0.5)
    
    # Annotation: Pond reference
    ax.text(0.98, 0.95,
            'Latency emulation: Pond (ASPLOS\'23)\n'
            'methodology — CPU-less NUMA node',
            transform=ax.transAxes, ha='right', va='top',
            fontsize=5.5, color='gray',
            bbox=dict(boxstyle='round', facecolor='white', alpha=0.7))
    
    fig.savefig(f"{FIGURES}/numa_sensitivity.pdf")
    fig.savefig(f"{FIGURES}/numa_sensitivity.png")
    plt.close(fig)
    print(f"Saved: {FIGURES}/numa_sensitivity.pdf")

# ── Figure 4: eBPF overhead distribution ─────────────────────
def plot_ebpf_overhead():
    if not os.path.exists(f"{RESULTS}/ebpf_overhead.csv"):
        return
    
    df = pd.read_csv(f"{RESULTS}/ebpf_overhead.csv")
    if 'overhead_ns' not in df.columns:
        return
    
    fig, ax = plt.subplots()
    ax.hist(df['overhead_ns'], bins=50, color='#1f77b4',
            edgecolor='none', alpha=0.8)
    ax.axvline(1670, color='red', linewidth=0.8, linestyle='--',
               label='Cloudflare ref: 1670 ns')
    ax.axvline(df['overhead_ns'].mean(), color='orange',
               linewidth=0.8, linestyle='-',
               label=f"Measured: {df['overhead_ns'].mean():.0f} ns")
    ax.set_xlabel('eBPF uprobe overhead (ns)')
    ax.set_ylabel('Count')
    ax.legend()
    ax.grid(True, alpha=0.3, linewidth=0.5)
    
    fig.savefig(f"{FIGURES}/ebpf_overhead.pdf")
    plt.close(fig)

# Run all
plot_latency_cdf()
plot_breakdown()
plot_numa_sensitivity()
plot_ebpf_overhead()
print("All figures generated.")
```

---

## PHASE 6: PAPER NOTES GENERATION

### Script: scripts/08_generate_figures.sh

```bash
#!/bin/bash
python3 measurement/plot_results.py

# Auto-generate paper notes
python3 - << 'PYTHON'
import pandas as pd, json, os
from datetime import datetime

r = "paper/results"
out = f"{r}/paper_notes.md"

def safe_read(path, col):
    try:
        df = pd.read_csv(path)
        return df[col].describe() if col in df else None
    except:
        return None

bl = safe_read(f"{r}/baseline_latency.csv", "latency_us")
of = safe_read(f"{r}/offload_latency.csv",  "latency_us")

lines = [
    f"# PoC Measurement Notes",
    f"Generated: {datetime.now().isoformat()}",
    f"",
    f"## Emulation Mode",
]

if os.path.exists(f"{r}/emulation_mode.txt"):
    with open(f"{r}/emulation_mode.txt") as f:
        lines += [f"```", f.read().strip(), f"```"]

lines += ["", "## CPU Baseline (no eBPF)"]
if bl is not None:
    lines += [
        f"- Mean:   {bl['mean']:.1f} µs",
        f"- p50:    {bl['50%']:.1f} µs",
        f"- p95:    {bl['75%']:.1f} µs",
        f"- Deadline miss (>500µs): "
        f"{(pd.read_csv(f'{r}/baseline_latency.csv')['latency_us']>500).mean()*100:.1f}%",
    ]

lines += ["", "## Offload Path (eBPF + CXL + OpenCL)"]
if of is not None:
    lines += [
        f"- Mean:   {of['mean']:.1f} µs",
        f"- p50:    {of['50%']:.1f} µs",
        f"- Deadline miss (>500µs): "
        f"{(pd.read_csv(f'{r}/offload_latency.csv')['latency_us']>500).mean()*100:.1f}%",
    ]

lines += [
    "",
    "## Calibration vs Six Times to Spare",
    "Target: ~710 µs for large TB (20 iter, CPU, arXiv:2602.04652)",
    "See: paper/results/calibration_check.txt",
    "",
    "## Novelty Evidence",
    "- Lines of srsRAN code changed: 0 (transparent interception)",
    "- eBPF attachment: CPU-side uprobe on srsRAN binary",
    "- NOT inside GPU driver (differentiates from eGPU HCDS'25)",
    "- CXL memory: shared via /dev/dax0.0 or NUMA node",
    "- OpenCL GPU daemon: PoCL CPU backend (drop-in for real GPU)",
    "",
    "## Prior Art Differentiation",
    "- AtlasRAN: modified OAI + CUDA. We: unmodified srsRAN + open.",
    "- eGPU: bpftime + GPU-side. We: kernel uprobe + CPU-side.",
    "- UDON: near-memory CPU, no eBPF. We: GPU offload + eBPF.",
    "- Six Times to Spare: CUDA only. We: OpenCL + CXL shared mem.",
]

with open(out, "w") as f:
    f.write("\n".join(lines))
print(f"Notes written to {out}")
PYTHON
```

---

## AGENT EXECUTION LOOP

```
Stage 0: Install deps        → scripts/00_install_deps.sh
Stage 1: Build srsRAN        → scripts/01_build_srsran.sh
Stage 2: Boot QEMU + CXL     → qemu/boot.sh + qemu/setup_vm.sh
Stage 3: Find probe symbol   → scripts/04_find_probe_symbol.sh
Stage 4: Calibration         → scripts/05_run_calibration.sh
         GO/NO-GO: large TB latency in range 0.2ms–3.0ms?
         If not: check AVX2 flags, LDPC iteration count, CPU frequency
Stage 5: Build eBPF + daemon → make -C ebpf && make -C gpu_daemon
Stage 6: Run PoC             → scripts/06_run_poc.sh
         GO/NO-GO: offload produces correct LDPC output?
Stage 7: NUMA sweep          → scripts/07_numa_sweep.sh
Stage 8: Generate figures    → scripts/08_generate_figures.sh

If any stage fails: debug, fix, re-run. Do NOT skip stages.
Do NOT generate synthetic data to fill missing CSV files.
Log every failure and fix to build.log.
```

---

## SUCCESS CRITERIA

### Non-negotiable (paper cannot be submitted without these)

```
1. srsRAN binary is real upstream srsRAN Project code
   NOT a synthetic stub
   Verify: git log in third_party/srsRAN_Project

2. GPU daemon uses real OpenCL API (clCreateBuffer, clEnqueueNDRange)
   NOT bare C functions
   Verify: clinfo shows device before daemon starts

3. CXL memory is exercised via real kernel path
   Verify: dmesg shows cxl driver messages OR
           NUMA node count increased after setup

4. eBPF uprobe attaches to REAL srsRAN binary
   Verify: bpftool prog show lists the uprobe
           dmesg shows no BPF verifier errors

5. Calibration check passes:
   srsRAN large TB latency: 0.2ms–3.0ms range
   If outside range: explain in paper (different CPU)
   Never falsify to match 0.71ms target

6. All three NUMA sweep files exist:
   paper/results/numa_0ns.csv
   paper/results/numa_142ns.csv
   paper/results/numa_255ns.csv

7. emulation_mode.txt records actual mode used
   (qemu-cxl-type3-dax, numa-emulation, or tmpfs-fallback)

8. Zero lines of srsRAN source modified
   Verify: git diff in third_party/srsRAN_Project is empty
```

### Fallback rules (only if above cannot be achieved)

```
If srsRAN build fails:       → OAI nr-softmodem --rfsim --phy-test
                               (also real open source)
If PoCL not available:       → Document, use bare C daemon
                               Note in emulation_mode.txt
If CXL device not found:     → NUMA mode (Pond methodology)
                               Note in emulation_mode.txt
If NUMA not available:       → tmpfs shared memory
                               Add prominent caveat in paper notes
```

---

## WHAT PROF. BASU WILL CHECK

When you walk into IISc with this PoC:

```
He will ask:      "Is this real srsRAN code?"
You can say:      "Yes — git clone from srsRAN_Project main,
                   no modifications, verified by git diff."

He will ask:      "What GPU runtime?"
You can say:      "PoCL OpenCL on CPU. Same OpenCL API as AMD ROCm
                   or NVIDIA OpenCL. Real GPU is a drop-in swap."

He will ask:      "How do you justify CXL emulation?"
You can say:      "Pond methodology (ASPLOS'23). CPU-less NUMA node
                   as CXL proxy. 142ns = Pond's measured CXL latency.
                   Same method accepted at ASPLOS, ATC, EuroSys."

He will ask:      "Why not real hardware?"
You can say:      "That's why I'm here. IISc HACC has MI210.
                   This architecture proof shows the approach works.
                   Real hardware validates the performance numbers."

He will NOT accept: custom LDPC implementations, fake latency
                    numbers, skipped NUMA sweep, or undocumented
                    emulation modes.
```