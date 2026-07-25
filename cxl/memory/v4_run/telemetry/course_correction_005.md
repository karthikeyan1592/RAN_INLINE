# Course correction 005 — 2026-06-16 (follow-up to CC-004)

## Purpose

Follow-up confirmation requested by CC-004: "After fixes 1–3, submit a follow-up
course correction confirming the corrected files before running Phase 6."

This CC audits only the three CC-004 blockers. No new gate files have been added.
All gates confirmed in CC-001 through CC-004 remain unchanged.

---

## CC-004 blocker resolution

### Blocker 1 — RESULTS_SUMMARY.md §5.2 provenance for second number

**Required:** Correct "Measured end-to-end: bpftime…consumer runs bit-exact OpenCL
decode" label and 299× value; explicitly identify source as overhead_ns, not ocl_ns.

**Observed in file:**
```
Second number: 327×   (163,528 µs / 500 µs)
               Measured end-to-end: overhead_ns = consumer_ts_ns − probe_ts_ns.
               This is the true pipeline time: bpftime interception fires in gNB's
               LDPCdecoder, LLR copied via BPF map, consumer runs bit-exact OpenCL
               decode (Phase 1 kernel), consumer records completion time.
               ...
               Source: ablation_raw.csv pass=1 overhead_ns column (NOT ocl_ns).
               Note: ocl_ns only (pure OpenCL wall-clock) = 149,548 µs/slot (299×);
               this is a sub-component, not the end-to-end pipeline time.
```

**Verification:** 163,528 µs = pass-1 overhead_ns mean (81,763,907 ns × 2 CBs / 1000).
Independently computed: 163,527.8 µs. File rounds to 163,528. ✓

**Status: RESOLVED ✓**

---

### Blocker 2 — comparison_table.csv pipeline row note and value

**Required:** Change "Full pipeline: interception + OCL decode" note and 149,548 µs
value to reflect overhead_ns (true end-to-end); keep OCL-only as a separate row.

**Observed in file (two relevant rows):**
```
Row 3: "Full pipeline end-to-end: overhead_ns = consumer_ts_ns − probe_ts_ns
         (interception + LLR copy + OCL)..." → value_us=163527.8, source=measured
         note includes: "327× over 500us budget; source: ablation_raw.csv pass=1 overhead_ns"

Row 4 (new): "ocl_ns pure OpenCL wall-clock only (clEnqueueNDRange → clFinish);
              NOT the end-to-end pipeline time" → value_us=149548.2, source=measured
              note: "299× over 500us; separated from overhead_ns for GPU projection use"
```

**Verification:**
- 163,527.8 µs ← ablation_raw.csv pass-1 overhead_ns × 2 / 1000 = 163,527.814... ✓
- 149,548.2 µs ← ablation_raw.csv pass-1 ocl_ns × 2 / 1000 = 149,548.188... ✓
- The OCL-only row is correctly labeled "NOT the end-to-end pipeline time" ✓

**Status: RESOLVED ✓**

---

### Blocker 3 — GPU projection formula

**Required:** Change `2,636 + (149,548−2,636)/6` to `2,636 + 149,548/6`.

**Observed in RESULTS_SUMMARY.md §5.3:**
```
Projected per-slot:  2,636 (interception from pass 0) + 149,548/6 (OCL speedup)
                   = 2,636 + 24,925
                   = 27,561 µs/slot = 55.1× over budget
```

**Observed in comparison_table.csv projected row:**
```
value_us=27560.7
note="formula: interception_overhead_us + ocl_us/6 = 2636 + 149548/6 = 27561 us/slot; 55.1×"
```

**Verification:**
- `2636.015 + 149548.188/6 = 2636.015 + 24924.698 = 27560.713` ✓
- File shows 27560.7 (truncated to 1 decimal) ✓
- Old wrong value 27,121 / 54.2×: **zero occurrences** in any of the three paper-facing
  artifacts (grep confirms) ✓

**Status: RESOLVED ✓**

---

## Residual 149,548 / 299× occurrences — all legitimate

Two occurrences of `149548` remain in paper-facing files:

| File | Context | Legitimate? |
|------|---------|-------------|
| comparison_table.csv row 4 | OCL-only sub-component row, labeled "NOT the end-to-end pipeline time", source=measured | YES — correctly represents ocl_ns |
| comparison_table.csv row 5 (formula note) | `2636 + 149548/6 = 27561` — correct GPU projection formula using ocl_ns as intended | YES — correct formula |

No occurrences of `27121`, `54.2×`, or the wrong `(149548-2636)/6` formula remain. ✓

gate_5.md raw-evidence section retains original `149548.2` values in its CSV preview
and `ocl_per_cb_ns` labels — intentionally preserved per CC-004 (historical snapshot
of what was observed at gate-run time). ✓

---

## Independent number verification (from ablation_raw.csv)

| Claim | My computation | File value | Match |
|-------|---------------|------------|-------|
| overhead_ns mean_us/slot | 163,527.8 | 163,527.8 | ✓ |
| overhead_ns p50_us/slot | 145,571.2 | 145,571.2 | ✓ |
| overhead_ns p95_us/slot | 302,519.0 | 302,519.0 | ✓ |
| overhead_ns p99_us/slot | 405,497.6 | 405,497.6 | ✓ |
| Second number (×budget) | 327.1× | 327× | ✓ |
| GPU projection (µs/slot) | 27,560.7 | 27,560.7 | ✓ |
| GPU projection (×budget) | 55.12× | 55.1× | ✓ |

All numbers verified to 1-decimal precision from ablation_raw.csv. ✓

---

## PRIMARY_CONFIG unchanged

```
per_slot_latency_us: 11703    overshoot_factor: 23.4
```
Unchanged across all five course corrections. ✓

---

## Gate 5 status change

Gate 5 moves from **DISPUTED → CONFIRMED**.

The original DISPUTED finding (CC-004): "the 299× headline and `+gpu_compute_full`
row measure OCL-only time but are labeled 'measured end-to-end' / 'full pipeline'."

All three paper-facing artifacts now correctly represent:
- 327× / 163,528 µs as the true end-to-end (overhead_ns, probe fire → decode complete)
- 299× / 149,548 µs as the OCL-only sub-component, clearly labeled as such
- GPU projection uses correct formula: 2,636 + 149,548/6 = 27,561 µs = 55.1×

---

## Remaining open items (not blockers for Phase 6)

These were CC-004 actions 4 and 5, deferred to Phase 6:

1. **latency_ladder_v2.csv** now includes the true end-to-end row with overhead_ns
   values (163,528 µs), which was CC-004 recommended action 4. ✓ (completed
   alongside the blocker fixes, not deferred)

2. **emulation_mode.txt** still contains v3 stale content (unmodified since Jun 13).
   Must be rewritten in Phase 6 (DEV-005, DEV-010, DEV-014 all require it).

3. **Phase 6 figures must label OCL rows as "CPU-class OpenCL (PoCL/WSL2)"**
   per DEV-014. No figure files exist yet; this remains a Phase 6 authoring constraint.

---

## STOP / GO

**GO** — Gate 5 is CONFIRMED. Phase 6 may proceed.

---

## Gates covered, verdicts (machine-readable summary for next invocation's Step 1)

```
CONFIRMED: 0.1, 0.2, 0.3, 0.4, 1, 2, 3, 5
DEFERRED: 4
DISPUTED: none
INSUFFICIENT_EVIDENCE: none
NOT_YET_REACHED: 6
```

Last DEV number seen: DEV-014
