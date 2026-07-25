# Course correction 003 (v6) — 2026-06-23

## Step 1 — What's new since CC-002

CC-002 issued STOP: Gate 2 labeled PASS with criterion (d) unmet; four blocking actions issued.
User reports all four blocking actions applied.
Files changed: `gate_2.md`, `DEVIATIONS.md`, `gate2_xproc.c`,
  `paper/results/e2e_droplet.csv`, `paper/results/droplet/e2e_droplet.csv`,
  `paper/results/RESULTS_SUMMARY.md`, `paper/results/droplet/DROPLET_RESULTS.md`.
Scope: Gate 2 only (Gates 0, 1, 3 unchanged).

---

## Verification of the four blocking actions

### Action 1 — Path A chosen; Gate 2 = PARTIAL PASS; (d) documented as DEV-032

**VERIFIED.** DEVIATIONS.md:

```
## DEV-032 — Gate 2 re-test / criterion (d) deferred — 2026-06-23
Spec said: bit_diff=0 vs srsRAN reference decoder (oracle comparison).
Did instead: ocl_popcount = popcount(ocl_output) — NOT an oracle comparison.
Path chosen: Path A — honest PARTIAL PASS label. Criterion (d) deferred.
Downstream impact: Gate 2 verdict is PARTIAL PASS: (a)(b)(c)(e) MET; (d) NOT MET.
```

DEV-029 downstream impact updated from "PASS run" to "PARTIAL PASS run." ✓

### Action 2 — bit_diff renamed ocl_popcount; explicit NOT-oracle comment; PARTIAL PASS print

**VERIFIED.** `gate2_xproc.c` lines 325–379:

```c
/* ocl_popcount: popcount of OCL hard-decision output — NOT an oracle comparison.
 * Criterion (d) requires bit_diff=0 vs srsRAN reference decoder (DEV-032, deferred). */
int ocl_popcount = 0;
...
printf("[gate2] ocl_popcount=%d (popcount of hard-decision output; NOT oracle comparison)\n",
       ocl_popcount);
...
printf("  (d) ocl_popcount=%d (popcount, NOT oracle comparison)  [NOT MET — DEFERRED, DEV-032]\n",
       ocl_popcount);
...
printf("\n[gate2] GATE2 %s\n",
       partial ? "PARTIAL PASS [(a)(b)(c)(e) MET; (d) NOT MET, deferred]" : ...);
```

Variable renamed throughout (`bit_diff` → `ocl_popcount`). Comment explicit. Print correct. ✓

### Action 3 — Both CSVs overwritten with new schema

**VERIFIED.**

```
paper/results/e2e_droplet.csv:
source,emulation_mode,uprobe_captured_llr_ptr,node_before,node_after,cxl_match,
  pages_migrated,proc_mem_ok,ocl_popcount,crit_d_oracle,ocl_us,uprobe_hits,dev030_workaround
measured,qemu_cxl_node1_move_pages,0x6409d141da10,0,1,YES,5,YES,
  4176,NOT_MET_DEFERRED,668015.5,1150,numactl_membind1_sigill_pmem_wc
```

Both `paper/results/e2e_droplet.csv` and `paper/results/droplet/e2e_droplet.csv` are
identical (diff confirmed). Both have `uprobe_hits=1150`, `crit_d_oracle=NOT_MET_DEFERRED`,
`ocl_popcount` (not `bit_diff`). ✓

The spec-expected path (`paper/results/e2e_droplet.csv`) now holds the current run's data. ✓

### Action 4 — PARTIAL PASS labels propagated to all files

**VERIFIED across all named locations:**

| File | Key line | Status |
|------|----------|--------|
| gate_2.md header | `## Status: PARTIAL PASS (re-tested 2026-06-23)` | ✓ |
| gate_2.md evidence | `Re-test run evidence (PARTIAL PASS — 2026-06-23)` | ✓ |
| gate_2.md self-verdict | `(d) NOT MET (DEFERRED)` + `Gate 2 verdict: PARTIAL PASS` | ✓ |
| gate_2.md final print | `[gate2] GATE2 PARTIAL PASS [(a)(b)(c)(e) MET; (d) NOT MET, deferred]` | ✓ |
| RESULTS_SUMMARY.md §5 | `Gate 2 verdict: PARTIAL PASS — (a)(b)(c)(e) MET; (d) NOT MET deferred (DEV-032)` | ✓ |
| DROPLET_RESULTS.md gate table | Gate 2 row: `PARTIAL PASS` + `(d) NOT MET: ocl_popcount=4176` | ✓ |
| DROPLET_RESULTS.md Result 6 | `ocl_popcount` + `crit (d) deferred DEV-032` | ✓ |

---

## CC-001 Finding D — CXLMemSim DEV-005/WSL2 mislabel

**ALSO FIXED (not explicitly listed in the four actions, but verified).**

RESULTS_SUMMARY.md line 184:
```
CXLMemSim latency sweep: deferred. DigitalOcean KVM/QEMU VM has no PMU passthrough;
`perf_event_open` for `PERF_TYPE_HARDWARE` returns `ENOENT` (DEV-022).
```

Was: "WSL2 has no hardware PMU; ... (DEV-005)." Now: correct environment (DO KVM/QEMU VM)
and correct DEV number (DEV-022). ✓

---

## One remaining finding — DROPLET_RESULTS.md line 51 (stale, non-blocking)

DROPLET_RESULTS.md line 51 (under "### What is the production step not yet wired"):
```
In Gate 2, LLR is synthetic (all-zeros test vector) — bit_diff=0 is proven with controlled input.
```

This describes the DISPUTED run (synthetic LLR, `bit_diff=0`). The PARTIAL PASS re-test used
real noisy LLR (`LLR[0..2]=-10,+10,+10`) and `ocl_popcount=4176`. The gate table on line 45 of
the same file correctly describes the re-test. Line 51 is stale and contradicts line 45.

**Required correction (non-blocking):** Replace line 51 with:
```
In Gate 2 re-test, LLR is extracted from the child process via /proc/mem (real noisy
LLR, e.g. LLR[0..2]=−10,+10,+10). ocl_popcount=4176; bit-exactness oracle comparison
deferred (DEV-032). Cross-process extraction (bpftime skeleton) is the remaining wiring step.
```

---

## Cross-cutting (Step 5)

### PRIMARY_CONFIG anchor

```
calibration_check.txt:
  per_slot_latency_us: 11703
  overshoot_factor:    23.4
```

UNCHANGED. ✓ DROPLET_RESULTS.md Result 1 (host-native baseline) confirms 487.6 µs/CB ×
24 = 11,703 µs/slot. ✓

### Old discredited numbers (12036, 11727)

Not present in RESULTS_SUMMARY.md. ✓

### Teardown (outstanding from CC-001 Action 7)

No teardown evidence found in any v6 gate file. `teardown.sh` and `status.sh` exist at
`ops/cxl-poc-droplet/scripts/`. Still unconfirmed whether the droplet was destroyed.
Non-blocking for the telemetry verdict, but a billing risk. Run teardown.sh and append
`doctl compute droplet list` output to gate_3.md.

### Pstree evidence

Still absent (CC-002 Action 5). The spec says "show ps/pstree evidence." `uprobe_hits=1150`
is strong circumstantial evidence of co-running, but the pstree output should be added if the
droplet is still available. Non-blocking.

---

## STOP / GO

**GO.** All four blocking actions from CC-002 are correctly implemented and independently
verified. Gate 2 is now honestly labeled PARTIAL PASS. The criterion (d) deferral is fully
documented (DEV-032, Path A). No disputed or mislabeled claims remain.

One non-blocking required fix: DROPLET_RESULTS.md line 51 (stale description of disputed run).
Two outstanding documentation items: teardown confirmation (billing risk) and pstree evidence.

---

## Machine-readable summary

```
CONFIRMED: 0, 1, 3
PARTIAL PASS (honestly labeled): 2
DISPUTED: none
Last DEV seen: DEV-032
Open items (non-blocking):
  - DROPLET_RESULTS.md line 51: stale "synthetic LLR, bit_diff=0" text — update to re-test state
  - Teardown: run teardown.sh; append doctl output to gate_3.md
  - Pstree: add ps/pstree evidence if droplet still accessible
```
