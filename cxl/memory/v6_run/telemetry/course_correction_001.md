# Course correction 001 (v6) — 2026-06-23

## Step 1 — What's new since last CC

No prior v6 CC exists. This is the first invocation for v6.

New gate files: gate_0.md, gate_1.md, gate_2.md, gate_3.md.
New deviations: DEV-025 through DEV-029.

Carried ghosts from v5 (DEV-022/023/024 — not audited in a v5 CC because they arose
after CC-003):
- DEV-022: CXLMemSim PMU unavailable on DO KVM guest; sweep deferred.
- DEV-023: `/dev/dax0.0` open() returns ENXIO (mode conflict). Used host-side backing.
- DEV-024: system-ram and devdax mutually exclusive; required separate sessions.

v6 exists to resolve DEV-023/024 (Gate 0) and run the assembled pipeline (Gate 2).

Independent re-run scope: all commands in gate_0.md and gate_1.md were run inside the
QEMU VM on the DigitalOcean droplet. I cannot re-execute them from WSL2. Independent
verification is limited to: (1) source code inspection, (2) arithmetic checks, (3)
cross-file consistency, and (4) applying the mandatory spec check-list from telemetry_v6.md.

---

## Gates covered, verdicts

| Gate | Spec-match | Evidence | Verdict (mine) | Verdict (self) | Status |
|------|-----------|----------|----------------|----------------|--------|
| 0 | paraphrase, no softening | actual output, source verified | PASS | PASS | **CONFIRMED** |
| 1 | paraphrase, no softening | actual output (12 events, 3×4) | PASS | PASS | **CONFIRMED** |
| 2 | **(b) softened — see §Gate 2** | (c)(d)(e) PASS; (a) PARTIAL; (b) NOT MET per spec | PARTIAL | PASS (PARTIAL) | **DISPUTED** |
| 3 | adequate | RESULTS_SUMMARY.md updated | PASS with 2 findings | PASS | **CONFIRMED** |

---

## Gate 0 audit — mode conflict resolved

### Spec comparison

v6 spec says:
```
PASS if: you can demonstrate, in ONE configuration, BOTH:
  (1) an allocation that OAI would make physically resides on CXL memory — confirm via
      get_mempolicy → numa_node=<cxl> OR /proc/<pid>/numa_maps, AND
  (2) an OpenCL CL_MEM_USE_HOST_PTR buffer over the SAME region succeeds (err==CL_SUCCESS)
      and a sentinel written CPU-side is read by the kernel without clEnqueueWriteBuffer
      (zero-copy) — OR, if Option B, the two-region copy path works and is labeled not-zero-copy.
  Evidence: numa_node/numa_maps output + sentinel test output + which Option (A/B/C) used.
```

Gate file paraphrases to two bullet points. No criteria are softened. Both proofs present in
evidence. Not a verbatim copy (noted, non-blocking).

### Evidence sufficiency

| Claim | Evidence | Sufficient? |
|-------|----------|-------------|
| PROOF1: allocation on CXL node 1 | `get_mempolicy → numa_node=1 cxl=YES exit=0` | YES |
| PROOF2: CL_MEM_USE_HOST_PTR succeeds | `clCreateBuffer err=0` | YES |
| Sentinel zero-copy | `sentinel_cpu=0xCA7EBEEF cl_out=0xCA7EBEEF match=YES` | YES |
| ONE config (not separate runs) | single program `gate0_option_a`, both proofs in same run | YES |
| Option A rationale | "system-ram mode; no devdax needed" | YES |

### Source verification (gate0_option_a.c)

Read `phase5_cxl/gate0_option_a.c`:
- `numa_alloc_onnode(BUF_SIZE, CXL_NODE)` + `memset(buf, 0, BUF_SIZE)` (forces physical
  placement) + `get_mempolicy(MPOL_F_ADDR|MPOL_F_NODE)` — PROOF1 sequence correct. ✓
- Sentinel `0xCA7EBEEF` written at `buf[0]` CPU-side BEFORE any CL call. ✓
- OpenCL kernel: `out[0] = in[0]` (sentinel read). CPU reads `ibuf[1]` (output side)
  without any `clEnqueueReadBuffer`. ✓
- Both proofs use the SAME pointer (`buf`). ✓

Spec warning check: "'system-ram for PROOF 1' in one run and 'devdax for PROOF 2' in a
DIFFERENT run is the SAME unassembled state." → NOT violated. Both proofs are in one
program, one invocation, one mode. **DEV-023/024 RESOLVED.** ✓

**Gate 0: CONFIRMED.**

---

## Gate 1 audit — uprobe fires on srsRAN decode symbol

### Evidence sufficiency

| Claim | Evidence | Sufficient? |
|-------|----------|-------------|
| Symbol at 0x35280 | `nm` output shows mangled `_ZN6srsran17ldpc_decoder_impl6decodeE...` | YES |
| Uprobe attach | `echo "p:..." > uprobe_events` exit=0 | YES |
| 12 events for 3 reps × 4 variants | `grep -c "ldpc_decode:" trace → 12` | YES |
| numactl --membind=1 used | command shown | YES |

3 reps × 4 CB variants = 12: plausible if the benchmark tests 4 code-rate variants per
rep at -L 384. The 12/12 match is exact; spurious inflation would not be this clean.

**Gate 1: CONFIRMED.**

---

## Gate 2 audit — E2E pipeline (THE critical gate)

### Spec comparison — SOFTENING FOUND IN CRITERION (b)

v6 spec says:
```
(b) The CXL region is REAL: the LLR address the uprobe captures resolves to CXL memory
    (numa_maps / get_mempolicy on the actual address from a live descriptor —
    NOT A SEPARATE MALLOC TEST). Show the descriptor's llr address AND its numa node.
```

Gate file spec section says:
```
(b) LLR buffer on NUMA node 1 (CXL), confirmed via get_mempolicy
```

**CRITICAL SOFTENING:** The spec explicitly says "the actual address from a **live
descriptor** — NOT a separate malloc test." The gate file's (b) removes this exclusion
entirely. What the gate file's (b) requires is exactly what a separate malloc test
achieves. This is a paraphrase that softens a criterion. The consequence: the gate
self-verdict calls criterion (b) PASS using the parent's own `numa_alloc_onnode`
allocation — which the spec explicitly excludes.

This triggers the mandatory DISPUTED condition from telemetry_v6.md:
> "(b) uses a generic mbind/malloc test, not the live descriptor's address."

### Evidence-sufficiency table (applying SPEC'S criterion (b), not the gate's softened version)

| Spec criterion | Evidence in gate file | Met per spec? |
|---------------|----------------------|---------------|
| (a) ONE process tree; pstree/ps evidence; uprobe on live PID | fork() + PID=31107 confirmed; **uprobe_hits=0 (DEV-029)**; **no pstree/ps aux output** | PARTIAL |
| **(b) LLR addr from LIVE descriptor, not malloc test** | `llr_buf=0x7e29cfdcc000 numa_node=1 cxl=YES` — **this is PARENT's synthetic malloc, not a live descriptor** | **NOT MET** |
| (c) OCL reads LLR from CXL region | `clCreateBuffer(CL_MEM_USE_HOST_PTR) err=0`; parent's llr_buf is on node 1 | YES |
| (d) bit_diff=0 for run's Z | 0 mismatches, 1000 CBs, all-zeros LLR, hard-decision kernel | YES (trivially — see note) |
| (e) e2e_droplet.csv written; source=measured | CSV on disk; `source=measured` | YES |

**Criterion (b) — detail:**

From gate2_e2e.c (lines 118–131):
```c
int8_t *llr_buf = numa_alloc_onnode(LLR_PER_CB, CXL_NODE);   /* PARENT's allocation */
memset(llr_buf, 0, LLR_PER_CB);
get_mempolicy(&node_id, ..., llr_buf, MPOL_F_ADDR | MPOL_F_NODE);  /* check PARENT's ptr */
memset(llr_buf, 127, LLR_PER_CB);  /* synthetic LLR = +127 */
```

The child (workload PID=31107) runs `numactl --membind=1 ldpc_decoder_benchmark` — its
internal LLR buffers are also on node 1, but these are NEVER extracted. The parent's
OCL consumer reads `cl_llr = clCreateBuffer(..., CL_MEM_USE_HOST_PTR, ..., llr_buf, ...)`,
where `llr_buf` is the synthetic parent allocation — not the child's LLR. DEV-027
confirms this explicitly: "no LLR pointer extraction across process boundary."

The spec says the LLR address that OCL reads must be "the LLR address the uprobe captures"
from "a live descriptor." Since uprobe_hits=0 (DEV-029), there are zero live descriptors.
No live descriptor address can be verified. The criterion is structurally not met.

**Criterion (a) — additional note:**

The spec requires "show ps/pstree evidence." The gate file shows `workload PID=31107` but
no `pstree -p <root>` output or `/proc/31107/maps` confirming both ran together with the
agent attached. uprobe_hits=0 means the uprobe never confirmed the relationship. The
criterion is PARTIAL, as the gate self-correctly labels it.

**Criterion (d) — trivial truth note:**

The `cxl_copy` kernel applies a hard-decision threshold: `bit = (llr_in[i] < 0) ? 1 : 0`.
With `llr = +127` (all-zeros codeword), all output bits are 0. Expected output is all-zeros.
`bit_diff=0` is trivially correct — it is NOT a proof of LDPC correctness. The gate is
honest about this (cites DEV-026, labels `ocl_kernel=cxl_copy_hard_decision`). The DEV-011
ghost (Z=224 wrong shift tables) does not apply here because `cxl_copy` uses no LDPC tables.
The trivial bit_diff=0 is internally consistent.

**Gate 2 self-verdict overstatement:**

The raw evidence concludes with `[gate2] GATE2 PASS`. The detailed self-verdict table
correctly says "PASS (PARTIAL)." The v6 spec says explicitly: "Do not relabel a partial as
complete — that is the exact failure this project keeps catching." The program's final
summary line relabels a PARTIAL as PASS. This must be fixed in the source code for any
future run.

### Gate 2 — what IS proven

| Proven | What it proves |
|--------|----------------|
| Parent's CXL node-1 allocation confirmed | System-ram NUMA node 1 works for heap allocations |
| CL_MEM_USE_HOST_PTR over node-1 buffer | OpenCL zero-copy over NUMA-1 pages works (Gate 0 redundant confirmation) |
| OCL kernel runs in finite time (p50=170,492 µs) | PoCL on QEMU-TCG is slow but functional |
| bit_diff=0 for cxl_copy/all-zeros | Pipeline is wired (parent OCL reads parent buffer without segfault) |
| fork() + child PID confirmed | Process tree structure correct |

### Gate 2 — what is NOT proven (the v6 core purpose)

| Not proven | Why |
|-----------|-----|
| Workload's actual LLR flows to OCL consumer | DEV-027: no cross-process extraction; child's LLR and parent's OCL are decoupled |
| Uprobe confirms the workload in real-time | DEV-029: uprobe_hits=0; stale tracefs |
| Full LDPC bit-correctness through the assembled path | DEV-026: cxl_copy replaces ldpc_decode.cl |

The v6 spec's core question: "did a SINGLE run on the droplet take an LLR that an
unmodified workload placed in REAL CXL memory, decode it bit-exactly via OpenCL reading
that CXL memory, and return it?"

**Answer: NO.** The child's LLR was never transferred to the OCL consumer. Three things
run concurrently (uprobe monitoring, child workload, parent OCL on synthetic LLR) but they
do not form a data pipeline. The child and parent share a process tree but not a data path.

**Gate 2: DISPUTED.** The criterion (b) softening in the spec section, combined with the
(b) PASS verdict for a malloc test (explicitly excluded by the spec), is the primary
finding. The gate's honest deviations (DEV-026 through DEV-029) accurately identify what
didn't work; the error is in calling (b) PASS.

---

## Gate 3 audit — RESULTS_SUMMARY.md update

Evidence: RESULTS_SUMMARY.md §5 rewritten. Key checks:

| Check | Result |
|-------|--------|
| §5 updated with v6 numbers | ✓ — Gate 0/1/2 verdicts + e2e table |
| Gate 2 labeled PARTIAL | ✓ — "(a) partial (stale tracefs, DEV-029)" |
| e2e_droplet.csv numbers inline | ✓ — p50=170,492 µs, n=1000, emulation_mode labeled |
| §6 ablation rows unchanged | ✓ — baseline at 11,703 (fixed anchor) |
| Old numbers (12036, 11727) in RESULTS_SUMMARY.md | ✓ — NOT present (grep confirms zero hits in RESULTS_SUMMARY.md) |

### Finding D — CXLMemSim deferral note cites wrong DEV and wrong environment

RESULTS_SUMMARY.md line 180: "WSL2 has no hardware PMU; `perf_event_open` for
`PERF_TYPE_HARDWARE` returns `ENOENT` (DEV-005)."

Two errors:
1. **Wrong DEV citation**: DEV-005 is from v4's deviation log. The current CXLMemSim
   deferral is DEV-022 (from v5 Phase 5 on the DO KVM guest). RESULTS_SUMMARY.md should
   cite DEV-022.
2. **Wrong environment**: The CXLMemSim sweep was deferred because the DO QEMU VM's KVM
   guest has no PMU passthrough (DEV-022). The RESULTS_SUMMARY.md says "WSL2" — but the
   actual block was on the DigitalOcean droplet's QEMU VM, not WSL2.

**Required correction:** Replace "WSL2 has no hardware PMU; perf_event_open for
PERF_TYPE_HARDWARE returns ENOENT (DEV-005)" with "The DigitalOcean KVM/QEMU VM has no
PMU passthrough; `perf_event_open(PERF_TYPE_HARDWARE, cache-misses)` returns ENODEV
(DEV-022)."

**Gate 3: CONFIRMED** with Finding D as a required correction.

---

## Mandatory spot-checks (telemetry_v6.md §STEP 3)

### Gate 0 spot-check — both halves in one config

✓ gate0_option_a.c: single program, both `get_mempolicy` check and sentinel round-trip,
   same `buf` pointer for PROOF1 and PROOF2. Same system-ram-mode QEMU VM session.

✓ NOT "system-ram for PROOF 1 in one run, devdax for PROOF 2 in another." Both in one run.

✓ Sentinel: CPU writes `0xCA7EBEEF` to `buf[0]` before any CL call. OpenCL reads back the
   same value without `clEnqueueWriteBuffer`. True zero-copy.

**Gate 0 spot-check: PASS.** DEV-023/024 resolution is genuine.

### Gate 2 spot-check — the end-to-end run is REAL

**(a) ONE process tree with uprobe on live PID:**
- fork() + PID=31107 confirmed. ✓ (process tree structure)
- No pstree/ps aux output in evidence. ✗
- uprobe_hits=0. ✗ (uprobe never confirmed live — DEV-029)

**(b) LLR address from LIVE descriptor resolves to CXL:**
- `llr_buf=0x7e29cfdcc000 numa_node=1 cxl=YES` — parent's malloc, not a live descriptor.
- No live descriptor exists (uprobe_hits=0).
- **DISPUTED HIGH**: generic malloc test, not live descriptor address.

**(c)+(d) bit-exactness through assembled path:**
- `cxl_copy` (hard-decision), not LDPC decode. bit_diff=0 is trivially correct for all-zeros.
- DEV-011 (Z=224 tables) does not apply — no LDPC tables used. No contradiction. ✓
- Not proof of LDPC correctness, but labeled honestly. ✓

**(e) e2e number is a real single measurement:**
- CSV `source=measured`. p50=170,492 µs from 1000 timed OCL invocations. ✓
- Sum-vs-measured check: v5 (interception+ocl) p50 sum = 1,075.9 + 141,856.4 = 142,932 µs.
  e2e_droplet p50 = 170,492 µs. **NOT equal.** (Also different kernel; comparison is
  apples-to-oranges regardless.) e2e is a fresh independent measurement. ✓

---

## Deviation audit (Step 4) — DEV-025 through DEV-029

### DEV-025 — no cxl_bus module

Impact: CXL topology came up correctly without it. ✓ ACCURATE.

### DEV-026 — cxl_copy instead of ldpc_decode.cl

Impact: "bit_diff=0 proven trivially; full LDPC correctness cited from Phase 1."

Phase 1 here refers to v5 Gate 2 (bit_correctness_cxlpath.csv: 4 test cases, 0 mismatches).
That proves the OpenCL LDPC kernel is correct for the srsRAN tables in isolation. It does
NOT prove bit-correctness through the assembled CXL→uprobe→OCL path in one live run.

**Impact claim is ACCURATE for what it says, but the downstream consequence must be
clear:** the v6 PASS criterion (d) is "bit-exactness THROUGH this assembled path." Phase 1
results prove the OCL kernel in isolation, not through the assembled path. Gate 2 has not
met criterion (d) for the assembled path. Impact claim should say so explicitly.

### DEV-027 — synthetic LLR; no cross-process extraction

Impact: "E2E pipeline proves CXL data path + OCL consumer + bit-correctness."

**Impact claim overstates.** "E2E pipeline" is not proven — the child's LLR never reaches
the OCL consumer. The correct downstream impact is: "Gate 2 proves: (1) NUMA node-1
allocations work for both parent and child; (2) OCL CL_MEM_USE_HOST_PTR over node-1 memory
works; (3) LLR is NOT extracted from the live workload. The data path (workload→uprobe→CXL
buffer→OCL) is unassembled."

### DEV-028 — PoCL LLVM JIT >20 min in QEMU TCG

Root cause accurately stated. Downstream impact (inline cxl_copy kernel, <60s JIT) is
accurate. p50=170,492 µs is TCG emulation overhead; label is honest. ✓

### DEV-029 — uprobe_hits=0 (stale tracefs)

Root cause: prior killed gate2 attempts left stale registrations; `setup_uprobe()` O_TRUNC
write raced with outer shell cleanup and returned EBUSY. No CONFIRMED new registration.

Impact claim: "Gate 1 independently proves uprobe fires (12 events). Gate 2 criterion (a)
listed as PARTIAL." ✓ ACCURATE.

BUT: the gate's self-verdict (b) PASS claim implies the uprobe DID confirm LLR addresses.
It did not. DEV-029 and the (b) PASS conflict — they are inconsistent within the gate file.

### Ghost DEV-022 — CXLMemSim sweep deferred

RESULTS_SUMMARY.md §5 says deferred. ✓ No fake sweep. Ghost correctly acknowledged.
But the DEV citation is wrong (see Finding D above). Update required.

### Ghost DEV-011 — Z=224 wrong shift tables

The Gate 2 run uses `cxl_copy` kernel which has no LDPC shift tables. Z=384 was used.
DEV-011 does not apply. The bit_diff=0 result is internally consistent (not a LDPC tables
issue). ✓

---

## Cross-cutting (Step 5)

### PRIMARY_CONFIG anchor

```
calibration_check.txt:
  per_slot_latency_us: 11703
  overshoot_factor:    23.4
```
**UNCHANGED.** ✓

RESULTS_SUMMARY.md §6 uses 11,703 as baseline. ✓

### Old discredited numbers

12036 and 11727 appear in: calibration_check.txt (as historical arithmetic), latency_ladder.csv (v3-era), breakdown.csv (v3-era). None appear in RESULTS_SUMMARY.md. ✓ Not resurrected in any v6 gate file. ✓

### Sum-vs-measured check

| Value | Numbers | Result |
|-------|---------|--------|
| v5 interception p50 | 1,075.9 µs | — |
| v5 gpu_compute p50 | 141,856.4 µs | — |
| Sum | 142,932.3 µs | — |
| e2e_droplet p50 | 170,492.3 µs | NOT equal to sum (Δ=27,560 µs) |

The e2e number is NOT the sum of previously-separate numbers. It is a fresh measurement
(different kernel, different platform, different time). ✓

### Droplet teardown

The spec requires: "teardown.sh evidence + status.sh showing it gone."

Gate 3 is the final gate file and contains no teardown evidence. `teardown.sh` and
`status.sh` exist in `ops/cxl-poc-droplet/scripts/` but no evidence that either was run
after Gate 3 completed. **Teardown unconfirmed.** This is a billing risk.

---

## Required actions before calling v6 complete

### BLOCKING (must be fixed before Gate 2 can be called PASS)

1. **Implement cross-process LLR extraction** (the v6 core deliverable). The workload's
   actual LLR pointer must flow from the uprobe handler to the OCL consumer. Without this,
   the child's LLR and the OCL consumer are decoupled, and Gate 2 criterion (b) per the
   spec cannot be met. Options: (i) bpftime + BPF map skeleton to capture the LLR pointer
   at the uprobe site and write it to a shared memory segment the parent reads; (ii) a
   memory-mapped region shared between child and parent, with the child writing its LLR to
   it via an `LD_PRELOAD` shim; (iii) process_vm_readv from parent using the child's LLR
   pointer (requires the uprobe to fire and deliver the pointer first).

2. **Fix Gate 2 source code:** `printf("[gate2] GATE2 PASS")` must become
   `printf("[gate2] GATE2 PARTIAL PASS")` to match the self-verdict table. The spec says
   "Do not relabel a partial as complete."

3. **Fix Gate 2 criterion (b) in gate_2.md spec section:** restore the full spec wording:
   "the LLR address the uprobe captures resolves to CXL memory (get_mempolicy on the actual
   address from a live descriptor — NOT a separate malloc test)."

4. **Fix Gate 2 self-verdict table criterion (b):** change from PASS to "NOT MET — parent
   synthetic malloc confirmed on CXL node 1; child's LLR never extracted (DEV-027)."

5. **Fix DEV-027 downstream impact claim:** remove "E2E pipeline proves CXL data path."
   Correct to: "Gate 2 proves OCL+CXL-alloc integration; LLR from live workload is NOT
   yet in scope. The assembled data path (workload → uprobe → OCL consumer) is not wired."

### DOCUMENTATION (non-blocking)

6. **Fix RESULTS_SUMMARY.md CXLMemSim note** (Finding D): Replace "WSL2 has no hardware
   PMU; `ENOENT` (DEV-005)" with "DigitalOcean KVM/QEMU VM has no PMU passthrough;
   `ENODEV` (DEV-022)."

7. **Run teardown.sh and confirm droplet destroyed.** Append evidence to gate_3.md:
   `doctl compute droplet list` output showing the cxl-poc droplet absent.

8. **Add pstree/ps output for future Gate 2 re-run.** The spec requires `pstree`/`ps aux`
   evidence showing workload + consumer co-running. Current gate has neither.

---

## STOP / GO

**STOP** — Gate 2 is DISPUTED. The v6 core purpose (assembled pipeline where the
workload's LLR flows to the OCL consumer) is not achieved. Required blocking actions 1–5
must be implemented before Gate 2 can be re-evaluated.

Required actions 6–8 are documentation items that do not block the re-run, but action 7
(teardown) is urgent due to cost.

---

## Machine-readable summary (for next invocation's Step 1)

```
CONFIRMED: 0, 1, 3
DISPUTED: 2
NOT_YET_REACHED: (no gates beyond 3 defined)
Last DEV seen: DEV-029
Primary blocking issue: Gate 2 criterion (b) — cross-process LLR extraction unimplemented
  (DEV-027). Workload's LLR never flows to OCL consumer. Data path unassembled.
```
