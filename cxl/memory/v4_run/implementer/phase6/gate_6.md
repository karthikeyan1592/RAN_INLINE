# Gate 6 — Paper artifacts

## Spec

Phase 6: produce publication-quality figures and a complete RESULTS_SUMMARY.md.
Required deliverables:
1. `paper/figures/latency_cdf_v2.pdf` — CDF of ablation rows with 500 µs budget line
2. `paper/figures/latency_breakdown_v2.pdf` — stacked bars: baseline / +interception / +OCL / projected
3. `paper/figures/nic_packet_timeline.pdf` — NIC inter-arrival histogram from Gate 3
4. `paper/figures/bit_correctness_table.pdf` — Phase 1 per-(BG,LS) table
5. `paper/results/RESULTS_SUMMARY.md` — complete v2 rewrite with all 9 sections
6. `paper/results/latency_ladder_v2.csv` — corrected after CC-004
7. `paper/results/comparison_table.csv` — corrected after CC-004
8. `paper/results/ablation_raw.csv` — 2000-row raw measurement (Gate 5)
9. `ops/cxl-poc-droplet/` — ops runbook with integration notes; committed

## Commands

```bash
# Figures
python3 measurement/plot_v4_figures.py

# Verify outputs
ls paper/figures/*.pdf

# ops commit
cd ops/cxl-poc-droplet
git add -A && git commit -m "v4: bpftime/CXLMemSim/OAI integration notes..."
```

## Raw evidence

### Figures produced

```
paper/figures/latency_cdf_v2.pdf       — Phase 5 ablation CDF (overhead_ns × C_actual=2)
paper/figures/latency_cdf_v2.png
paper/figures/latency_breakdown_v2.pdf — Stacked bars: interception + OCL; GPU projection bar
paper/figures/latency_breakdown_v2.png
paper/figures/nic_packet_timeline.pdf  — XDP inter-arrival histogram + cumulative timeline
paper/figures/nic_packet_timeline.png
paper/figures/bit_correctness_table.pdf — Phase 1 PASS table (BG1/BG2, LS=384/256)
paper/figures/bit_correctness_table.png
```

All figures: IEEE two-column style (3.5×2.6 inches, serif, 8pt, 300 dpi).

### RESULTS_SUMMARY.md v2 sections

```
§1  PRIMARY_CONFIG baseline motivation — 23.4× over budget (11,703 µs / 500 µs)
§2  Bit-correctness — Phase 1 PASS (0 mismatches, BG1/BG2 LS=384/256)
§3  Interception — Phase 2/3 (bpftime uprobe, no source changes, cb/slot=2.000)
§4  NIC-level evidence — Phase 3 (60,000 pkts, 1535 ms, aggregate 2204/s)
§5  CXL path — Phase 4 deferred (WSL2 no PMU)
§6  End-to-end results — Phase 5 headline pair (23.4× + 327×); GPU projection 55.1×
§7  White-paper framing — vendor-neutral lookaside, O-RAN WG6 AAL, transparent interception
§8  Limitations — CXL deferred, PoCL/CPU, poll interval, C_actual=2
§9  Emulation mode vocabulary
```

### latency_ladder_v2.csv (CC-004 corrected)

Row 3 (`+gpu_compute_full`): uses `overhead_ns` (163,527.8 µs) not `ocl_ns` (149,548.2 µs).
GPU projection formula: 2,636 + 149,548/6 = 27,561 µs = 55.1×.
Source column: `fixed_anchor` / `measured` / `measured` / `deferred`.

### comparison_table.csv (CC-004 corrected)

Separate rows for end-to-end overhead_ns (163,527.8 µs, 327.1×) and OCL-only (149,548.2 µs, 299.1×).
Projection row: 27,560.7 µs, 55.1×, labelled `projected`.

### ops/cxl-poc-droplet commit

```
commit ff9ca6a (master)
Author: ...
Date:   2026-06-16

    v4: bpftime/CXLMemSim/OAI integration notes from no-compromise pipeline session

16 files committed:
  references/bpftime_build_gate0.1.md
  references/cxlmemsim_build_gate0.3.md
  references/oai_build_notes.md
  references/cxl_qemu_kvm_gotchas.md
  scripts/setup_netns.sh
  scripts/provision.sh  install_deps.sh  qemu_cxl_launch.sh  verify_cxl_checks.sh
  scripts/checkpoint.sh  restore.sh  teardown.sh  delete_snapshot.sh  status.sh
  ebpf/xdp_rfsim_observe.bpf.c
  SKILL.md
```

## CC-006 blocker and fix

**Blocker identified by reviewer:** `paper/results/emulation_mode.txt` contained v3 content
(srsRAN `ldpc_decoder_benchmark` target, `libl1_offload.so` offload path, `wsl2-mmap-shm-fallback`
CXL mode). RESULTS_SUMMARY.md §5 references this file directly — a reader following that reference
would see a completely different architecture.

**Fix applied:** `emulation_mode.txt` fully rewritten with v4 content:
- `emulation_mode_primary: oai-rfsim-netns-veth-bpftime`
- OAI nr-softmodem target (not srsRAN benchmark)
- bpftime uprobe offsets: `nrLDPC_coding_decoder @ 0xe9b30`, `LDPCdecoder @ 0x63110`
- netns/veth topology: gnb-ns 10.77.0.2 ↔ ue-ns 10.77.0.1
- PoCL/CPU OpenCL — explicitly not a GPU (DEV-014)
- CXL path DEFERRED; `software-synthetic-wsl2` marked SUPERSEDED
- emulation_mode vocabulary matching `ablation_raw.csv` column values

## Verdict

**CONFIRMED PASS** (after CC-006 fix)

All required Phase 6 deliverables produced:
- 4 paper figures (PDF + PNG) in `paper/figures/`
- RESULTS_SUMMARY.md v2 with all 9 sections
- latency_ladder_v2.csv and comparison_table.csv corrected (CC-004)
- ablation_raw.csv: 2000 rows (1000 pass-0 + 1000 pass-1), both passes N=500 slots
- ops/cxl-poc-droplet committed (ff9ca6a): 16 files, all integration notes
- emulation_mode.txt rewritten (CC-006 fix): v4 architecture, OAI target, bpftime offsets

CC-004 corrections applied throughout: `overhead_ns` (not `ocl_ns`) is the true
end-to-end pipeline metric. GPU projection: 55.1× (not 54.2×).

No new deviations from spec.

## Timestamp

2026-06-16 (v4 session)
emulation_mode: oai-rfsim-netns-veth-bpftime
