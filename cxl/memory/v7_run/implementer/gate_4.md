## Spec

Gate 4 — Assembled run: Gates 0–3 simultaneously, e2e_gcp.csv with measured ocl_us

Criteria:
- (a) Benchmark and llr_gate3 run simultaneously (pstree / ps evidence while both live)
- (b) CXL memfd live during run (gate3 consumer holds allocation on node 1)
- (c) OpenCL reads from CXL buffer in same session as uprobe fires
- (d) bit_diff: honest path — if PoCL fast enough on KVM, run oracle comparison; else
      document as crit_d=DEFERRED PARTIAL PASS (same as v6 gate2_xproc criterion d)
- (e) e2e_gcp.csv written to `paper/results/e2e_gcp.csv` with measured `ocl_us` column
      (NOT the sum of prior separate measurements — must be from this assembled run)

Anchor (immutable): `primary_config_us_slot=11703, primary_config_slowdown=23.4`
The ocl_us in e2e_gcp.csv is the CXL offload path latency — separate from the anchor.

DEV-011 ghost: state which Z value the assembled run used. If Z=384: DEV-011 not affected.

## Commands

```bash
# Inside VM, assembled run:
bash /root/cxl/cxl_ran_poc/phase5_cxl/run_gate3_v7.sh --gate all

# After run completes:
cat /root/cxl/paper/results/e2e_gcp.csv

# Assembled evidence:
BENCH_PID=$(pgrep -x ldpc_decoder_benchmark | head -1)
ps aux | grep -E "ldpc_decoder|llr_gate3" | grep -v grep
pstree -p $BENCH_PID
```

## Raw evidence

<!-- PASTE VERBATIM TERMINAL OUTPUT BELOW — must include simultaneous process snapshot -->

```

```

<!-- END RAW EVIDENCE -->

## Self-verdict

| Criterion | Status | Evidence |
|-----------|--------|----------|
| (a) simultaneous pstree/ps evidence | PENDING | |
| (b) CXL memfd live | PENDING | |
| (c) OCL from CXL in same session | PENDING | |
| (d) bit_diff oracle or DEFERRED | PENDING | |
| (e) e2e_gcp.csv with measured ocl_us | PENDING | |
| DEV-011 ghost: Z value used | PENDING | state Z=? |

**Overall: PENDING**
