# Droplet Results — Zero-Copy CXL RAN Offload PoC

**Environment**: DigitalOcean BLR1, s-4vcpu-8gb (Intel Haswell, 4 vCPU, 8 GB DRAM).
All measurements in this folder are from this single machine. WSL2 results are NOT included.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  QEMU VM (Ubuntu 22.04, kernel 6.8.0-124-generic, q35+CXL)   │
│                                                              │
│  ┌─────────────────────────┐    ┌───────────────────────┐   │
│  │  srsRAN workload        │    │  OpenCL consumer      │   │
│  │  ldpc_decoder_benchmark │    │  (parent process)     │   │
│  │  numactl --membind=1    │    │                       │   │
│  │  (LDPC decode on CXL)   │    │  CL_MEM_USE_HOST_PTR  │   │
│  └──────────┬──────────────┘    └────────┬──────────────┘   │
│             │                            │                   │
│    kernel uprobe                 numa_alloc_onnode           │
│    ldpc_decoder_impl::decode     (CXL node 1)                │
│    (bpftime: extract LLR ptr)         │                      │
│             └──────────► CXL NUMA NODE 1 ◄───────────────── │
│                          1920 MB, distance=20                │
│                          /dev/dax0.0 → system-ram mode       │
│                          (pxb-cxl → cxl-rp → cxl-type3)     │
└──────────────────────────────────────────────────────────────┘
```

**Zero-copy path**: srsRAN allocates LLR on CXL node 1 (numactl --membind=1).
bpftime uprobe captures the LLR buffer pointer at `ldpc_decoder_impl::decode` entry.
OpenCL consumer maps the SAME physical CXL pages via `CL_MEM_USE_HOST_PTR` — no copy.
OpenCL kernel reads LLR from CXL and produces decoded bits.

---

## What is proven (Gates 0–3)

| Gate | What | Status | Evidence |
|------|------|--------|----------|
| Topology | CXL NUMA node 1 live | PASS | 5/5 checks: hypervisor bit=0, no cache-sync, region0, daxctl system-ram, 2 NUMA nodes |
| 0 | CXL zero-copy confirmed | PASS | `numa_alloc_onnode(node=1)` + `get_mempolicy → node=1` + `CL_MEM_USE_HOST_PTR err=0` + sentinel 0xCA7EBEEF match |
| 1 | Uprobe fires on srsRAN | PASS | 12 events for 3 reps × 4 CB variants at offset `0x35280` |
| 2 | E2E: cross-process LLR extraction + OCL | **PARTIAL PASS** | (a)(b)(c)(e) MET: uprobe fetcharg llr_ptr=0x6409d141da10; move_pages→CXL node1 (5/7 pages); /proc/mem LLR=-10,10,10; 1150 hits. (d) NOT MET: ocl_popcount=4176 (popcount, not oracle). DEV-032. |

### What is the production step not yet wired

bpftime agent extracting the LLR pointer from srsRAN's stack at uprobe entry and writing it
into the OpenCL consumer's CXL buffer. Uprobe fires (Gate 1). Skeleton compile pending.
In Gate 2 re-test (2026-06-23), LLR is extracted from the child process via /proc/mem
(real noisy LLR, e.g. LLR[0..2]=−10,+10,+10). ocl_popcount=4176; bit-exactness oracle
comparison deferred (DEV-032). Full LDPC OCL decode blocked by PoCL JIT in QEMU-TCG (DEV-028).

---

## Result 1 — Baseline decode latency (host-native, no VM)

srsRAN `ldpc_decoder_benchmark`, BG1, Z=384, I=20, AVX2, R=1000, N=24 CB/slot.

| Metric | Value |
|--------|-------|
| per-CB p50 | **487.6 µs** |
| per-slot (×24 CB) | **11,703 µs** |
| slot budget (µ=1) | 500 µs |
| overshoot | **23.4×** |

Source: `baseline_srsran_droplet.csv`  
This is the paper's PRIMARY_CONFIG fixed anchor — not re-measured.

---

## Result 2 — LDPC OCL decoder bit-correctness (host-native)

OpenCL layered min-sum LDPC decoder (`ldpc_decode.cl`) verified bit-exact against
srsRAN encoder. Zero source modifications to srsRAN. N_iter=6.

| BG | Z | Iter | Bits tested | Mismatches | Status |
|----|---|------|-------------|------------|--------|
| 1 | 384 | 6 | 84,480 | 0 | **PASS** |
| 2 | 384 | 6 | 38,400 | 0 | **PASS** |
| 1 | 256 | 6 | 56,320 | 0 | **PASS** |
| 2 | 256 | 6 | 25,600 | 0 | **PASS** |

Source: `bit_correctness_droplet.csv`

---

## Result 3 — CXL topology (QEMU VM on droplet)

```
available: 2 nodes (0-1)
node 0 cpus: 0 1 2 3    size: 3915 MB  (DRAM)
node 1 cpus: (none)     size: 1920 MB  (CXL emulated, distance=20)

CXL device: pxb-cxl → cxl-rp → cxl-type3 → persistent-memdev (/tmp/cxl_mem.img)
Region: region0 (mem0 / decoder0.0)
Mode: daxctl system-ram → NUMA node 1 online
```

5/5 topology checks PASS.

---

## Result 4 — Gate 0: Zero-copy CXL→OCL proof (QEMU VM)

```
[gate0] PROOF1 ptr=0x76139bca2000  numa_node=1  cxl_node=YES
[gate0] wrote sentinel 0xCA7EBEEF at buf[0]  (CPU-side, no clEnqueueWriteBuffer)
[gate0] PROOF2 clCreateBuffer(CL_MEM_USE_HOST_PTR) err=0
[gate0] sentinel_cpu=0xCA7EBEEF  cl_out=0xCA7EBEEF  match=YES
[gate0] GATE0 PASS: option=A  zero_copy=YES  numa_node=1
```

`CL_MEM_USE_HOST_PTR` over a NUMA node-1 page: OpenCL kernel reads the sentinel
written by the CPU without any `clEnqueueWriteBuffer` call. This is the zero-copy
proof — the CXL pages are mapped directly into the OCL device address space.

---

## Result 5 — Gate 1: Uprobe fires on srsRAN decode (QEMU VM)

```
decode_offset=0x35280
uprobe_attach_exit=0
uprobe_events: 12 hits  (3 reps × 4 CB variants)
```

Every call to `ldpc_decoder_impl::decode` in the srsRAN benchmark fires the
kernel uprobe at the correct offset. bpftime can attach at this address to extract
the LLR buffer pointer from registers/stack.

---

## Result 6 — Gate 2: E2E pipeline (QEMU VM, re-tested 2026-06-23)

Cross-process LLR extraction: fork child (benchmark), tracefs fetcharg `llr_ptr=%dx:x64`
captures `%rdx` at `ldpc_decoder_impl::decode` entry, `move_pages()` migrates child's LLR
pages to CXL node 1, `/proc/<child>/mem` bridges LLR to parent OCL buffer, OCL processes.

| Metric | Value |
|--------|-------|
| uprobe_captured_llr_ptr | `0x6409d141da10` (real, from %rdx fetcharg) |
| node_before | 0 (DRAM — DEV-030: numactl --membind=1 SIGILL) |
| node_after | **1 (CXL)** — move_pages() confirmed |
| cxl_match | **YES** |
| pages_migrated | 5/7 (DEV-031: 2 demand-paged) |
| proc_mem_ok | **YES** |
| LLR sample | `-10, 10, 10` (child's real decode input) |
| ocl_popcount | 4,176 (popcount of hard-decision output; NOT oracle comparison; crit (d) deferred DEV-032) |
| ocl_us | 668,016 µs (PoCL CPU, QEMU TCG — emulation) |
| uprobe_hits | 1,150 |
| dev030_workaround | numactl_membind1_sigill_pmem_wc |

Source: `e2e_droplet.csv` (re-test run)

**Honest label**: OCL latency dominated by PoCL+QEMU-TCG emulation (no KVM, no GPU).
DEV-030: pmem-backed CXL pages have WC cache type; numactl --membind=1 causes SIGILL.
Workaround: move_pages() from parent to migrate child's specific LLR pages to CXL.
On real hardware: workload allocates natively on CXL; bpftime replaces /proc/mem bridge.

---

## What remains for real-hardware validation

| Item | Blocker | Target |
|------|---------|--------|
| bpftime LLR extraction (cross-process) | BPF skeleton compile for srsRAN symbol | bpftool + vmlinux BTF in VM |
| Full LDPC OCL decode timing | PoCL JIT impractical on QEMU-TCG | Real GPU or bare-metal PoCL |
| CXL latency sensitivity sweep | No hardware PMU (QEMU-TCG / WSL2) | IISc HACC or bare-metal Intel |
| srsRAN gNB (not just benchmark) | rfsimulator needs netns + UE side | Full OAI stack in QEMU |

---

## Files

| File | Contents |
|------|----------|
| `e2e_droplet.csv` | Gate 2 E2E pipeline results (1000 CBs, CXL node 1) |
| `baseline_srsran_droplet.csv` | Host-native srsRAN benchmark (11,703 µs/slot) |
| `bit_correctness_droplet.csv` | OCL LDPC decoder 0-mismatch verification |
| `qemu_cxl/` | Prior QEMU CXL offload latency sweeps |
| `gates/gate_0.md` | Zero-copy proof (sentinel test) |
| `gates/gate_1.md` | Uprobe fires (12 events) |
| `gates/gate_2.md` | E2E pipeline run log |
| `gates/gate_3.md` | RESULTS_SUMMARY update log |
| `gates/DEVIATIONS.md` | DEV-025 through DEV-029 |
