# CXL RAN PoC — Consolidated Results (v2–v7)

**PRIMARY_CONFIG anchor** (fixed, never re-derived):
100 MHz, µ=1, MCS28, 273 PRBs, C=24 CB/slot, Z=384, AVX2 min-sum I=20
→ **11,703 µs/slot = 23.4× over 500 µs slot budget**
Source: `baseline_latency.csv`, droplet host-native, N=1000.

---

## v2 — WSL2 Synthetic Baseline

**Environment**: WSL2 Ubuntu 24.04, 4 vCPU, 8 GB.
**Scope**: Initial prototype — custom LDPC decoder, bare-C GPU functions, no real open-source tools.
**Status**: Superseded. Results not carried forward.
**Key finding**: Synthetic numbers rejected as not publication-grade. Motivated v3 rewrite to real tools (srsRAN, PoCL, QEMU CXL, libbpf CO-RE).

---

## v3 — WSL2 + Real Tools (srsRAN + PoCL + QEMU CXL)

**Environment**: WSL2 Ubuntu 24.04.
**Scope**: Replaced all synthetic stubs with real open-source tools.
**Status**: Superseded by v4 (OAI rfsim integration). Latency ladder CSVs in `paper/results/superseded/`.

| File | Content |
|------|---------|
| `superseded/numa_0ns_software-synthetic-wsl2.csv` | WSL2 NUMA 0 ns (software synthetic) |
| `superseded/numa_142ns_software-synthetic-wsl2.csv` | WSL2 NUMA 142 ns (software synthetic) |
| `superseded/numa_255ns_software-synthetic-wsl2.csv` | WSL2 NUMA 255 ns (software synthetic) |
| `superseded/README.md` | Explains why these are invalid |

**Key finding**: QEMU does not model CXL latency. Synthetic NUMA latency injection (Pond methodology) produces artificial numbers. Carried forward: PoCL as OpenCL backend, srsRAN as target workload.

---

## v4 — WSL2 + OAI nr-softmodem + bpftime (2026-06-15–16)

**Environment**: WSL2 Ubuntu 24.04 (4 vCPU, 8 GB) for gates 0.1–3, 5, 6. DigitalOcean BLR1 Haswell droplet for baseline measurement only.
**Target**: OAI nr-softmodem (libldpc.so), not srsRAN.
**bpftime**: ubpf JIT (`BPFTIME_VM_NAME=ubpf`, DEV-002).

### Gate 0.1 — bpftime uprobe smoke test: PASS

| Metric | Value |
|--------|-------|
| bpftime round-trip | **248.5 ns/call** |
| Kernel uprobe (baseline) | 9,941 ns/call |
| Speedup | **40× faster** |
| call_count | 1,200,000 exact |
| VM | ubpf JIT |

### Gate 0.2 — OAI rfsimulator: PASS

| Metric | Value |
|--------|-------|
| TCP ESTAB | Both sides (gnb-ns + ue-ns veth) |
| Symbol | `nrLDPC_coding_decoder @ 0xe9b30` |
| Symbol 2 | `LDPCdecoder @ 0x63110` |
| Deviations | DEV-006 (serveraddr flag), DEV-007 (UE PHY sync) |

### Gate 0.3 — CXLMemSim: FAIL (deferred)

| Reason | FAIL |
|--------|------|
| WSL2 no hardware PMU | `perf_event_open` → No such file or directory |
| Downstream | Phase 4 CXLMemSim latency sweep deferred throughout all versions |

### Gate 1 — OpenCL LDPC bit-correctness: PASS

| BG | Zc | Iter | Bits checked | Mismatches | Rate |
|----|----|------|-------------|------------|------|
| 1 | 384 | 6 | 84,480 | 0 | **0.000000** |
| 2 | 384 | 6 | 38,400 | 0 | **0.000000** |
| 1 | 256 | 6 | 56,320 | 0 | **0.000000** |
| 2 | 256 | 6 | 25,600 | 0 | **0.000000** |

Implementation: `llr_add` sticky saturation, `llr_sub` plain arithmetic. DEV-008 (runtime oracle).
Source: `bit_correctness.csv`.

### Gate 2 — bpftime uprobe on OAI libldpc.so: PASS

| Metric | Value |
|--------|-------|
| slot_calls | 5,055 |
| cb_calls | 10,090 |
| CB/slot ratio | **2.000** (C_actual=2, DEV-009) |
| OCL CBs decoded | 200 |
| DEV-010 | LLR payload in BPF map (not pointer) |
| DEV-011 | Z=224 → iLS=0 shift tables |

### Gate 3 — Sustained run + XDP NIC observation: PASS

| Metric | Value |
|--------|-------|
| slot_calls | 464,737 |
| cb_calls | 929,474 |
| CB/slot | **2.000** |
| uprobe attach | Two offsets confirmed (e9b30, 63110) |
| XDP packets | 60,000 over 1,535 ms |
| Mean inter-arrival | 25.6 µs |
| Slot rate implied | 2,204/s ≈ 2,000/s (µ=1) |
| DEV-013 | PERCPU_ARRAY for concurrent OAI thread-pool |

### Gate 5 — Ablation measurement: PASS

**N=500 slots, C_actual=2 CB/slot, Z=224, WSL2**

| Row | mean µs/slot | p50 | p95 | p99 | Source |
|-----|-------------|-----|-----|-----|--------|
| baseline (PRIMARY_CONFIG) | **11,703** | — | — | — | fixed_anchor |
| +interception_only | **2,636** | 1,900 | 6,730 | 9,216 | `ablation_raw.csv` pass=0 |
| +gpu_compute_full | **163,528** | 145,571 | 302,519 | 405,498 | `ablation_raw.csv` pass=1 |

OCL sub-component: **149,548 µs/slot** (ocl_ns).
Interception overhead dominated by 2 ms poll interval (DEV-014). Event-driven floor: 248.5 ns.

**Headline pair**:
- `23.4×` — 11,703 µs / 500 µs — fixed anchor, motivation
- `327×` — 163,528 µs / 500 µs — measured E2E, CPU OCL, WSL2

**GPU projection** (labelled projected, not measured):
2,636 + 149,548 / 6 = **27,561 µs/slot = 55.1×** (citing "Six Times to Spare" 6× GPU speedup)

Source: `latency_ladder_v2.csv`, `comparison_table.csv`, `ablation_raw.csv`.

### Gate 6 — Paper artifacts: PASS

Figures generated: `latency_cdf_v2.pdf`, `latency_breakdown_v2.pdf`, `nic_packet_timeline.pdf`, `bit_correctness_table.pdf`. RESULTS_SUMMARY.md §1–9 complete.

---

## v5 — WSL2 + SPSC Ring + CXL Topology (2026-06-20–22)

**Environment**: WSL2 for Gates 1–4; DigitalOcean BLR1 droplet for Gate 5 (QEMU CXL).
**Changes from v4**: SPSC descriptor ring, busy-poll consumer, Change A/B/C architecture.

### Gate 1 — SPSC ring + busy-poll: PASS

| Metric | Value |
|--------|-------|
| K=1e3/1e5/1e6 | All zero drops |
| poll-to-handle p50 | **17 ns** (vs v4's ~950,000 ns sleep-poll) |
| Improvement | **55,882× latency reduction** |
| CXL region | /tmp/cxl_standin.bin (placeholder) |

### Gate 2 — CXL + OCL bit-correctness: PASS

| Metric | Value |
|--------|-------|
| Sentinel (CL_MEM_USE_HOST_PTR) | 0xBE match — CONFIRMED |
| BG1/384, BG2/384, BG1/256, BG2/256 | All bit_diff_rate=0.000000 |
| DEV-015 | PoCL USE_HOST_PTR crash on large buffers → 4096-byte sentinel slices |
| DEV-016 | Kernel arg order bug — fixed |

Source: `bit_correctness_cxlpath.csv`.

### Gate 3 — bpftime RINGBUF uprobe: PASS

| Metric | Value |
|--------|-------|
| Descriptors received | 200 in 24.9 s |
| Rate | 8.0/s |
| Drops | 0 |
| Ring overflows | 0 |
| Unique OAI worker TIDs | 2 (57847, 57851) confirmed MPSC path |
| DEV-017/018/019 | Build/attach deviations |

### Gate 4 — Ablation v5: PARTIAL PASS

| Metric | Value |
|--------|-------|
| interception_only rate | 613.7 CBs/s (startup=13.6s) |
| interception p50 | 1,075 µs/CB (bpftime IPC floor, DEV-020) |
| OCL standalone (N=1000) | mean=141,628 µs/CB |
| C_actual | 2 confirmed |

Source: `latency_ladder_v2_v5.csv` (3 rows).

### Gate 5 — CXL topology on DO droplet: PARTIAL PASS

| Check | Result |
|-------|--------|
| verify_cxl_checks.sh 5/5 | **PASS** (hypervisor-bit, no-cache-sync, create-region, daxctl-system-ram, NUMA-2-nodes) |
| PROOF1 (mbind + get_mempolicy) | numa_node=1, cxl_node=YES |
| PROOF2 (host-side CL_MEM_USE_HOST_PTR) | 50 CBs decoded, CL_MEM_USE_HOST_PTR over /tmp/cxl_mem.img |
| CXLMemSim sweep | **FAIL** — DEV-022 (DO KVM no PMU passthrough) |
| /dev/dax0.0 devdax mmap | **FAIL** — DEV-023 ENXIO |
| system-ram + devdax | **FAIL** — DEV-024 mutually exclusive |

---

## v6 — DigitalOcean Droplet + QEMU CXL VM (2026-06-23)

**Environment**: DigitalOcean BLR1 Haswell droplet (s-4vcpu-8gb, no KVM). QEMU CXL VM: Ubuntu 22.04, kernel 6.8.0-124-generic, QEMU-TCG (no KVM).
**Target**: srsRAN `ldpc_decoder_benchmark` (not OAI).
**OpenCL device**: `cpu-haswell-DO-Regular` (PoCL, CPU backend).
**Uprobe offset**: `0x35280` (DO droplet build of srsRAN).

### Gate 0 — Mode conflict resolved (Option A): PASS

DEV-023/024 resolved: single CXL region in system-ram mode.

```
[gate0] PROOF1 ptr=0x76139bca2000 numa_node=1 cxl_node=YES exit=0
[gate0] PROOF2 clCreateBuffer(CL_MEM_USE_HOST_PTR) err=0
[gate0] sentinel_cpu=0xCA7EBEEF cl_out=0xCA7EBEEF match=YES
[gate0] GATE0 PASS: option=A zero_copy=YES numa_node=1
```

| Fact | Value |
|------|-------|
| NUMA node 1 | 1,920 MB, distance=20 |
| Node 0 | 3,915 MB DRAM |
| Allocation | `numa_alloc_onnode(1 MB, node=1)` |

### Gate 1 — Uprobe fires on srsRAN decode: PASS

| Fact | Value |
|------|-------|
| Symbol | `_ZN6srsran17ldpc_decoder_impl6decodeE...` |
| Offset | `0x35280` |
| Events for 3 reps × 4 CB variants | **12 / 12 expected** |
| DEV-025 | No `cxl_bus.ko` in kernel 6.8.0-124 |

### Gate 2 — E2E pipeline (cross-process LLR extraction): PARTIAL PASS

**Re-tested 2026-06-23 with gate2_xproc.c**

```
[gate2] child PID=42371
[gate2] llr_ptr=0x6409d141da10  (from %rdx fetcharg)
[gate2] move_pages(42371 → node1): 5/7 pages OK, 2/7 EFAULT
[gate2] LLR[0..2]=-10 10 10  (real decode input)
[gate2] OCL decode: 668,015.5 µs
[gate2] bit_diff=4176 (popcount, NOT oracle — criterion d NOT MET)
[gate2] uprobe_hits=1150
```

| Criterion | Status | Evidence |
|-----------|--------|----------|
| (a) Process tree + uprobe | **MET** | child PID=42371, uprobe_hits=1150 |
| (b) LLR on CXL node 1 | **MET** | llr_ptr from %rdx; move_pages→node1; N1=5 pages |
| (c) CL_MEM_USE_HOST_PTR | **MET** | Parent CXL buf, child LLR via /proc/mem; err=0 |
| (d) bit_diff=0 vs oracle | **NOT MET — DEFERRED** | popcount only (DEV-032) |
| (e) CSV written | **MET** | `paper/results/droplet/e2e_droplet.csv` |

Key deviations:
- **DEV-030**: `numactl --membind=1` → SIGILL on pmem WC pages. Workaround: run on node 0, migrate via `move_pages()`.
- **DEV-031**: 2/7 LLR pages EFAULT (demand-paged, not yet faulted in).
- **DEV-028**: PoCL JIT >20 min in QEMU-TCG for `ldpc_decode.cl`. Used `cxl_copy` kernel instead.

Source: `paper/results/droplet/e2e_droplet.csv`
```
emulation_mode: qemu_cxl_node1_move_pages
uprobe_hits: 1150, pages_migrated: 5/7, ocl_us: 668015.5
```

### Gate 3 — RESULTS_SUMMARY.md update: PASS

Paper §5 updated with honest v6 data. No prior discredited numbers reinstated.

**Official DEV log ends at DEV-032.**

---

## v7 — GCP KVM n2-standard-4 (2026-06-26)

**Environment**: GCP n2-standard-4 (asia-south2-a), Ubuntu 22.04, nested KVM. QEMU CXL VM with real KVM.
**Target**: srsRAN `ldpc_decoder_benchmark`. **Uprobe offset**: `0x30cf0` (GCP build, differs from DO 0x35280).
**Status**: INFRASTRUCTURE ONLY — no gate files submitted.

### What was completed

| Item | Status |
|------|--------|
| GCP instance provisioned, packages installed | DONE |
| Source rsync'd, bpftime built (DEV-035 cmake patch) | DONE |
| srsRAN benchmark built; offset `0x30cf0` → `/etc/cxl_poc_uprobe_offset` | DONE |
| QEMU VM launched, kernel 5.15 → 6.8 HWE upgrade | DONE |
| ndctl v80 built from source (apt ships v72.1, lacks `cxl create-region`) | DONE |
| CXL topology: system-ram, NUMA node 1, 1920 MB | DONE |
| Ops scripts: provision/install_deps/rsync/build_tools/prepare_vm/launch_vm/run_e2e_test/teardown | DONE |

### What is missing

| Required | Status |
|----------|--------|
| `memory/v7_run/implementer/gate_0.md–gate_5.md` | **NOT SUBMITTED** |
| `e2e_gcp.csv` in `paper/results/` | **NOT SUBMITTED** |
| DEV-033+ log entries | **NOT SUBMITTED** |
| **Gate 3: LLR mover** (bpftime handler → shared CXL memfd) | **NOT WRITTEN** |
| GCP instance deletion confirmation | **UNCONFIRMED** |
| DEV-031/033 numbering conflict in SETUP_RUNBOOK.md | **NOT FIXED** |

### Numbering conflict

SETUP_RUNBOOK.md incorrectly uses "DEV-031" for GCP numactl SIGILL. DEV-031 in the official log = v6 "2/7 pages EFAULT". GCP-specific deviations must start at **DEV-033**:
- DEV-033: GCP numactl SIGILL (pmem=off still produces WC pages)
- DEV-034: ndctl 72.1 missing `cxl create-region` → built v80
- DEV-035: bpftime cmake bpftool symlink → `find_program()` patch
- DEV-036: (next available)

### What v7 gates require

| Gate | Proof needed |
|------|-------------|
| 0 | `dmesg \| grep KVM` inside VM → "KVM" (not TCG) |
| 1 | uprobe_hits > 0 with offset `0x30cf0` (GCP-build) |
| 2 | numactl --hardware → 2 nodes; mbind smoke test; DEV-033 resolved |
| **3** | **bpftime handler writes LLR[0..2] ∈ ±5..±20 into shared CXL memfd** |
| 4 | (a)–(e) all assessed; pstree shows simultaneous processes |
| 5 | `gcloud compute instances list` → cxl-systems-lab absent |

**Gate 3 is the decisive gate**: the first time bpftime handler writes LLR directly into a CXL-backed shared memfd (not tracefs fetcharg + move_pages).

---

## DEV Log Summary (cross-version)

| DEV | Version | Gate | Description | Status |
|-----|---------|------|-------------|--------|
| DEV-001 | v4 | 0.4 | Multi-socket check skipped (WSL2 single-socket) | Closed (informational) |
| DEV-002 | v4 | 0.1 | bpftime ubpf JIT instead of LLVM JIT | Closed |
| DEV-003 | v4 | 2 | Non-atomic bpftime counter | Closed (DEV-013 PERCPU_ARRAY) |
| DEV-004 | v4 | 0.3 | CXLMemSim CLI mismatch | Closed (informational) |
| DEV-005 | v4 | 0.3 | WSL2 no hardware PMU | Open (deferred, all versions) |
| DEV-006 | v4 | 0.2 | OAI serveraddr flag unsupported | Closed |
| DEV-007 | v4 | 0.2 | UE PHY sync mismatch (not blocking) | Closed |
| DEV-008 | v4 | 1 | Runtime oracle instead of pre-built vectors | Closed |
| DEV-009 | v4 | 2 | C_actual=2 (not C=24 from PRIMARY_CONFIG) | Open (known limitation) |
| DEV-010 | v4 | 2 | LLR payload in BPF map, not pointer | Closed |
| DEV-011 | v4 | 2 | Z=224 uses iLS=0 shift tables | Open (ghost — v7 must close) |
| DEV-012 | v4 | 3 | Slot period in aggregate NIC rate | Closed |
| DEV-013 | v4 | 3 | PERCPU_ARRAY for OAI thread-pool atomics | Closed |
| DEV-014 | v4 | 5 | 2ms poll dominates interception overhead | Open (known limitation) |
| DEV-015 | v5 | 2 | PoCL USE_HOST_PTR crash on large buffers | Closed |
| DEV-016 | v5 | 2 | OCL kernel arg order bug | Closed |
| DEV-017–019 | v5 | 3 | bpftime build/attach deviations | Closed |
| DEV-020 | v5 | 4 | bpftime IPC floor 1,075 µs/CB | Open (known) |
| DEV-021 | v5 | 4 | (ablation sub-issue) | Closed |
| DEV-022 | v5 | 5 | DO KVM no PMU → CXLMemSim blocked | Open (deferred all versions) |
| DEV-023 | v5 | 5 | /dev/dax0.0 ENXIO in devdax mode | Closed (Option A, Gate 0) |
| DEV-024 | v5 | 5 | system-ram + devdax mutually exclusive | Closed (Option A) |
| DEV-025 | v6 | 1 | No cxl_bus.ko in kernel 6.8.0-124 | Closed |
| DEV-026 | v6 | 2 | cxl_copy instead of LDPC OCL kernel | Open (DEV-028 root cause) |
| DEV-027 | v6 | 2 | LLR via /proc/mem not bpftime zero-copy | Open (Gate 3 v7 closes this) |
| DEV-028 | v6 | 2 | PoCL LLVM JIT >20 min in QEMU-TCG | Open (needs real KVM) |
| DEV-029 | v6 | 2 | Stale tracefs uprobe_hits=0 | Closed (gate2_xproc fix) |
| DEV-030 | v6 | 2 | numactl --membind=1 SIGILL on pmem WC | Open (ghost — v7 DEV-033) |
| DEV-031 | v6 | 2 | 2/7 LLR pages EFAULT (demand-paged) | Open (ghost) |
| DEV-032 | v6 | 2 | criterion (d) deferred — popcount not oracle | Open (ghost) |
| DEV-033 | v7 | — | GCP numactl SIGILL (pmem=off, WC persists) | **Not logged yet** |
| DEV-034 | v7 | — | ndctl 72.1 missing cxl create-region | **Not logged yet** |
| DEV-035 | v7 | — | bpftime cmake bpftool ExternalProject → find_program | **Not logged yet** |

---

## Key Numbers Reference

| Metric | Value | Version | Source |
|--------|-------|---------|--------|
| PRIMARY_CONFIG | **11,703 µs/slot = 23.4×** | v4 | `baseline_latency.csv` (fixed anchor) |
| bpftime round-trip floor | **248.5 ns/call** | v4 | Gate 0.1 |
| Kernel uprobe baseline | 9,941 ns/call | v4 | Gate 0.1 |
| OCL LDPC bit-correctness | **0 mismatches** / 204,800 bits | v4 | `bit_correctness.csv` |
| SPSC poll-to-handle p50 | **17 ns** | v5 | Gate 1 |
| v4 interception overhead | **2,636 µs/slot** (poll-dominated) | v4 | `ablation_raw.csv` pass=0 |
| v4 gpu_compute_full | **163,528 µs/slot = 327×** | v4 | `ablation_raw.csv` pass=1 |
| GPU projected (6× speedup) | **27,561 µs/slot = 55.1×** | v4 | projected |
| CXL NUMA node 1 | 1,920 MB, distance=20 | v5/v6/v7 | QEMU emulated |
| CXL sentinel zero-copy | **PASS** (0xCA7EBEEF match) | v6 | Gate 0 |
| v6 uprobe hits | **1,150** | v6 | Gate 2 / gate2_xproc |
| v6 OCL decode (QEMU-TCG) | **668,015 µs** | v6 | Gate 2 (emulation only) |
| v7 uprobe offset (GCP build) | **0x30cf0** | v7 | `nm ldpc_decoder_benchmark` |
| v7 uprobe offset (DO build) | 0x35280 | v6 | `nm ldpc_decoder_benchmark` |
| v7 this-session kernel BPF test | 4,080 hits / 12.2 MB LLR | v7 session | `e2e_gcp.csv` (wrong mechanism) |

---

## Open Ghosts (must close in v7)

| Ghost | DEV | What closes it |
|-------|-----|----------------|
| Non-oracle bit comparison | DEV-032 | Full LDPC OCL decode on real KVM (not QEMU-TCG) |
| numactl SIGILL on CXL | DEV-030/033 | pmem=off + mbind shim in Gate 2 |
| 2/7 pages EFAULT | DEV-031 | bpftime writes directly to pre-faulted CXL memfd (Gate 3) |
| Z=224 iLS table | DEV-011 | Gate 4 assessment |
| LLR via /proc/mem not zero-copy | DEV-027 | Gate 3: bpftime handler → CXL memfd |
| CXLMemSim deferred | DEV-005/022 | Real hardware with PMU (IISc HACC or bare-metal) |
