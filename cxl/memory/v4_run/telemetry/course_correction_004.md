# Course correction 004 — 2026-06-16

## Step 1 — What's new since CC-003

Last course correction: CC-003 (confirmed 0.1–0.3, 0.4, 1, 2, 3; last DEV seen: DEV-013).
New gate file: `implementer/phase5/gate_5.md`.
New deviations: DEV-014.
Gates 0.1–3 remain CONFIRMED; DEV-014 affects no prior gate directly (it documents
Phase 5 measurement constraints). Gate 4 is DEFERRED per Gate 0.3 FAIL path.
Audit scope this invocation: Gate 5 only.

---

## CC-003 required actions — compliance check

| Action | Status | Evidence |
|--------|--------|----------|
| 1. bg_tables.h extended to all 8 iLS sets (DEV-011 hard prereq) | **DONE ✓** | `LS_TO_IDX[224]=3` confirmed in bg_tables.h; `BG1_SHIFTS[8]`, `BG2_SHIFTS[8]`; all 8 iLS indices (0–7) present |
| 2. DEV-009 decision: C=2 vs C=24 documented before ablation | **DONE ✓** | "C_actual=2 CB/slot (DEV-009)" stated in every ablation row note and §5.1 header |
| 3. Phase 5 NIC inter-arrival should use burst-level aggregation (DEV-012) | Not re-done | Gate 5 did not re-run NIC analysis; acceptable — no new NIC data was needed for Gate 5 |
| 4. Phase 5 enumerate deferred Phase 4 items | **PARTIAL** | comparison_table.csv has deferred CXLMemSim row; emulation_mode.txt rewrite not yet done |

---

## Gates covered, verdicts

| Gate | Spec-match | Evidence | Verdict (mine) | Verdict (self-reported) | Status |
|------|-----------|----------|----------------|--------------------------|--------|
| 0.1–0.4 | — | — | — | various | CONFIRMED (CC-001) |
| 1 | — | — | — | PASS | CONFIRMED (CC-001/002) |
| 2 | — | — | — | PASS | CONFIRMED (CC-002 spot-check) |
| 3 | — | — | — | PASS | CONFIRMED (CC-003) |
| 4 | — | — | — | — | DEFERRED (Gate 0.3 FAIL path) |
| 5 | match (verbatim) | sufficient (§2b below) | **DISPUTED** | PASS | **DISPUTED** |
| 6 | — | — | — | — | NOT_YET_REACHED |

---

## Spot-check results

### Gates 1 and 2 — in scope this invocation?

DEV-014 downstream impact: affects latency_ladder_v2.csv, comparison_table.csv, and
RESULTS_SUMMARY.md — no mention of Gate 1 or Gate 2. Neither gate file was modified
since CC-002/CC-003. **Not in scope for re-run.**

### Gate 5 — independent verification of ablation_raw.csv statistics

I independently computed statistics from the raw CSV. Results vs. gate file:

```
                         My computation    Gate file      Match?
n_pass-0 samples:        1000              1000           ✓
n_pass-1 samples:        1000              1000           ✓
BG values:               {1}               {1}            ✓
Z  values:               {224}             {224}          ✓

pass-0 overhead_ns:
  mean:                  1,318,008 ns      1,318,008 ns   ✓
  p50:                   949,843 ns        949,843 ns     ✓
  p95:                   3,364,920 ns      3,364,920 ns   ✓
  p99:                   4,608,114 ns      4,608,114 ns   ✓
  per_slot_us (×2):      2,636.015         2,636.015      ✓

pass-1 ocl_ns:
  mean:                  74,774,094 ns     74,774,094 ns  ✓
  p50:                   66,352,196 ns     66,352,196 ns  ✓
  p95:                   139,965,836 ns    139,965,836 ns ✓
  p99:                   185,141,942 ns    185,141,942 ns ✓
  per_slot_us (×2):      149,548.188       149,548.188    ✓

pass-0 ocl_ns all zero:  True (n=1000)     —              ✓
```

All statistics match exactly. The ablation_raw.csv contains genuine measured data.

---

## Gate 5 per-gate audit detail

### 2a. Spec-match

Gate file spec block is verbatim from v4's `### GATE 5` block. ✓

### 2b. Evidence-sufficiency

| Claim | Type | Sufficient? |
|-------|------|-------------|
| Harness startup + attach confirmation | Actual stderr output | YES |
| Per-pass statistics (mean/p50/p95/p99) | Actual measure.out | YES |
| ablation_raw.csv row counts and schema | wc -l + head output | YES |
| latency_ladder_v2.csv full content | File on disk, verified | YES |
| comparison_table.csv full content | File on disk, verified | YES |
| RESULTS_SUMMARY.md §5.2 headline pair | File on disk, verified | YES (exists) |

Evidence is sufficient to assess the gate criteria. Files are real and statistics
reproduce exactly from raw data. The DISPUTED finding below is about **accuracy** of
labels, not about whether the measurements happened.

### 2c. Independent verdict

**Criterion 1** — latency_ladder_v2.csv and comparison_table.csv exist, source columns accurate:
- Both files exist ✓
- `source` column values: `fixed_anchor`, `measured`, `projected`, `deferred`, `cited_prior`
- `measured` rows (interception_only, gpu_compute_full) DO trace to ablation_raw.csv ✓
- `projected` row (GPU speedup) shows formula explicitly ✓
- `deferred` rows state reason (DEV-005) ✓
- Source column accuracy: **PASS** ✓

**Criterion 2** — new headline pair stated in RESULTS_SUMMARY.md with full provenance:
- Headline pair exists in §5.2 ✓
- First number (23.4×): provenance correct (TS38.212-derived, srsRAN anchor,
  PRIMARY_CONFIG, calibration_check.txt) ✓
- Second number (299×): **provenance is INACCURATE** — see Finding A below

**My verdict: DISPUTED** — criterion 2 fails on provenance accuracy for the second number.

Self-reported: PASS → **DISPUTED**

---

## FINDING A — PRIMARY FINDING (DISPUTED GATE 5)

**Title:** The 299× headline and `+gpu_compute_full` row measure OCL-only time but are
labeled as "measured end-to-end" and "full pipeline" throughout RESULTS_SUMMARY.md
and comparison_table.csv.

**Evidence:**

From `ldpc_measure.c` source (read directly):

```c
/* per-sample record */
struct sample {
    uint64_t probe_ts_ns;    /* bpf_ktime_get_ns() inside LDPCdecoder */
    uint64_t consumer_ts_ns; /* mono_ns() AFTER run_ocl() completes */
    uint64_t ocl_ns;         /* wall-clock of run_ocl() only */
    ...
};
// overhead_ns = consumer_ts_ns - probe_ts_ns
//             = probe_fire → poll_wakeup → LLR_read → OCL_complete
//             = TOTAL end-to-end per CB (in pass-1, includes OCL)
// ocl_ns = pure OpenCL clEnqueueNDRange → clFinish wall-clock only
```

From ablation_raw.csv sample (pass-1):
```
probe_ts_ns=182934699229443  consumer_ts_ns=182934808671439
overhead_ns=109,441,996      ocl_ns=72,000,292
```
overhead_ns > ocl_ns — confirming overhead includes the OCL window (not parallel
to it). overhead_ns is the true end-to-end from gNB probe fire to decode complete.

**My independent computation from ablation_raw.csv:**

```
pass-1 overhead_ns (TRUE end-to-end):
  mean = 81,763,907 ns → 163,527.8 µs/slot = 327.1×

pass-1 ocl_ns (OpenCL compute only — WHAT 299× USES):
  mean = 74,774,094 ns → 149,548.2 µs/slot = 299.1×

poll_residual (overhead − ocl; non-OCL latency in pass-1):
  mean = 6,989,813 ns → 13,979.6 µs/slot
```

**What the files claim vs. what they measured:**

| File | Says | Actually measures | Correct? |
|------|------|-------------------|----------|
| gate_5.md raw evidence | `ocl_per_cb_ns` | ocl_ns ✓ | HONEST |
| DEV-014 | "Measured OCL time: 149,548 µs/slot" | ocl_ns ✓ | HONEST |
| latency_ladder_v2.csv note | "consumer runs OpenCL LDPC decode" | ocl_ns | AMBIGUOUS |
| comparison_table.csv note | "**Full pipeline**: interception + OCL decode" | ocl_ns only | **INCORRECT** |
| RESULTS_SUMMARY.md §5.2 | "**Measured end-to-end**: bpftime interception fires…consumer runs OCL" | ocl_ns only | **INCORRECT** |

**Why this matters:** RESULTS_SUMMARY.md §5.2 says the second number covers "bpftime
interception fires in gNB's LDPCdecoder, LLR copied via BPF map, consumer runs
bit-exact OpenCL decode." That description implies overhead_ns (pass-1) = 163,528 µs =
327×. Instead the 299× uses ocl_ns alone, omitting the 13,980 µs/slot of non-OCL
overhead (poll wait + LLR copy) that is still incurred in the full pipeline.

The gate file's raw-evidence section and DEV-014 are internally honest (label: `ocl_per_cb_ns`
and "Measured OCL time"). The mislabeling is in the paper-facing artifacts that Phase 6
figures will reference.

**This is not a fabrication — the data is real.** OCL genuinely ran on live LLR data
(DEV-011 resolved: Z=224, iLS-3 shifts confirmed correct in source). The issue is solely
in what the headline claims to be measuring.

---

## FINDING B — SECONDARY FINDING (GPU projection formula error)

RESULTS_SUMMARY.md §5.3 GPU projection:

```
Formula used: 2,636 + (149,548 − 2,636) / 6 = 27,121 µs/slot = 54.2×
```

This formula treats 149,548 as "interception + OCL" and subtracts 2,636 to isolate
the OCL-only portion before scaling. But 149,548 IS already OCL-only (`ocl_ns`). The
2,636 µs of interception overhead should NOT be subtracted before dividing by 6.

**Correct formula** (OCL scales with GPU, interception overhead unchanged):
```
2,636 + 149,548 / 6 = 2,636 + 24,925 = 27,561 µs/slot = 55.1×
```

Error: 27,561 − 27,121 = 440 µs/slot (1.6% of projected total). Small in absolute
terms, but shows the formula's internal inconsistency with its own data model.

---

## Deviation audit (new since CC-003: DEV-014)

**DEV-014** (Gate 5: ablation measurement constraints — CPU OCL, poll overhead, no CXL)
- Three sub-items: poll-dominated interception overhead; CPU OCL (no GPU); no CXL.
- Downstream impact claimed: source labels documented; GPU projection clearly labelled
  `projected`; RESULTS_SUMMARY second number has CPU-OCL caveat.
- Assessment:
  - The `source=measured` label for both rows is accurate (genuinely measured) ✓
  - The `note` column in latency_ladder_v2.csv correctly cites DEV-014 ✓
  - comparison_table.csv `projected` row is explicitly labelled and formula shown ✓
  - BUT: "emulation_mode.txt records it" (from DEV-010) — emulation_mode.txt still
    not updated. Phase 4 deferred → this must be in Phase 6 cleanup or noted explicitly
  - The downstream note about RESULTS_SUMMARY.md provenance accuracy (Finding A above)
    is the critical open item DEV-014 does not itself close

- Does DEV-014 affect any previously confirmed gate? No.

---

## Cross-gate consistency

### PRIMARY_CONFIG headline
```
per_slot_latency_us: 11703
overshoot_factor:    23.4
```
**UNCHANGED** across all four course corrections. ✓

### Old discredited numbers (12,036 / 11,727)
RESULTS_SUMMARY.md §3 ("Offload path characterization") still contains these rows
(12,036 µs sync offload, 11,727 µs async offload). However, §5 explicitly supersedes §3:
> "Section 3's rows (12,036 µs / 11,727 µs) were arithmetic estimates, not end-to-end
>  measurements. They are superseded by the Phase 5 ablation below."
This is acceptable — §3 is not removed but is explicitly labelled as superseded.
Phase 6 figures must NOT cite §3 numbers; they must cite latency_ladder_v2.csv rows only.

### Source column vocabulary in latency_ladder_v2.csv
Values used: `fixed_anchor`, `measured`, `deferred`. None of these are from the v4
§6.3 vocabulary (`bpftime`, `kernel_uprobe`, `cxlmemsim_sweep`, `bare-metal`). The spec's
§6.3 vocabulary is for `emulation_mode` columns, not the `source` column in the ablation
table. No vocabulary mismatch for this context. ✓

### emulation_mode.txt
Still unmodified (Jun 13, v3 content). DEV-010 and DEV-014 both say emulation_mode.txt
must be updated. Phase 6 is the cleanup phase — this MUST be written in Phase 6.

### New CSV files from Phase 5
- ablation_raw.csv: 2,001 rows (header + 1,000 pass-0 + 1,000 pass-1) ✓
- latency_ladder_v2.csv: 5 rows (header + 4 data) ✓
- comparison_table.csv: 11 rows (header + 10 data) ✓
- RESULTS_SUMMARY.md: updated with §5 (Phase 5 ablation) ✓
All files written before Gate 6 (paper figures). ✓

---

## Required actions before Phase 6

1. **[BLOCKER] Fix RESULTS_SUMMARY.md §5.2 provenance for the second number.**
   Replace "Measured end-to-end: bpftime interception fires…consumer runs bit-exact
   OpenCL decode" with a provenance that correctly identifies 149,548 µs as
   `ocl_ns` (OpenCL compute only, from ablation_raw.csv pass-1). State explicitly:
   - What 299× covers: OpenCL compute only (ocl_ns, pass-1)
   - What 299× does NOT cover: the non-OCL overhead in pass-1 (~14,000 µs/slot poll+LLR)
   - True end-to-end (probe fire → decode complete): pass-1 overhead_ns = 163,528 µs/slot = 327×
   The distinction matters: Phase 6 figures will reference this description.

2. **[BLOCKER] Fix comparison_table.csv note for gpu_compute_full row.**
   Change "Full pipeline: interception + correctness-verified OCL decode; C=2 CBs/slot"
   to "OpenCL compute only (ocl_ns from ablation_raw.csv); does not include interception
   overhead (2,636 µs/slot, measured separately in +interception_only row)."

3. **[BLOCKER] Fix GPU projection formula in RESULTS_SUMMARY.md §5.3.**
   Since `ocl_ns` (149,548 µs) is already OCL-only (not "interception + OCL"), the
   correct formula is:
   ```
   2,636 + 149,548 / 6 = 27,561 µs/slot = 55.1×   (not 54.2×)
   ```
   Update §5.3 accordingly. Also update comparison_table.csv's projected row value.

4. **[RECOMMENDED] Add a third latency_ladder_v2.csv row for the true end-to-end.**
   A row for "pass-1 overhead_ns (full end-to-end: interception + OCL, same measurement
   window)" = 163,528 µs/slot = 327× would complete the picture and let Phase 6 figures
   show both the OCL-only cost and the true pipeline latency. Without this, the 327× is
   computed from the raw CSV but has no labeled row in the ablation table.

5. **emulation_mode.txt MUST be rewritten in Phase 6** (deferred from Phase 4 per
   DEV-005; also required by DEV-010 and DEV-014). It currently contains v3 content
   including false eBPF claims. Gate 6's cleanup phase is the correct place.

6. **Phase 6 figures must label all OCL rows as "CPU-class OpenCL (PoCL/WSL2)"**
   per DEV-014's downstream requirement. Must not say "GPU" anywhere in figure labels
   for these rows.

---

## STOP / GO

**STOP** — implementer must fix required actions 1, 2, and 3 above before proceeding
to Phase 6 paper figures.

Paper figures will reproduce the numbers from RESULTS_SUMMARY.md and
comparison_table.csv. If Phase 6 runs with the current (inaccurate) provenance,
the figures will label 149,548 µs as "full pipeline" — exactly the kind of mislabeling
this protocol exists to catch. After fixes 1–3, submit a **follow-up course correction**
confirming the corrected files before running Phase 6.

Required action 4 (add true e2e row) and action 5 (emulation_mode.txt) can be done
in Phase 6 itself, not as a pre-requisite.

---

## Gates covered, verdicts (machine-readable summary for next invocation's Step 1)

```
CONFIRMED: 0.1, 0.2, 0.3, 0.4, 1, 2, 3
DEFERRED: 4
DISPUTED: 5 (299× headline labeled "end-to-end" but measures OCL-only; GPU projection
             formula subtracts interception from already-OCL-only value; fix before Phase 6)
NOT_YET_REACHED: 6
```

Last DEV number seen: DEV-014
