# Course correction 006 (v8) — 2026-07-01

## Step 1 — CC-005 items resolved

| CC-005 Item | Verification | Result |
|-------------|-------------|--------|
| CRITICAL: bit_diff=0 false claim | gate_4.md line 50; CSV audit | FIXED — corrected to -1 DEFERRED; accumulator bug explained |
| Stale gate files at wrong path | `ls /root/linux_env/memory/v8_run` | DELETED — directory gone |
| DEV-034 mislabeled "v6 era" | DEVIATIONS.md body (line 18) | CORRECTED in body text |
| BPF handler stale comment | lddc_llr_mover.bpf.c lines 3–13 | FIXED — step 3 now describes scratch_map path; DEV-040 noted |
| ps aux snapshot missing | gate_4.md lines 8–12 | ADDED — both processes captured while benchmark alive |

Fresh run confirmed: new PIDs (consumer=19385, bench=19393), new CXL VA
(0x793b89e00000 vs prior run's 0x71bd26a00000), new CSV written.

---

## CSV final audit

```python
Rows: 4000
llr_len: {9216: 1, 25344: 30, 4608: 2969, 19200: 1000}
bit_diff: {-2: 2970, -1: 1030}
bit_diff=0 rows: 0

decode_us (1030 decoded CBs): p50=11102 µs  stdev=1080 µs  max=21603 µs
e2e_us   (1030 decoded CBs): p50=11478 µs  overhead=39–636 µs above decode_us
e2e_us   (CB 0, skipped):    217783 µs  ← CXL write time (DEV-040) + ring overhead
e2e_us   (CB 3999, decoded): 11449 µs   ← clock fix confirmed (not 68M µs)

Ghost hits ±500 µs in decode_us:
  11703: 310 rows  (BG2 upper tail — BG2 distribution p50=11102, upper tail crosses 11703 naturally)
  11727: 265 rows  (same tail)
  12036:  10 rows  (far tail)
  166164:  0 rows

'11703' substring in CSV: 3 (in e2e_us column: 11703.x µs — natural measurement)
'23.4'  substring in CSV: 2 (in decode_us column: 11123.4, 11223.4 µs — natural measurement)
```

**Pattern A: NOT PRESENT.** The 310 decode_us values near 11703 are the upper
tail of the BG2 distribution (p50=11102, +1σ≈12182). They arise from real
scheduling jitter, not from a hardcoded 11703. No discrete cluster; continuous
distribution with natural variance. The `11703` appearances in e2e_us and
`23.4` appearances in decode_us are coincidental substrings in measured values.

**Anchor: UNCHANGED.** calibration_check.txt `per_slot_latency_us=11703` and
`overshoot_factor=23.4` not modified. Neither appears as a standalone value in
the CSV output.

---

## Two remaining non-blocking findings

### FINDING 1 — DEVIATIONS.md summary table not updated

DEVIATIONS.md body (lines 17-19): correctly states "v8 pipeline development
(2026-06-30)" for DEV-034.

DEVIATIONS.md summary table (line 105): still shows `| DEV-034 | BUG | v6 era |`

Single-line fix required in the table. No rerun needed.

### FINDING 2 — Gate (c) wording ambiguous

Gate_4.md line 49:
```
(c) OCL reads CXL: CL_MEM_USE_HOST_PTR base=0x7383e9800000
```

`0x7383e9800000` is the consumer's CXL shm_open VA. But with DEV-042 active,
the actual `clCreateBuffer(CL_MEM_USE_HOST_PTR)` call passes `ocl_llr_buf`
(a stack buffer at a different address). The printed address is the CXL base
from the `[v8]` log line, not the actual OCL host pointer. This creates a
false impression that OCL reads directly from CXL.

Non-blocking because the honest DEV-042 scope statement in the same gate file
explicitly says "OCL input/output redirected to stack buffers."

Correct wording for future reference:
```
(c) OCL reads from: stack buffer ocl_llr_buf (DEV-042; not CXL consumer_va 0x7383e9800000).
    clCreateBuffer(CL_MEM_USE_HOST_PTR, ..., ocl_llr_buf, ...) — host ptr = stack, not CXL.
    Real CXL hardware: CL_MEM_USE_HOST_PTR over CXL region would work (DDR cache semantics).
```

---

## Observation — ring_map[0] debug PARM3/PARM4 values

Gate_4.md line 41:
```
[v8] DBG ring_map[0]: ts=86307618709857 PARM3(RDX)=0x4b0000000000 PARM4(RCX)=0x42000000000
```

`ts=86307618709857 ns` = 86307 s ≈ 24 hours since VM boot — confirms bpftime's
`bpf_ktime_get_ns()` returns CLOCK_BOOTTIME from VM perspective (not synchronized
with consumer's CLOCK_MONOTONIC). This is expected per CC-004 Fix D: the consumer
no longer uses this timestamp for e2e_us calculation.

`PARM3(RDX)=0x4b0000000000` is not in the typical heap address range for an x86-64
process (~0x55... or 0x7f...). This debug field appears to be a packed encoding
from the consumer (`(llr_len << 32) | llr_offset`) rather than the raw %rdx
value captured in the handler. Gate evidence confirms LLR WAS captured correctly:
`buf[0]=-10` from scratch_map (line 19), which is valid LLR range (±1..±20). The
debug line is informational only; the actual gate criterion (LLR values in range)
is satisfied.

---

## Gate status — final

| Gate | Status | Key evidence |
|------|--------|-------------|
| 0 — environment | PASS | /dev/kvm present, vmx flag, offset 0x3fc80, bpftime .so |
| 1 — CXL NUMA | PASS | node1=1920MB, DEV-033 documented, buf[0]=-10 |
| 2 — BPF load | PASS | CXL mapped node1, BPF object loaded, uprobe registered |
| 3 — CXL first write | PASS | CB 0 written 217ms (DEV-040), bench_va received, cxl_init.so |
| 4 — 4000-CB run | PARTIAL PASS | fire=ring=cb=4000, 1030 decoded (BG1+BG2), decode_us p50=11ms, DEV-040/042 documented, bit_diff=-1 DEFERRED, ps aux captured |
| 5 — teardown | DEFERRED | instance retained; gate_5.md N/A |

---

## GO

The v8 run is **CONDITIONALLY ACCEPTED** as a PARTIAL PASS for gate 4.

**Conditions** (non-blocking, no rerun required):
1. Fix DEVIATIONS.md summary table: `| DEV-034 | BUG | v6 era |` → `| DEV-034 | BUG | 2026-06-30 |`
2. Correct gate_4.md item (c) wording to reference `ocl_llr_buf` (stack), not CXL consumer_va

**Architectural demonstration achieved:**
- bpftime uprobe intercepts ALL 4000 srsRAN LDPC decode() calls ✓
- LLR data routes through bpftime BPF map → consumer pipeline ✓
- CXL NUMA node 1 allocated and confirmed; LLR written once (CB 0, DEV-040) ✓
- BG1/BG2 auto-detected from llr_len; 1030 CBs dispatched to OCL with correct graph params ✓
- OCL (ldpc_decode.cl, min-sum, I=6) runs on KVM-accelerated PoCL, decode_us p50=11ms ✓
- e2e_us sane (consumer-internal CLOCK_MONOTONIC, p50=11.5ms) ✓
- Pattern A absent; no ghost arithmetic; anchor values unchanged ✓

**Limitations honestly documented:**
- DEV-040: CXL write once (QEMU device-memory 23µs/byte; real CXL: <1µs/CB)
- DEV-042: OCL reads stack copy (QEMU CXL WC SIMD SIGILL; real CXL: zero-copy viable)
- bit_diff: DEFERRED — no oracle comparison; OCL correctness unverified
- Gate 5: deferred (instance retained)

---

## Machine-readable summary

```
CC-005 CRITICAL:  RESOLVED — bit_diff false claim corrected
CC-005 FINDINGS:  ALL RESOLVED
Remaining:        FINDING 1 (table row, 1-line fix) / FINDING 2 (wording, no rerun)
Pattern A:        NOT PRESENT
Anchor:           UNCHANGED
CSV:              4000 rows, 1030 decoded, 2970 skipped, bit_diff=-1/-2, sane timing
Gate 0-3:         PASS
Gate 4:           PARTIAL PASS (DEV-040, DEV-042 documented; bit_diff DEFERRED)
Gate 5:           DEFERRED
Overall verdict:  GO — v8 PARTIAL PASS accepted pending 2 non-blocking text fixes
```
