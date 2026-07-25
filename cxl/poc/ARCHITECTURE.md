# CXL RAN PoC — Architecture

## Overview

This PoC demonstrates intercepting LLR (Log-Likelihood Ratio) data produced by an srsRAN 5G NR LDPC decoder using a kernel BPF uprobe, staging it through a CXL-backed memory region, and reading it back via OpenCL. The goal is to validate the viability of CXL memory as a shared, low-latency offload staging buffer between a RAN baseband pipeline and a heterogeneous accelerator.

---

## System Layers

```
┌─────────────────────────────────────────────────────────────┐
│  LOCAL MACHINE (WSL2)                                       │
│  /root/linux_env/cxl/poc/  ← source repo                   │
└────────────────────┬────────────────────────────────────────┘
                     │ ssh (id_ed25519)
┌────────────────────▼────────────────────────────────────────┐
│  GCP HOST  n2-standard-4, Ubuntu 22.04                      │
│  34.131.224.105  (asia-south2-a)                            │
│  QEMU process + disk image                                  │
└────────────────────┬────────────────────────────────────────┘
                     │ ssh (vm_key, port 2222)
┌────────────────────▼────────────────────────────────────────┐
│  QEMU CXL VM  kernel 6.8.0-124-generic                      │
│                                                             │
│   NUMA node 0: 4 GB DRAM                                    │
│   NUMA node 1: 1920 MB CXL  ← persistent-memdev, pmem=off  │
│                                daxctl system-ram            │
│                                                             │
│   [srsRAN LDPC benchmark]  ←─── uprobe @ +0x2fef0          │
│   [ldpc_uprobe_loader]      ←─── kernel BPF loader         │
│   [PoCL OpenCL runtime]     ←─── USE_HOST_PTR              │
└─────────────────────────────────────────────────────────────┘
```

---

## Component Map

```
┌──────────────────────────────────────────────────────────────────────┐
│ QEMU VM (root process space)                                         │
│                                                                      │
│  ┌─────────────────────────┐                                         │
│  │  ldpc_uprobe_loader     │  Phase 4 orchestrator (C, userspace)    │
│  │                         │                                         │
│  │  1. bpf_object__open()  │──► ldpc_llr_mover.bpf.o                │
│  │  2. bpf_object__load()  │──► kernel BPF verifier                  │
│  │  3. attach_uprobe()     │──► perf_event on ldpc_decoder_benchmark │
│  │  4. fork() + execv()    │──► ldpc_decoder_benchmark child proc    │
│  │  5. waitpid()           │                                         │
│  │  6. map_lookup_elem()   │◄── BPF array map: llr_region            │
│  │  7. write e2e_gcp.csv   │                                         │
│  └──────────┬──────────────┘                                         │
│             │ fork/execv                                             │
│  ┌──────────▼──────────────┐                                         │
│  │  ldpc_decoder_benchmark │  srsRAN binary (PIE ELF)                │
│  │  (child process)        │                                         │
│  │                         │  decode(bit_buffer&,                    │
│  │  ldpc_decoder_impl      │        span<llr>,   ← rdx=ptr, rcx=len │
│  │    ::decode()           │        crc*,                            │
│  │    @ offset 0x2fef0  ◄──┼── kernel uprobe fires here             │
│  └─────────────────────────┘                                         │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────┐        │
│  │  KERNEL BPF subsystem                                    │        │
│  │                                                          │        │
│  │  ldpc_llr_mover.bpf.o  [uprobe/ldpc_decode]             │        │
│  │                                                          │        │
│  │  on fire:                                                │        │
│  │    llr_ptr = PT_REGS_PARM3(ctx)  ← rdx = span.data      │        │
│  │    llr_len = PT_REGS_PARM4(ctx)  ← rcx = span.size      │        │
│  │    bpf_probe_read_user(slot, len, llr_ptr)               │        │
│  │    atomic_add(meta_seq, 1)                               │        │
│  │    atomic_add(stats.hits, 1)                             │        │
│  │                                                          │        │
│  │  BPF Maps:                                               │        │
│  │  ┌─────────────────────────────┐                         │        │
│  │  │ llr_region  ARRAY[1]        │  12,304 bytes/slot      │        │
│  │  │   [0..12287] : LLR payload  │  int8_t elements        │        │
│  │  │   [12288]    : uint32 len   │                         │        │
│  │  │   [12292]    : uint32 seq   │                         │        │
│  │  └─────────────────────────────┘                         │        │
│  │  ┌─────────────────────────────┐                         │        │
│  │  │ stats       ARRAY[1]        │  16 bytes               │        │
│  │  │   hits       : uint64       │                         │        │
│  │  │   bytes_total: uint64       │                         │        │
│  │  └─────────────────────────────┘                         │        │
│  └──────────────────────────────────────────────────────────┘        │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────┐        │
│  │  Phase 2: CXL + OpenCL Sentinel (standalone test)        │        │
│  │                                                          │        │
│  │  numa_alloc_onnode(64 KB, node=1)                        │        │
│  │    ↓ allocates on CXL NUMA node 1                        │        │
│  │  CPU write: sentinel @ +8                                │        │
│  │  CPU read-back: ~34,329 ns latency                       │        │
│  │  clCreateBuffer(CL_MEM_USE_HOST_PTR, cxl_buf)           │        │
│  │  clEnqueueMapBuffer → read sentinel → PASS               │        │
│  └──────────────────────────────────────────────────────────┘        │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────┐        │
│  │  CXL NUMA node 1  (QEMU emulated)                        │        │
│  │                                                          │        │
│  │  QEMU: -object memory-backend-file,                      │        │
│  │         size=2G,mem-path=/dev/shm/cxl0,                  │        │
│  │         pmem=off,share=on                                │        │
│  │  ACPI CEDT marks window as Persistent Memory             │        │
│  │  → pages get Write-Combining cache attribute             │        │
│  │  → page fault cost ~100 ms/page if unfaulted             │        │
│  │  → safe buffer ceiling: 64 KB (pre-faulted)              │        │
│  └──────────────────────────────────────────────────────────┘        │
└──────────────────────────────────────────────────────────────────────┘
```

---

## Data Flow

```
srsRAN ldpc_decoder_impl::decode()
  │
  │  called ~4080 times per benchmark run
  │  args: (this, bit_buffer&, span<llr>, crc*, cfg&)
  │        rdi    rsi           rdx  rcx   r8    r9
  │
  ▼
[kernel uprobe @ ELF offset 0x2fef0]
  │
  │  BPF program runs in process context of ldpc_decoder_benchmark
  │  reads rdx (span.data pointer) and rcx (span.size)
  │  bpf_probe_read_user copies LLR bytes into kernel BPF array map
  │
  ▼
BPF map: llr_region [slot 0]
  │  payload: up to 12,288 int8_t LLR values
  │  metadata: len (uint32) + seq counter (uint32, atomic)
  │
  ▼
ldpc_uprobe_loader (after waitpid)
  │  bpf_map_lookup_elem(llr_fd, &key=0, region)
  │  reads len, seq, first 8 bytes for verification
  │
  ▼
e2e_gcp.csv
  uprobe_hits, llr_bytes_total, last_llr_len, last_seq, wall_s


Phase 2 (separate verification path):
  │
  numa_alloc_onnode(65536, node=1)  →  CXL NUMA node 1
  │  CPU write sentinel @ offset 8
  │
  ▼
CXL buffer @ node 1
  │  clCreateBuffer(CL_MEM_USE_HOST_PTR, cxl_buf, 65536)
  │
  ▼
PoCL OpenCL (pthread backend)
  clEnqueueMapBuffer → sentinel readback → PASS
```

---

## Key Design Decisions

### 1. Kernel BPF uprobe vs bpftime userspace uprobe
bpftime (userspace BPF via LD_PRELOAD) was built and explored but not used for the E2E run. Kernel uprobe (root + kernel 6.8) is simpler, requires no LD_PRELOAD in the target, and works for all child processes via `pid=-1` attachment.

### 2. `numa_alloc_onnode` vs `memfd + mmap + mbind`
The `memfd + mmap + mbind` approach allocates correctly but triggers the WC-mapped page fault problem at any size > ~256 KB (each 4 KB page costs ~100 ms through QEMU's ACPI CXL emulation). `numa_alloc_onnode` hits the same physical memory but uses a smaller, pre-faulted window (64 KB) that completes in microseconds.

### 3. bpftime bundled libbpf vs system libbpf
System libbpf on Ubuntu 22.04 is v0.5.0. It fails with `failed to find valid kernel BTF` when loading BPF objects compiled with `-g` (which embeds BTF for CO-RE). bpftime vendors a newer libbpf in its build tree (`build/libbpf/libbpf/libbpf.a`), which handles BTF correctly.

### 4. PARM3/PARM4 register mapping
`ldpc_decoder_impl::decode(bit_buffer&, span<llr>, ...)` under x86-64 System V ABI:
- `span<llr>` is a 16-byte aggregate `{T* data; size_t size}` passed by value
- Splits into two integer registers: `rdx` = data pointer (PARM3), `rcx` = element count (PARM4)
- Initial implementation incorrectly used PARM2/PARM3; verified via `objdump` of the function prologue

### 5. BPF map: single-slot ARRAY
A single `BPF_MAP_TYPE_ARRAY` slot captures the most-recent LLR snapshot. Each uprobe fire overwrites the previous slot and bumps a sequence counter. This avoids ring-buffer complexity at the cost of capturing only the last sample per map read. The loader reads once after the full benchmark run.

---

## File Layout

```
cxl/poc/
├── phase2_cxl_ocl_sentinel.c   Phase 2: CXL alloc + OCL sentinel (standalone)
├── ldpc_llr_mover.bpf.c        Phase 3: kernel BPF uprobe program
├── ldpc_llr_mover.bpf.o        compiled BPF object (in VM)
├── ldpc_llr_mover.skel.h       bpftool skeleton (in VM)
├── vmlinux.h                   kernel type definitions from BTF (in VM)
├── ldpc_uprobe_loader.c        Phase 4: uprobe loader + map reader
├── e2e_runner.c                Phase 4 alt: bpftime-based controller
├── Makefile                    builds all of the above
├── e2e_gcp.csv                 Phase 4 results
├── RESULTS_SUMMARY.md          measured results + notes
├── ARCHITECTURE.md             this file
└── DEPS.md                     dependency list + bootstrap steps

cxl/ops/gcp-cxl-lab/scripts/
├── launch_vm.sh                launch QEMU VM on GCP host
├── vm_cxl_setup.sh             idempotent CXL NUMA setup (run after each boot)
└── vm_install_ndctl80.sh       build ndctl v80 from source inside VM
```

---

## Measured Results

| Metric | Value | Notes |
|--------|-------|-------|
| CXL node 1 | 1920 MB | QEMU persistent-memdev |
| CXL read latency | 34,329 ns | CPU read via `numa_alloc_onnode` |
| CXL write BW (64 KB warm) | measured | WC-mapped; cold = ~0 GB/s |
| CXL write BW (32 MB cold) | ~0 GB/s / 160k× slowdown | WC page-fault bottleneck |
| OCL sentinel readback | PASS | `CL_MEM_USE_HOST_PTR` on CXL buf |
| uprobe fires (20 reps) | 4,080 | all BG1+BG2 lifting sizes |
| LLR bytes intercepted | 12,204,320 | ~11.6 MB per run |
| Last LLR slot size | 12,288 bytes | BG1 LS=384 max codeword |
| PRIMARY_CONFIG | 11,703 µs/slot = 23.4× | fixed anchor |
