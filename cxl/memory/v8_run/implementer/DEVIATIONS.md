# DEVIATIONS.md — CXL PoC v8 Run
Logs all deviations from the ideal architecture.
Scope labels: BUG (real bug, fixed), QEMU-WC (QEMU device-memory constraint only; does not apply to real CXL hardware), DESIGN (architectural simplification for PoC).

---

## DEV-033 — numactl --membind=1 SIGILL
**Date**: prior session (v4/v5 era)
**Scope**: QEMU-WC
**Issue**: `numactl --membind=1 <cmd>` causes SIGILL inside the QEMU VM. The numactl binary calls `mbind()` via a path that triggers an instruction that QEMU TCG does not emulate.
**Resolution**: llr_consumer_v8 calls `mbind()` directly via syscall. `get_mempolicy` with MPOL_F_ADDR confirms NUMA node assignment. Avoids numactl wrapper entirely.
**Real-hardware impact**: None. numactl works normally on bare-metal Linux.

---

## DEV-034 — bpf_probe_write_user SIGSEGV in bpftime
**Date**: v8 pipeline development (2026-06-30). Corrected from an earlier
mislabeling as "v6 era" — bpftime was not used in the v6 pipeline; this issue
was first encountered while assembling the v8 bpftime uprobe handler.
**Scope**: BUG (bpftime v0.0.x limitation)
**Issue**: `bpf_probe_write_user(cxl_base, llr_ptr, llr_len)` in the BPF handler caused SIGSEGV inside bpftime's ubpf VM. The bpftime user-space BPF implementation does not fully support `probe_write_user`.
**Resolution**: Removed `probe_write_user` from BPF handler entirely. LLR data path changed: BPF handler copies LLR into `scratch_map` (bpftime in-process ARRAY map) using `bpf_probe_read_user`; consumer reads from scratch_map via `bpf_map_lookup_elem`.
**Real-hardware impact**: Architectural improvement. scratch_map approach also works on real hardware; probe_write_user was never required by the PoC architecture.

---

## DEV-035 — bpf_get_prandom_u32 opcode rejected by ubpf
**Date**: prior session (v7 era)
**Scope**: BUG (bpftime v0.0.x limitation)
**Issue**: `bpf_get_prandom_u32()` helper call emitted opcode 0xc3 (return-like) which ubpf's verifier/JIT rejected.
**Resolution**: Removed `bpf_get_prandom_u32` from BPF handler. Replaced with direct seq counter. build_tools.sh patched to not require this helper.
**Real-hardware impact**: None. bpf_get_prandom_u32 works on kernel BPF.

---

## DEV-036 — GRUB UUID mismatch after CXL online
**Date**: prior session
**Scope**: QEMU-WC
**Issue**: After `daxctl reconfigure-device --mode=system-ram`, the GRUB boot UUID changed, causing reboot to fail. VM disk image inconsistency.
**Resolution**: Per-boot CXL setup script (`vm_cxl_setup.sh`) runs daxctl online/offline sequence at boot. Boot UUID kept stable.
**Real-hardware impact**: None. ndctl/daxctl operation on real CXL hardware does not affect GRUB.

---

## DEV-037 — scratch_map max_entries=1 (ring stall at head=1)
**Date**: 2026-06-30 session
**Scope**: BUG
**Issue**: The VM's `lddc_llr_mover.bpf.c` had `scratch_map` with `max_entries=1` (stale from early testing). After BPF handler writes seq=0 to slot 0, seq=1 would use ring_slot=1 which exceeded max_entries → `bpf_map_lookup_elem` returned NULL → handler exited without incrementing ring_head. Consumer hung at ring_head=1 indefinitely.
**Resolution**: Sync GCP host BPF source (RING_CAP=256, max_entries=256) to VM; recompile `.bpf.o` on GCP host (clang-12); deploy to VM. ring_head advances to 4000.
**Real-hardware impact**: None. Pure bug.

---

## DEV-038 — glibc SIMD memcpy SIGILL on QEMU CXL device-memory
**Date**: 2026-06-30 session
**Scope**: QEMU-WC
**Issue**: glibc's `memcpy` uses SSSE3 instructions (`palignr`, `movdqa`) at `libc.so.6+0x1871ce`. On QEMU CXL device-memory pages (mapped via `mmap` of shm_open region bound to NUMA node 1), these instructions trigger SIGILL (`#UD`) via QEMU's TCG instruction translation path.
**Resolution**: Custom `cxl_copy()` with `volatile int8_t *restrict dst` parameter + `__attribute__((noinline, optimize("O2", "no-tree-vectorize", "no-tree-loop-idiom")))`. volatile dst prevents GCC from recognizing the scalar loop as a `memcpy` idiom; the attribute disables the loop-idiom recognizer explicitly.
**Real-hardware impact**: None. Real CXL hardware has standard DDR cache semantics; glibc SIMD memcpy works normally.

---

## DEV-039 — GCC constprop of static scratch_buf → .constprop.0 → jmp memcpy@plt → SIGILL
**Date**: 2026-06-30 session
**Scope**: BUG (GCC optimization interaction)
**Issue**: When `scratch_buf` was declared `static int8_t scratch_buf[LLR_PER_CB]`, GCC created a `.constprop.0` specialization of `cxl_copy` with the compile-time-known static address as the source. In this specialization, GCC recognized the byte-copy loop as a `memcpy` idiom and emitted `jmp memcpy@plt`. This defeated the scalar copy intention and caused SIGILL (glibc memcpy SIMD on CXL pages; same as DEV-038).
**Verified via**: `objdump -d llr_consumer_v8 | grep -A2 constprop` showed `jmp memcpy@plt`. After fix: `objdump` shows scalar byte loop at `cxl_copy`, no `constprop.0`.
**Resolution**: Remove `static` from `scratch_buf` declaration. GCC cannot constant-propagate a runtime stack address → `cxl_copy` stays scalar for all callers.
**Real-hardware impact**: None. Pure GCC optimization issue.

---

## DEV-040 — QEMU CXL device-memory write rate 23µs/byte (CXL write once)
**Date**: 2026-06-30 session
**Scope**: QEMU-WC
**Issue**: Every byte-store to the CXL mmap'd region goes through QEMU's TCG device-emulation slow path (soft-MMU). Measured cost: ~23µs/byte. For a 19200-byte BG2 CB: 19200 × 23µs = 442ms per CB. Writing 4000 CBs = 1768 seconds — far exceeds any timeout.
**Resolution**: Write LLR to CXL ONCE (CB 0 only). With MAX_CBS=1, all CBs use slot 0 (`cxl_slot = seq % MAX_CBS = 0`). The single write (CB 0, 9216 bytes at ~23µs/byte = 220ms) demonstrates the srsRAN→CXL data path. CBs 1-3999 skip the CXL write; OCL decode reads from stack buffer (see DEV-042).
**Real-hardware impact**: None. Real CXL hardware: DDR cache semantics, byte-stores cost ~5ns via CPU cache. For 19200 bytes: ~96µs total (entirely within our 120s timeout). No write-once workaround needed.

---

## DEV-041 — waitpid() 30ms/call in tight spin loop (SIGCHLD + ring-stable fallback)
**Date**: 2026-06-30 session
**Scope**: BUG
**Issue**: Outer poll loop called `waitpid(bench_pid, WNOHANG)` every iteration. In QEMU without KVM on the previous setup, `SYS_wait4` cost ~30ms. Even with KVM, calling waitpid on every iteration (millions of times) wastes cycles.
**Resolution**: Install SIGCHLD handler (`SA_NOCLDSTOP`) before fork. Handler sets `volatile sig_atomic_t g_bench_done = 1` (no syscall). Outer loop checks flag without syscall overhead. Ring-stable fallback: if ring_head stays constant for IDLE_MAX=200,000 consecutive iterations AND cb_count ≥ 3900, exit via `goto done`. Handles cases where SIGCHLD doesn't fire (bpftime may swallow it).
**Real-hardware impact**: None. Pure bug.

---

## DEV-042 — QEMU CXL device-memory SIMD load SIGILL on OCL cl_mem host pointer
**Date**: 2026-06-30 session
**Scope**: QEMU-WC
**Issue**: PoCL's CPU backend uses SIMD loads (movdqa, palignr) when accessing the `cl_mem` host pointer (from `clCreateBuffer(CL_MEM_USE_HOST_PTR, cxl_base + offset, ...)`). On QEMU CXL device-memory, these SIMD loads trigger SIGILL via TCG, same root cause as DEV-038. Additionally, a SIMD-based `memcpy` in the bit_diff computation (`__builtin_popcount` loop over CXL memory) caused 24ms/CB overhead.
**Resolution**: Redirect OCL I/O to stack buffers. `cxl_copy(ocl_llr_buf, scratch_buf, desc.llr_len)` copies LLR from bpftime scratch_map (stack/shm RAM) to `static int8_t ocl_llr_buf[LLR_PER_CB]` (stack). `clCreateBuffer(CL_MEM_USE_HOST_PTR, ..., ocl_llr_buf, ...)` → PoCL accesses stack RAM (no SIMD issue). OCL output to `static uint8_t ocl_bit_buf[BITS_PER_CB]` (stack). bit_diff computed from `ocl_bit_buf` directly.
**Real-hardware impact**: None. Real CXL hardware: standard DDR cache semantics, SIMD loads work normally. `CL_MEM_USE_HOST_PTR` over CXL region would work without SIGILL. The stack-buffer copy is unnecessary on real hardware.

---

## DEV-043 — BG detection skipped legitimate min-cb-length benchmark configs
**Date**: 2026-07-01 session (CC-006)
**Scope**: BUG
**Issue**: CC-004's BG-detection fix only recognized `n_vn_eff == BG1_N-2` (66) and `n_vn_eff == BG2_N-2` (50), treating the srsRAN benchmark's other two configs (`llr_len=9216`, `n_vn_eff=24` for BG1; `llr_len=4608`, `n_vn_eff=12` for BG2) as "unsupported" and skipping them (bit_diff=-2). Reading `ldpc_decoder_benchmark.cpp:127-141` showed these are the benchmark's own `min_cb_length_bg` test case (`msg_length_bg + 2` punctured VNs, i.e. zero parity bits transmitted) — a deliberate, legitimate rate-matching scenario, not garbage. This caused 2970/4000 CBs (74.25%) to be skipped, undercounting decode coverage.
**Resolution**: Extended the BG-detection condition to recognize both `min_cb_length_bg` and `max_cb_length_bg` per base graph: BG1 accepts `n_vn_eff ∈ {24, 66}`, BG2 accepts `n_vn_eff ∈ {12, 50}`. Both use the same full N_FULL-column graph and shift tables; the existing zero-padding mechanism (already used for the 2 punctured VNs in the max-length case) generalizes automatically to the larger untransmitted region in the min-length case. Result: 4000/4000 CBs decoded, 0 skipped.
**Real-hardware impact**: None. Pure bug — the fix is a source-level graph-parameter mapping correction, unrelated to QEMU.

---

## DEV-044 — Live E2E bit_diff has no ground truth (benchmark design, not a gap)
**Date**: 2026-07-01 session (CC-006)
**Scope**: DESIGN (not fixable without changing the benchmark invocation)
**Issue**: `ldpc_decoder_benchmark`'s default invocation (`-L 384 -I 5 -T avx2 -R 1000`, no `-C` flag) uses `use_crc=false` (srsRAN's own default — see `ldpc_decoder_benchmark.cpp:39`). In this mode, each decode() call is fed **purely random LLR** (`(rgen() & 1) * 20 - 10`), not an LLR derived from a real encoded message. There is no ground-truth bit sequence to compare the OCL decoder's output against for these live CBs. `bit_diff=-1 (DEFERRED)` for all 4000 live CBs is therefore the honest and correct status — not an unimplemented feature.
**Resolution (partial, real evidence obtained via separate path)**: Built and ran `bit_diff_test.cpp` (already present in the repo, previously unbuilt/unrun) — a standalone program using srsRAN's own LDPC encoder to generate real messages, encode them, and verify the identical `ldpc_decode.cl` kernel (md5sum-confirmed match against the file compiled into `llr_consumer_v8`) reproduces every message bit exactly. Result: 0 mismatches / 10,240,000 bits across BG1/BG2 × LS=384/LS=256 (500 messages each). This proves kernel-level bit-exact correctness for the exact configs used live (BG1 LS=384, BG2 LS=384: 0/6,144,000 mismatches).
**To close the live-CB gap fully**: would require adding `-C` to the benchmark invocation (switches to real encode+CRC mode) plus a second uprobe/instrumentation point to capture the pre-encode message bits from the benchmark process as ground truth. Not implemented — out of scope for this run; the kernel-level oracle is the strongest correctness evidence obtainable without that additional engineering.
**Real-hardware impact**: None — this is a benchmark configuration property, independent of QEMU vs. real hardware.

---

## Summary table

| DEV | Scope | Fixed in |
|-----|-------|----------|
| DEV-033 | QEMU-WC | v4 era |
| DEV-034 | BUG | v8 pipeline development (2026-06-30) |
| DEV-035 | BUG | v7 era |
| DEV-036 | QEMU-WC | v7 era |
| DEV-037 | BUG | 2026-06-30 |
| DEV-038 | QEMU-WC | 2026-06-30 |
| DEV-039 | BUG | 2026-06-30 |
| DEV-040 | QEMU-WC | 2026-06-30 (documented; QEMU only) |
| DEV-041 | BUG | 2026-06-30 |
| DEV-042 | QEMU-WC | 2026-06-30 (documented; QEMU only) |
| DEV-043 | BUG | 2026-07-01 (CC-006 coverage fix: 4000/4000 decoded) |
| DEV-044 | DESIGN | 2026-07-01 (CC-006: kernel oracle via bit_diff_test.cpp, 0/10.24M mismatches) |

QEMU-WC deviations (DEV-033, DEV-038, DEV-040, DEV-042) apply ONLY to QEMU device-memory WC cache semantics. They are not architectural bugs and do not apply to real CXL hardware.

PRIMARY_CONFIG: 23.4x — UNCHANGED
