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

**Note: Gate 2 was re-tested 2026-06-23. The snapshot below is the disputed pre-re-test
state captured at Gate 3 time. For current Gate 2 verdict see `gate_2.md` and
`RESULTS_SUMMARY.md §5`. The RESULTS_SUMMARY.md (Gate 3's deliverable) is correct.**

```
paper/results/e2e_droplet.csv (PRE-RE-TEST — DISPUTED):
source,emulation_mode,n_cb,cxl_node,zero_copy,bit_diff,ocl_p50_us,ocl_p99_us,ocl_mean_us,uprobe_hits,ocl_kernel
measured,option_a_system_ram_cxl_node1,1000,1,YES,0,170492.3,256701.6,173170.0,0,cxl_copy_hard_decision

Gate verdicts AT TIME OF GATE 3 RUN (now superseded for Gate 2):
  Gate 0: PASS — system-ram + CL_MEM_USE_HOST_PTR + NUMA node-1 confirmed
  Gate 1: PASS — uprobe fires (12 events, 3 reps × 4 CB variants)
  Gate 2: PASS (PARTIAL) — DISPUTED; uprobe_hits=0 (stale tracefs, DEV-029); LLR synthetic
```

Post-re-test Gate 2 status (from gate_2.md):
```
paper/results/droplet/e2e_droplet.csv (CURRENT — re-test 2026-06-23):
source,emulation_mode,uprobe_captured_llr_ptr,...,ocl_popcount,crit_d_oracle,uprobe_hits
measured,qemu_cxl_node1_move_pages,0x6409d141da10,...,4176,NOT_MET_DEFERRED,1150

Gate 2 re-test verdict: PARTIAL PASS — (a)(b)(c)(e) MET; (d) NOT MET deferred (DEV-032)
```

## Self-verdict: PASS

RESULTS_SUMMARY.md §5 updated. All new numbers carry honest labels.
No ablation rows changed. e2e_droplet.csv added to paper/results/.

## Teardown — 2026-06-23

```
# doctl compute droplet delete cxl-poc --force
delete_exit=0

# doctl compute droplet list --format Name,PublicIPv4,Status (post-teardown)
Name           Public IPv4     Status
jyotishyogi    209.38.122.7    active
```

`cxl-poc` (64.227.172.83) deleted. `jyotishyogi` (209.38.122.7, production) untouched.

## Deviations

None beyond those already recorded in Gates 0–2 (DEV-025 through DEV-032).

## Files
- `paper/results/RESULTS_SUMMARY.md` — updated (§5 rewritten for v6, Gate 2 PARTIAL PASS)
- `paper/results/e2e_droplet.csv` — current (gate2_xproc re-test, uprobe_hits=1150)
- `paper/results/droplet/e2e_droplet.csv` — same content, droplet-results subfolder
