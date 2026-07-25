# Gap Analysis: Research vs. Implementation

**Project:** CXL + eBPF + 5G NR L1 Offload PoC  
**Research reference:** `claude_research.md`  
**Analysed:** 2026-06-12  
**Environment update:** 2026-06-12 (containerised QEMU board — see §Environment Setup)  
**Pipeline fix update:** 2026-06-13 (eBPF verifier bug fixed, CXL system-RAM mode, full pipeline running)  

---

## Overview

The research (`claude_research.md`) defines a three-stage roadmap:

| Stage | Goal | Status |
|-------|------|--------|
| 1 | Functional spine — QEMU CXL + eBPF uprobe + CPU daemon + LDPC bit-exact | Partial (critical bugs) |
| 2 | RAN integration — OAI/srsRAN rfsim with real LDPC shared library | Not started |
| 3 | Latency/fidelity claims — NUMA sweep + CXLMemSim/Mess sensitivity | Not started |

Seventeen gaps are identified below, grouped by severity.

---

## CRITICAL GAPS — Block paper validity

### GAP-1 · LDPC algorithm is a synthetic O(N²) placeholder — 37× too slow

**File:** `l1_sim/ldpc.c` — `ldpc_min_sum_block()`

The min-sum decoder uses hardcoded pseudo-random index offsets
(`i * 9973 + j * 4099`) instead of a real BG1/BG2 parity-check matrix.
Consequences:

- `calibration_check.txt` reports **26.4 ms** per codeblock vs. the **~0.71 ms**
  target from arXiv:2602.04652 (37× slower).
- `ldpc_verify()` only checks the LSB of each byte — it passes trivially and
  does not validate any LDPC codeword property.
- Numbers cannot be compared to any srsRAN/OAI benchmark.

There is also a `const`-cast write-through bug: `ldpc_min_sum_block` takes
`const int8_t *llr` but calls `memcpy((void *)llr, g_llr_buf, n)`, silently
mutating the caller's input buffer.

**Fix:** Replace with a real BG1/BG2 lookup-table min-sum decoder whose
parity-check matrix is derived from the 5G NR base-graph lifting tables
(3GPP TS 38.212 §5.3.2). Reference: srsRAN `lib/phy/upper/channel_coding/ldpc/`
or OpenAirInterface `openair1/PHY/CODING/nrLDPC_decoder/`.

---

### GAP-2 · Offload path provides zero speedup — daemon re-runs the same algorithm

**File:** `gpu_daemon/gpu_compute.c`

`gpu_compute_ldpc()` and `gpu_compute_fft()` call `ldpc_decode_internal()` and
`fft_process_internal()` — the same CPU implementations used by the baseline.
The full offload round-trip is:

```
L1 sim → memcpy to CXL shm → Unix socket connect+send → worker queue →
  daemon thread → ldpc_decode_internal() → signal_completion()
```

Result: baseline and offload both show **~108 ms/slot, 100% deadline miss
rate**. The offload path is marginally *slower* than the baseline due to IPC and
memcpy overhead.

**Fix (architectural proof, not performance):** Give the daemon a
reduced-iteration decoder (e.g., 5 iterations vs. the baseline's 20) so that
the offload *is* faster on the current synthetic decoder. Clearly label this as
a controlled architecture demonstration, not a GPU speedup claim. Stage 3 adds
projected GPU numbers from GPGPU-Sim/MGPUSim.

---

### GAP-3 · Measurement data corruption — one outlier row poisons all statistics

**File:** `paper/results/baseline_latency.csv`, row 98

Slot 98 contains `18446744073815234` µs (~584 years). This is produced by
`timespec_diff_us()` when the host OS interrupts the process between the two
`clock_gettime()` calls long enough to produce a large positive `int64_t` that
passes the `us > 0` guard but is physically impossible.

Because `pandas.mean()` is not outlier-resistant, this single row causes
`paper/notes.md` to report:

```
Mean per-slot latency: 2029141848215326.0 µs   ← corrupted
```

The same defect exists in `offload_latency.csv`.

**Fix:** In `timespec_diff_us()`, clamp the return value:
```c
return (us > 0 && us < 10000000LL) ? (uint64_t)us : 0;
```
In `generate_notes.py`, filter outliers before computing statistics:
```python
df = df[df["latency_us"] < 1_000_000]
```

---

### GAP-4 · "eBPF overhead" measurement measures LDPC compute, not probe cost

**File:** `measurement/measure.c` — `run_ebpf_overhead()`

The `--mode ebpf-overhead` path calls `ldpc_decode_internal()` in a tight loop
with no eBPF probe attached, recording ~734–771 µs per call. This is the bare
LDPC compute latency, not the ~1.7 µs uprobe round-trip cost cited from the
Cloudflare `ebpf_exporter` benchmark.

No code in the project actually measures the kernel uprobe overhead.

**Fix:** Add a dedicated microbenchmark:
1. Attach a no-op eBPF uprobe to a cheap stub function (`__attribute__((noinline)) void probe_target(void) {}`).
2. Call it in a loop of N=10 000 iterations.
3. Report `(total_ns / N)` as the per-probe cost.
This validates the ~1.7 µs figure on the actual test machine.

---

## ARCHITECTURAL GAPS — Block the novel contribution claim

### GAP-5 · eBPF and LD_PRELOAD offload paths are disconnected — **PARTIALLY FIXED**

**Files:** `ebpf/l1_intercept.bpf.c`, `ebpf/l1_intercept_loader.c`,
`offload/l1_offload.c`

The research's primary novelty claim is *"transparent eBPF-uprobe interception
routing compute through CXL shared memory."* In reality there are two separate,
unconnected mechanisms:

| Path | Mechanism | Effect |
|------|-----------|--------|
| LD_PRELOAD | `libl1_offload.so` weak symbol `l1_offload_try` | Actually short-circuits and offloads compute |
| eBPF | uprobes on `ldpc_decode` / `fft_process` | Passively emits telemetry to ringbuf; the original function **still executes** |

The eBPF uprobe fires **after** the weak-symbol check has already redirected
execution. If `LD_PRELOAD` is not in effect, the uprobe fires but the function
runs normally — no interception occurs. There are no uretprobes and no
`PT_REGS_RC` manipulation to suppress the original function.

**Fix (for the paper claim):** Either:
- (a) Document the architecture honestly as "weak-symbol + eBPF telemetry
  overlay" — still publishable as a transparent-to-the-application offload with
  zero source changes.
- (b) Implement true eBPF-based interception using a uprobe that writes the
  offload result via `bpf_probe_write_user` and a uretprobe that overwrites
  `PT_REGS_RC` to return 0 (indicating offload success), so `LD_PRELOAD` is not
  required at all.

**2026-06-13 update:** The eBPF loader now successfully loads and attaches uprobes.
Root causes fixed:
1. BPF verifier rejected `l1_intercept.bpf.c` due to use-after-submit: `event->input_len`
   was read after `bpf_ringbuf_submit(event, 0)` made `event` a dangling pointer.
   Fixed by moving stats update before submit.
2. Clang-18 binary was a zero-byte file (corrupted cloud-init install). Reinstalled.
3. `perf_event_paranoid=4` blocked uprobe attach. Fixed with `sysctl -w`.
The eBPF loader runs (`[ebpf] loader running (pid=…)`) and fires on every
`ldpc_decode`/`fft_process` invocation during the offload path.

---

### GAP-6 · No `BPF_MAP_TYPE_USER_RINGBUF` — control path is absent

**File:** `ebpf/l1_intercept.bpf.c`

The research recommends `BPF_MAP_TYPE_USER_RINGBUF` (Linux 6.1,
`BPF_MAP_TYPE_USER_RINGBUF`, commit 583c1f4) for user→kernel offload control
signalling. The implementation has only:

- `BPF_MAP_TYPE_RINGBUF`: kernel→user telemetry ✓
- `completion_map` (`BPF_MAP_TYPE_HASH`): defined in BPF but **never written
  to** by any code path.
- `offload_enabled`: a one-shot flag written at startup, not a control channel.

There is no eBPF-mediated mechanism for the daemon to signal "result is ready in
CXL buffer." Completion is signalled instead by a busy-poll on
`ctrl->gpu_done` in `l1_offload.c`.

**Fix:** Either keep the busy-poll (acceptable for functional PoC) and document
it, or add a `USER_RINGBUF` map for the daemon to push completion tokens back to
the BPF program.

---

### GAP-7 · New Unix socket connection opened per eBPF event

**File:** `ebpf/l1_intercept_loader.c` — `forward_event()`

```c
// Called for every ringbuf event:
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
connect(fd, ...);
write(fd, event, ...);
close(fd);
```

A Unix domain socket `connect()` costs ~10–50 µs. With 14 FFT uprobes and 4
LDPC uprobes per slot, this adds ~180–900 µs of IPC overhead per slot —
destroying the ~1.7 µs-per-probe budget the research relies on.

**Fix:** Open the socket once at startup, keep it persistent, and reconnect only
on ECONNREFUSED. Use `send()` in a loop.

---

## FIDELITY GAPS — Block latency and sensitivity claims (Stage 3)

### GAP-8 · Running on `mmap-shm-fallback`, not QEMU CXL Type-3 — **ADDRESSED**

**Status:** Resolved by the containerised QEMU board (see §Environment Setup).

`setup_qemu.sh` now:
- Uses QEMU 9.x (built by `docker/Dockerfile.qemu` via `scripts/build_qemu.sh`)
  which fully supports `-machine q35,cxl=on`.
- Boots an Ubuntu 24.04 cloud image (kernel 6.8-HWE) that ships
  `CONFIG_CXL_BUS`, `CONFIG_CXL_MEM`, `CONFIG_DEV_DAX` out of the box —
  no custom kernel build required.
- Exposes a 2 GB `cxl-type3` device backed by `cxl_mem_file`.
- `scripts/in_vm_setup.sh` runs `cxl create-region` + `daxctl
  reconfigure-device --mode system-ram` to surface `/dev/dax0.0` inside the VM.

`paper/results/emulation_mode.txt` will report `qemu-cxl-type3` after a
successful `in_vm_setup.sh` run.

---

### GAP-9 · NUMA sweep: 142 ns and 255 ns runs use the same `mbind` binding

**File:** `scripts/numa_sweep.sh`

```bash
numactl --cpunodebind=0 --membind=1 \   # labelled 142 ns
    ./measurement/measure ...
numactl --cpunodebind=0 --membind=1 \   # labelled 255 ns — SAME binding
    ./measurement/measure ...
```

Both runs use `--membind=1` and will produce identical results on any system.
The script needs a distinct mechanism to inject higher latency for the 255 ns
point (e.g., a different NUMA socket, `numactl --preferred`, or a CXLMemSim
delay parameter).

**Fix (single-socket fallback):** Use `numactl --interleave=all` for the 255 ns
label as a coarse proxy, document the limitation, and note that real
differentiation requires CXLMemSim or a dual-socket host.

---

### GAP-10 · No NUMA sweep results exist — Stage 3 not started

**Files:** `paper/results/numa_latency_*.csv` — none of these files exist.

`plot_results.py:plot_numa_sensitivity()` silently skips the figure when files
are absent. The latency sensitivity analysis called out in the research as the
key step that "elevates the work from a functional demo to a defensible systems
paper" (Stage 3, Recommendation 3) has not been run.

**Fix:** Run `scripts/numa_sweep.sh` after GAP-9 is resolved. On a single-NUMA
machine, annotate results as "local DRAM only — no cross-socket emulation
available on this host" and treat Stage 3 as a workstation-only milestone.

---

### GAP-11 · No CXLMemSim or Mess integration

The research identifies CXLMemSim (arXiv:2303.06153, UCSC) and Mess (BSC, MICRO
2024, 0.4–6% error) as the preferred tools for injecting realistic CXL.mem
load-to-use latency (~150–175 ns) without requiring real hardware.

Neither tool is referenced in any script or Makefile target.

**Fix (minimal):** Add a `scripts/run_cxlmemsim.sh` wrapper that launches
CXLMemSim with the L1 sim as the target process and collects memory-access
latency traces. Reference paper should cite these as the latency model source
rather than NUMA emulation alone.

---

## CORRECTNESS GAPS — Affect functional validation claims

### GAP-12 · IFFT implementation is mathematically wrong

**File:** `l1_sim/fft.c` — `fft_process_internal()`

For `direction < 0`, the implementation conjugates the FFT output:

```c
params->output[i] = conjf(params->output[i]);   // conj(FFT(x))
```

This gives `conj(FFT(x))`, which equals `N * x[-n]` (time-reversed signal), not
the true IFFT. The correct identity is:

```
IFFT(x) = conj(FFT(conj(x))) / N
```

For OFDM demodulation this produces wrong subcarrier-to-sample mapping.

**Fix:**
```c
if (params->direction < 0) {
    for (size_t i = 0; i < N; i++)
        params->input_conjugated[i] = conjf(params->input[i]);
    // run forward FFT on conjugated input, then conjugate output and divide by N
}
```
Or equivalently: run the Cooley-Tukey butterfly with twiddle factors
`exp(+2πi k/N)` instead of `exp(-2πi k/N)` for the IFFT path.

---

### GAP-13 · `ldpc_verify` checks only LSB parity — confirms nothing meaningful

**File:** `l1_sim/ldpc.c`

```c
if ((encoded[i] & 1) != (decoded[i] & 1))
    return -1;
```

The synthetic decoder outputs values in `{0, 1}`. The input generator also
produces `{0, 1}` values (cast from LLR sign). Both always match on LSB. The
`functional_correctness.txt = PASS` result has no evidential value for a paper.

**Fix:** Verify via BER (bit-error rate) at a known SNR. At SNR=10 dB with a
correct decoder and 20 iterations, BER should be < 10⁻⁴. Alternatively,
generate a known all-zero codeword (valid LDPC codeword for any base graph),
encode it, add AWGN, decode, and check that all bits are recovered correctly.

---

## INTEGRATION GAPS — Stage 2 not started

### GAP-14 · No srsRAN/OAI integration

The research Stage 2 roadmap requires replacing the synthetic L1 sim with:
- OAI `nr-softmodem --rfsim --phy-test --noS1`, using the hot-swappable LDPC
  shared library as the interception target, OR
- srsRAN standalone `ldpc_decoder_benchmark` / `pusch_processor_benchmark -m
  latency` binaries.

Neither is present. The `verify.sh` and `run_poc.sh` scripts only exercise the
synthetic `ran_l1_sim` binary.

**Fix:** Add a `scripts/run_srsran_bench.sh` that:
1. Builds srsRAN Project with CMake (`-DENABLE_EXPORT=ON`).
2. Runs `ldpc_decoder_benchmark --nof-repetitions 1000`.
3. Attaches `ebpf/l1_intercept_loader` to the benchmark binary.
4. Compares throughput (Mbps) with and without LD_PRELOAD offload.

---

## PAPER / INFRASTRUCTURE GAPS

### GAP-15 · Latency breakdown figure uses hardcoded representative values

**File:** `measurement/plot_results.py` — `plot_breakdown()`

```python
components = ["ebpf_probe", "ringbuf", "daemon", "compute", "signal"]
values = [1.7, 2.0, 5.0, 120.0, 3.0]   # ← not measured
```

These are assumed values. With the current implementation, the compute component
is ~26 000 µs, not 120 µs. Publishing a hardcoded breakdown chart without
labelling it as "projected/representative" would be misleading.

**Fix:** Measure each component individually and feed the results as CSV inputs
to `plot_breakdown()`. Label as "measured" vs. "projected (real GPU from
arXiv:2602.04652)" explicitly.

---

### GAP-16 · No throughput (Mbps) metric

The research cites 53–131 Mbps/core for AVX-512 LDPC as the key comparison
baseline ("Six Times to Spare," arXiv:2602.04652). The measurement framework
tracks only latency (µs/slot). A paper-quality evaluation needs:

```
throughput_mbps = (num_codeblocks * codeblock_bits) / (total_time_s * 1e6)
```

**Fix:** Add a `--throughput` output column to `measure.c` and a throughput bar
chart to `plot_results.py`.

---

### GAP-17 · `setup_qemu.sh` requires external build artifacts with no provisioning — **ADDRESSED**

**Status:** Resolved by the containerised QEMU board (see §Environment Setup).

Three new scripts replace the old stub:
- `scripts/build_qemu.sh` — builds QEMU 9.x via Docker, extracts binary
  to `$VM_DIR` (~40 MB on host). Run once.
- `scripts/provision_vm.sh` — downloads the Ubuntu 24.04 server cloud
  image, resizes it to a 20 GB sparse qcow2, pre-allocates the 2 GB CXL
  backing file, and builds `seed.iso` from `scripts/cloud-init/user-data`.
- `scripts/cloud-init/user-data` — first-boot script that installs srsRAN
  Project, PoCL, ndctl/daxctl/numactl, and builds the PoC source inside
  the VM. No manual steps required on a fresh clone.

A fresh checkout now boots to a fully provisioned environment with:
```
scripts/build_qemu.sh && scripts/provision_vm.sh && scripts/setup_qemu.sh
```

---

---

## Environment Setup

### Containerised QEMU Board (added 2026-06-12)

The PoC now has a fully automated environment build. The host runs Docker only
to compile QEMU 9.x; all simulators run inside a single QEMU VM that behaves
as the "board."

**Host requirements:** Ubuntu 22.04+, Docker 20+, KVM (`/dev/kvm`), ~5 GB free
on `/mnt/devdata`.

**Disk warning:** `/mnt/devdata` has ~6.3 GB free (98% used at time of writing).
The qcow2 + CXL backing file occupy ~4.1 GB initially; the qcow2 grows to ~7 GB
after srsRAN build inside the VM. Free at least 8 GB before running.

**One-time setup:**
```bash
# 1. Build QEMU 9.x (~20-30 min, Docker required)
./scripts/build_qemu.sh

# 2. Download Ubuntu 24.04 cloud image + create seed.iso
./scripts/provision_vm.sh

# 3. Boot the VM (first boot provisions all sims ~20 min)
./scripts/setup_qemu.sh
```

**Every run (inside the VM):**
```bash
# CXL device bring-up (once per VM boot)
sudo /opt/cxl_ran_poc/scripts/in_vm_setup.sh

# Run the full pipeline
/opt/cxl_ran_poc/scripts/in_vm_run.sh --slots 1000
# or with real srsRAN benchmarks:
/opt/cxl_ran_poc/scripts/in_vm_run.sh --slots 1000 --srsran
```

**Simulator inventory inside VM:**

| Simulator | Version | Role |
|-----------|---------|------|
| QEMU CXL Type-3 | 9.x (built by Docker) | CXL device + kernel driver path |
| Ubuntu 24.04 kernel | 6.8-HWE (from Ubuntu pkg) | CXL drivers, eBPF USER_RINGBUF |
| srsRAN Project | release_24_10 | Real BG1/BG2 LDPC L1 workload |
| PoCL 1.8 | Ubuntu package | OpenCL CPU runtime for GPU daemon |
| CXLMemSim | latest main | Stage 3 latency model |
| ndctl / daxctl | Ubuntu package | CXL region management |
| numactl | Ubuntu package | NUMA binding for latency sweep |

**Gaps addressed by this environment:** GAP-8, GAP-17.  
**Gaps not yet addressed by environment alone:** GAP-1 through GAP-7, GAP-9
through GAP-16 (see individual gap entries above).

---

## Prioritised Fix Roadmap

### Sprint 1 — Unblock Stage 1 claims

| # | Gap | Fix summary | Effort |
|---|-----|-------------|--------|
| GAP-3 | CSV outlier corruption | Clamp in `timespec_diff_us`; filter in `generate_notes.py` | 30 min |
| GAP-12 | Wrong IFFT | Fix twiddle sign or conjugate input | 1 h |
| GAP-13 | `ldpc_verify` trivial | Add BER check at known SNR | 2 h |
| GAP-4 | eBPF overhead mismatch | Add no-op uprobe microbenchmark | 2 h |
| GAP-7 | Per-event socket connect | Persistent socket in loader | 1 h |
| GAP-1 | Synthetic LDPC | Port BG1 LUT min-sum from srsRAN | 1–2 days |
| GAP-2 | Zero offload speedup | Use fewer iterations in daemon | 1 h |

### Sprint 2 — Architectural integrity

| # | Gap | Fix summary | Effort |
|---|-----|-------------|--------|
| GAP-5 | Disconnected eBPF/LD_PRELOAD | Document clearly OR implement uretprobe redirect | 1–3 days |
| GAP-6 | No USER_RINGBUF | Add `BPF_MAP_TYPE_USER_RINGBUF` completion map | 1 day |
| ~~GAP-8~~ | ~~No QEMU CXL boot~~ | ✅ Done — `build_qemu.sh` + `provision_vm.sh` + `setup_qemu.sh` | — |
| ~~GAP-17~~ | ~~No QEMU artifacts~~ | ✅ Done — `provision_vm.sh` + cloud-init `user-data` | — |

### Sprint 3 — Paper quality

| # | Gap | Fix summary | Effort |
|---|-----|-------------|--------|
| GAP-9 | NUMA sweep bug | Fix binding OR add CXLMemSim | 2 h–1 day |
| GAP-10 | No NUMA results | Run sweep on workstation | 1 h |
| GAP-11 | No CXLMemSim | Add wrapper script | 1–2 days |
| GAP-14 | No srsRAN integration | Add `run_srsran_bench.sh` | 1–2 days |
| GAP-15 | Hardcoded figure | Measure components individually | 2 h |
| GAP-16 | No Mbps metric | Add throughput column + chart | 1 h |

---

## Key Numbers Reference

| Metric | Research target | Current measurement | Status |
|--------|----------------|--------------------|---------| 
| LDPC codeblock latency | ~0.71 ms (arXiv:2602.04652) | 26.4 ms | ❌ 37× too slow |
| eBPF uprobe overhead | ~1.7 µs (Cloudflare ebpf_exporter) | 1487 µs mean / 1768 µs p95 (measures LDPC compute, not probe cost — GAP-4) | ⚠️ Wrong metric |
| Slot deadline miss rate (offload) | < 100% | 100% | ❌ |
| Emulation mode | QEMU CXL Type-3 | `qemu-cxl-system-ram-node1` — DAX in system-ram mode; CXL memory online as NUMA node 1 (2 GiB), shared mmap with `MPOL_PREFERRED` binding | ✅ Full CXL kernel driver path exercised |
| NUMA sensitivity data points | 3 (0 / 142 / 255 ns) | 0 | ❌ Missing |
| Functional correctness | Bit-exact LDPC through offload | LSB-only check (trivial pass) | ❌ Meaningless |
| Stage 2 (srsRAN/OAI) | OAI rfsim + LDPC interception | Not started | ❌ |
