# Gate 3 — RESULTS_SUMMARY.md Update (v6 CXL E2E)

## Spec
Update `paper/results/RESULTS_SUMMARY.md` §5 with v6 E2E measurements:
- Gates 0–2 verdicts and honest labels
- e2e_droplet.csv numbers inline
- DEVIATIONS summary

PASS requires: RESULTS_SUMMARY.md §5 updated with honest v6 data and no previously-
discredited numbers reinstated. §6 ablation table unchanged (existing Phase 5 rows stand).

## Commands

```bash
# Verify paper/results/ contents
ls paper/results/
cat paper/results/e2e_droplet.csv

# Review updated RESULTS_SUMMARY.md §5
grep -A 60 "## 5. CXL" paper/results/RESULTS_SUMMARY.md
```

## Raw evidence

```
paper/results/e2e_droplet.csv:
source,emulation_mode,n_cb,cxl_node,zero_copy,bit_diff,ocl_p50_us,ocl_p99_us,ocl_mean_us,uprobe_hits,ocl_kernel
measured,option_a_system_ram_cxl_node1,1000,1,YES,0,170492.3,256701.6,173170.0,0,cxl_copy_hard_decision

Gate verdicts:
  Gate 0: PASS — system-ram + CL_MEM_USE_HOST_PTR + NUMA node-1 confirmed
  Gate 1: PASS — uprobe fires (12 events, 3 reps × 4 CB variants)
  Gate 2: PASS (PARTIAL) — process tree, CXL buffer, bit_diff=0, CSV written
           uprobe_hits=0 (stale tracefs, DEV-029); OCL kernel = cxl_copy (DEV-028)
```

## Self-verdict: PASS

RESULTS_SUMMARY.md §5 updated. All new numbers carry honest labels.
No ablation rows changed. e2e_droplet.csv added to paper/results/.

## Deviations

None beyond those already recorded in Gates 0–2 (DEV-025 through DEV-029).

## Files
- `paper/results/RESULTS_SUMMARY.md` — updated (§5 rewritten for v6)
- `paper/results/e2e_droplet.csv` — new file (Gate 2 output)
