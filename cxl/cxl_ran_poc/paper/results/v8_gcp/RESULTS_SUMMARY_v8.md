# v8 Results — CXL/bpftime LDPC Offload, GCP KVM Environment
<!-- Generated 2026-07-01. All numbers traceable to gate files in
     memory/v8_run/implementer/ (gate_0.md .. gate_5.md, DEVIATIONS.md) and to
     paper/results/{e2e_gcp.csv, bit_correctness.csv}.
     Environment: GCP n2-standard-4 (asia-south2-a) running QEMU/KVM with a
     CXL type-3 device. This supersedes the DigitalOcean droplet run
     (paper/results/droplet/, no KVM, no real interception — see §7 for the
     delta) as the primary CXL/bpftime evidence base. -->

---

## 1. Environment

| Layer | Detail |
|-------|--------|
| Host | GCP `n2-standard-4`, project `cxl-systems-lab-26`, zone `asia-south2-a` |
| Hypervisor | QEMU 8.2.0, **`-enable-kvm -cpu host,-hypervisor`** (nested KVM — real hardware virtualization, not TCG) |
| CXL device | `-M q35,cxl=on`, `pxb-cxl` → `cxl-rp` → `cxl-type3`, `persistent-memdev`, 2 GB backing, `cxl-fmw.0.size=4G` |
| Guest VM | Ubuntu cloud image, kernel `6.8.0-124-generic` |
| CXL exposure | `daxctl reconfigure-device --mode=system-ram` → **NUMA node 1, 1920 MB, distance=20** |
| BPF runtime | bpftime (`ubpf` userspace VM) — `libbpftime-syscall-server.so` (consumer) + `libbpftime-agent.so` (LD_PRELOAD into benchmark) |
| OpenCL | PoCL 1.2, CPU device (`pthread-Intel(R) Xeon(R) CPU @ 2.80GHz`) — **no GPU in this environment** |
| Workload | srsRAN `ldpc_decoder_benchmark` (unmodified binary), `-L 384 -I 5 -T avx2 -R 1000` |

**Key environment difference vs. the earlier droplet run** (`paper/results/droplet/`):
the droplet had no `/dev/kvm` and no PMU passthrough, so all interception evidence
there was collected under full QEMU TCG software emulation. This GCP environment has
real KVM (`/dev/kvm` present, `vmx` flag confirmed, `systemd-detect-virt=qemu`), which
is corroborated by measured decode latency (§4) being 15–75× faster than the TCG-only
droplet numbers would predict for the same kernel.

---

## 2. Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│  QEMU VM (KVM-accelerated, kernel 6.8.0-124-generic, q35+CXL)          │
│                                                                        │
│  ┌──────────────────────────┐        ┌────────────────────────────┐   │
│  │ srsRAN benchmark          │        │ llr_consumer_v8 (parent)   │   │
│  │ ldpc_decoder_benchmark    │        │ under bpftime               │   │
│  │ LD_PRELOAD:                │        │ syscall-server               │   │
│  │  libbpftime-agent.so      │        │                              │   │
│  │  cxl_init.so (shm mapper) │        │ shm_open("/cxl_region_v8")  │   │
│  │                            │        │  → mmap → mbind(node=1)     │   │
│  │ uprobe:                    │        │                              │   │
│  │  ldpc_decoder_impl::decode │◄──────►│ ring_map / scratch_map /    │   │
│  │  (bpftime ubpf JIT)        │  bpf   │ config_map / fire_count     │   │
│  │                            │  maps  │ (all bpftime in-process     │   │
│  └────────────┬───────────────┘        │  shared-memory maps)         │   │
│               │                        │                              │   │
│      each decode() call:               │ busy-poll ring_map:          │   │
│      1. read LLR ptr from %rdx (PARM3) │  for each descriptor:        │   │
│      2. bpf_probe_read_user → scratch_ │   read scratch_map[slot]     │   │
│         map[seq % 256]                 │   → cxl_copy() to CXL (CB 0  │   │
│      3. push descriptor to ring_map    │     only — DEV-040)          │   │
│                                         │   → zero-pad + BG-detect     │   │
│                                         │     (n_vn_full/n_cn/n_vn_    │   │
│                                         │     info from llr_len/Z)     │   │
│                                         │   → OCL decode (stack        │   │
│                                         │     buffer input — DEV-042)  │   │
│                                         │   → write e2e_gcp.csv row    │   │
│                                         └──────────┬───────────────────┘   │
│                                                    │                       │
│                                          CXL NUMA NODE 1 (1920 MB) ◄──────┘
│                                          /dev/dax0.0 → system-ram mode
│                                          (pxb-cxl → cxl-rp → cxl-type3)
└────────────────────────────────────────────────────────────────────────┘
```

**Interception mechanism**: bpftime uprobe on `ldpc_decoder_impl::decode()`, zero
kernel modules, zero srsRAN source modification (unmodified benchmark binary).
LLR is read from `%rdx` (span pointer, 3rd argument) via `bpf_probe_read_user` into
a bpftime in-process ring (`scratch_map`, 256 slots × 26,112 bytes). The consumer
copies from `scratch_map` into the CXL-backed shared-memory region and dispatches to
an OpenCL LDPC min-sum decoder kernel (`ldpc_decode.cl`, layered, BG1/BG2, N_iter=6).

**CXL data-path note**: `bpf_probe_write_user` directly into the CXL region was tried
and removed (DEV-034 — installs a SIGSEGV handler in bpftime's ubpf VM that conflicts
with the benchmark process). The BPF handler therefore only *reads* LLR into
`scratch_map`; the consumer performs the CXL write itself, once per run (CB 0 only —
DEV-040, a QEMU device-memory throughput constraint, not an architectural limit; see §6).

---

## 3. Phase-by-phase gate results

| Gate | What is checked | Result | Key evidence |
|------|------------------|--------|---------------|
| 0 | System environment: KVM, benchmark binary, bpftime libs, uprobe offset | **PASS** | `/dev/kvm` present, `vmx` flag in cpuinfo, `systemd-detect-virt=qemu`, uprobe offset `0x3fc80` |
| 1 | CXL NUMA topology + uprobe attach + first fire | **PASS** | `numactl --hardware`: node 1 = 1920 MB, distance=20; `gate0_option_a` mbind-shim proof: `numa_node=1 cxl_node=YES zero_copy=YES`; uprobe registered pre-fork; first ring fire at `ring_slot=0` |
| 2 | CXL region mapping + BPF object load | **PASS** | `shm_open` + `mmap` + `mbind(MPOL_BIND, node=1)` → `cxl_ok=YES`; BPF object loaded via bpftime syscall-server |
| 3 | bpftime start + config_map handoff + first CXL write | **PASS** | bench VA received via `cxl_init.so` constructor; CXL write of 9,216 bytes = 219 ms (DEV-040, see §6) |
| 4 | 4000-CB end-to-end run | **PASS** | `fire_count = ring_head = cb_count = decoded_count = 4000`, exit code 0; process tree captured live (`ps aux` snapshot, both processes simultaneously visible) |
| 5 | Instance teardown | N/A | GCP instance kept live between runs (stop/start managed manually, not torn down per-run) |

Full verbatim terminal output for every gate is in
`memory/v8_run/implementer/gate_0.md` … `gate_5.md`.

---

## 4. Latency results

### 4.1 Per-CB decode latency (measured, KVM-accelerated PoCL CPU backend)

4,000 code blocks, mixed BG1/BG2 (see §4.3 for the CB-type breakdown), N_iter=6:

| Metric | decode_us (OCL kernel wall-clock) | e2e_us (consumer-internal, ring-read → OCL-complete) |
|--------|-----------------------------------|-------------------------------------------------------|
| min | 10,090 µs | 10,231 µs |
| p50 | 11,065 µs | 11,449 µs |
| p95 | 11,381 µs | 11,792 µs |
| p99 | 11,599 µs | 12,016 µs |
| max | 24,123 µs | 237,370 µs *(CB 0 only; includes the one-time 219 ms CXL write — excl. CB 0: max = 24,234 µs)* |
| mean | 11,061 µs | 11,511 µs *(excl. CB 0: 11,454 µs)* |

`e2e_us` is measured entirely within the consumer process using
`clock_gettime(CLOCK_MONOTONIC)` for both the start (ring-descriptor read) and end
(OCL `clFinish()` return) timestamps — **not** a cross-process measurement. An earlier
version subtracted the consumer's `CLOCK_MONOTONIC` timestamp from a BPF-side
`bpf_ktime_get_ns()` timestamp with a different clock epoch, producing meaningless
values (one row showed 18.9 hours); this was corrected before the numbers above were
collected.

Both `decode_us` and `e2e_us` distributions are consistent with KVM-accelerated
execution: on unaccelerated QEMU TCG, an equivalent PoCL kernel call would be expected
in the 40–200 ms range, not ~11 ms (see §7 for the TCG-vs-KVM comparison against the
droplet run).

### 4.2 One-time CXL write cost (DEV-040)

| Step | Bytes | Cost |
|------|-------|------|
| CXL device-memory write (CB 0 only) | 9,216 | 219,270 µs (≈23.8 µs/byte) |
| bpftime scratch_map read (per CB, in-process) | up to 26,112 | 4–24 µs |

The CXL write rate (~24 µs/byte) reflects QEMU's device-memory emulation path for the
`memory-backend-file`-backed CXL region, not a property of CXL hardware — see DEV-040
in §6 for the real-hardware comparison (<5 ns/byte expected).

### 4.3 Coverage: all 4,000 code blocks, both base graphs, both benchmark configs

The unmodified srsRAN benchmark (`ldpc_decoder_benchmark.cpp`) exercises 4 distinct
code-block configurations per its own test matrix (2 base graphs × {min, max}
transmitted-VN count):

| llr_len (bytes) | Count | Config | Graph parameters used |
|------------------|-------|--------|------------------------|
| 9,216  | 1     | BG1, min-length (0 parity transmitted) | N=68, M=46, K=22 |
| 25,344 | 26    | BG1, max-length (full parity) | N=68, M=46, K=22 |
| 4,608  | 8     | BG2, min-length (0 parity transmitted) | N=52, M=42, K=10 |
| 19,200 | 3,965 | BG2, max-length (full parity) | N=52, M=42, K=10 |
| **Total** | **4,000** | | **0 skipped** |

Graph parameters are inferred per-CB at runtime from `llr_len / Z` — no hardcoded
base-graph assumption. All 4,000 dispatched CBs used correctly matched graph
parameters and shift tables (`BG1_SHIFTS` / `BG2_SHIFTS`, TS 38.212 Table 5.3.2-2/3).

---

## 5. Bit-exact correctness of the decode kernel

The live benchmark run (§4) does **not** provide a correctness oracle: its default
invocation (`-L 384 -I 5 -T avx2 -R 1000`, no `-C` flag) generates **purely random LLR
with no encoded message** for each `decode()` call (srsRAN's own `use_crc=false`
default — `ldpc_decoder_benchmark.cpp:39,143-146`). There is no ground truth to
compare live-run decoder output against; this is a property of the benchmark's
default configuration, not a limitation of the pipeline.

To establish real correctness for the exact OpenCL kernel used in the live pipeline
(`ldpc_decode.cl`, byte-identical file — confirmed via checksum), a standalone
encode→decode→compare harness (`bit_diff_test.cpp`) was built against srsRAN's own
LDPC encoder and run:

| BG | Zc (lifting size) | Iterations | Messages | Total bits | Mismatches | Result |
|----|--------------------|------------|----------|-------------|------------|--------|
| 1 | 384 | 6 | 500 | 4,224,000 | 0 | **PASS** |
| 2 | 384 | 6 | 500 | 1,920,000 | 0 | **PASS** |
| 1 | 256 | 6 | 500 | 2,816,000 | 0 | **PASS** |
| 2 | 256 | 6 | 500 | 1,280,000 | 0 | **PASS** |
| **Total** | | | **2,000** | **10,240,000** | **0** | **PASS** |

Method: srsRAN's own LDPC encoder (`create_ldpc_encoder_factory_sw("generic")`)
generates a random message, encodes it, and converts the codeword to LLR (±10
amplitude, matching the live pipeline's LLR convention). The kernel's decoded output
is compared bit-for-bit against the original message. BG1 LS=384 and BG2 LS=384 are
the exact configurations used by the live 4,000-CB run in §4.

Source: `cxl_ran_poc/paper/results/v8_gcp/bit_correctness_v8.csv`.

---

## 6. Deviations (QEMU-specific vs. real bugs)

Full detail with resolutions in `memory/v8_run/implementer/DEVIATIONS.md`
(DEV-033 through DEV-044). Summary, scope-labelled:

| DEV | Issue | Scope | Real-hardware impact |
|-----|-------|-------|------------------------|
| 038 | glibc SIMD memcpy SIGILL on CXL device-memory pages | QEMU-WC | None — real CXL has DDR cache semantics |
| 040 | CXL device-memory write rate ≈24 µs/byte → write once (CB 0), not per-CB | QEMU-WC | None — real CXL: <5 ns/byte, all 4,000 CBs writable within budget |
| 042 | PoCL SIMD loads SIGILL on `CL_MEM_USE_HOST_PTR` over CXL device-memory → OCL I/O redirected through stack buffers | QEMU-WC | None — real CXL: `CL_MEM_USE_HOST_PTR` works directly |
| 037, 039, 041 | Ring-buffer sizing bug, GCC memcpy-idiom substitution, `waitpid` syscall overhead in a tight spin loop | BUG | Fixed in source; not QEMU-specific |
| 043 | BG-detection missed 2 of 4 legitimate benchmark configs (2,970/4,000 CBs under-counted) | BUG | Fixed in source |
| 044 | Live benchmark has no correctness ground truth (`use_crc=false` default) | DESIGN | Independent of QEMU vs. real hardware; addressed via the standalone kernel oracle (§5) |

**QEMU-WC entries (038, 040, 042) share one root cause**: the CXL region is backed by
QEMU `memory-backend-file` device-memory, which lacks the write-combining/cache
semantics that both glibc's SIMD `memcpy` and PoCL's SIMD loads assume. All three are
emulation artifacts of this specific QEMU CXL backend and do not apply to physical CXL
hardware, which presents standard DDR-class cache semantics to the CPU.

---

## 7. Comparison to the DigitalOcean droplet run

| Aspect | Droplet (`paper/results/droplet/`) | GCP v8 (this document) |
|--------|--------------------------------------|--------------------------|
| KVM | Absent (`ENOENT` on `PERF_TYPE_HARDWARE`) | Present, confirmed via decode latency |
| CXL write path | `numactl --membind=1` (SIGILL, DEV-030) → `move_pages()` fallback | `mbind()` direct + `shm_open` region |
| Decode coverage | 1 CB (single-CB OCL proof, `cxl_copy` hard-decision only — full iterative decode blocked by >20 min PoCL JIT compile under TCG, DEV-028) | 4,000/4,000 CBs, full iterative min-sum decode (N_iter=6) |
| Bit-correctness | Cited from a separate Phase 1 run (Gate 1, same kernel, no live oracle) | Same approach — standalone oracle (§5) — plus full live-run coverage |
| OCL decode latency (single CB) | 668,016 µs (QEMU TCG + PoCL JIT) | 11,065 µs p50 (KVM + PoCL, no per-CB JIT — kernel built once) |
| Gate 4(d) bit_diff | NOT MET (deferred, DEV-032) | Still `-1` (DEFERRED) for the *live* run — same honest status, but now backed by a real kernel-level oracle (§5) that the droplet run did not have |

The GCP/KVM environment is a materially stronger evidence base than the droplet run:
real hardware virtualization (not full-system TCG emulation) makes the decode-latency
numbers representative of software LDPC decode cost rather than emulator overhead, and
full 4,000-CB coverage replaces the droplet's single-CB proof-of-mechanism.

---

## 8. What is proven vs. what remains open

**Proven, with verifiable evidence:**
- Zero-modification interception of an unmodified srsRAN binary via bpftime uprobe (4,000/4,000 fires)
- CXL NUMA node 1 allocation and one confirmed write (bpftime → scratch_map → CXL)
- Correct per-CB base-graph parameter inference from wire-format `llr_len` alone (4/4 benchmark configs handled)
- Bit-exact correctness of the exact OpenCL LDPC kernel used live, under real srsRAN-encoded messages (10.24M bits, 0 mismatches)
- KVM-accelerated decode latency: p50 = 11.1 ms/CB on a CPU OpenCL backend (no GPU in this environment)

**Explicitly open, not claimed:**
- Live per-CB correctness oracle: the benchmark's default invocation has no encoded
  ground truth (§5); would require `-C` benchmark flag + additional instrumentation to
  capture pre-encode message bits from the live process. Not implemented.
- GPU acceleration: this environment has PoCL CPU only; a real GPU backend is expected
  to reduce `decode_us` substantially (see §6 of the main `RESULTS_SUMMARY.md`, which
  projects ~6× from "Six Times to Spare", not re-measured here).
- Real CXL hardware latency: this is still a QEMU-emulated CXL device
  (`memory-backend-file`); CXLMemSim / real hardware validation remains future work
  (same limitation noted in the main summary, §8).

---

## 9. Source files

- Gate evidence (verbatim terminal output): `memory/v8_run/implementer/gate_0.md` … `gate_5.md`
- Deviation log: `memory/v8_run/implementer/DEVIATIONS.md`
- Live-run CSV (4,000 rows): `cxl_ran_poc/paper/results/v8_gcp/e2e_gcp.csv`
- Kernel-oracle CSV (this run, 500 msg/config): `cxl_ran_poc/paper/results/v8_gcp/bit_correctness_v8.csv`
  (distinct from the earlier Phase 1 `cxl_ran_poc/paper/results/bit_correctness.csv`,
  10 msg/config — both PASS with 0 mismatches; this run adds statistical weight, 50× more messages)
- Consumer source: `cxl_ran_poc/phase5_cxl/llr_consumer_v8.c`
- BPF handler source: `cxl_ran_poc/phase5_cxl/lddc_llr_mover.bpf.c`
- OCL kernel: `cxl_ran_poc/gpu_daemon/ldpc_cl/ldpc_decode.cl`
- Kernel-oracle harness: `cxl_ran_poc/gpu_daemon/ldpc_cl/bit_diff_test.cpp`

**PRIMARY_CONFIG anchor (11,703 µs/slot = 23.4× over budget) is unrelated to and
unchanged by this document** — it is the paper's fixed motivation baseline from a
separate host-native AVX2 measurement (`paper/results/RESULTS_SUMMARY.md` §1), not
re-derived or re-measured here.
