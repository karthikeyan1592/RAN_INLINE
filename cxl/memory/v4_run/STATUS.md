# v4 run status — last updated 2026-06-16 12:00

Current phase: COMPLETE
Current gate: Gate 6 PASS

Gates completed:
  0.1 PASS  — bpftime 248.5 ns/call, 40x under 9941 ns kernel-uprobe
  0.2 PASS  — rfsimulator TCP ESTAB gnb-ns↔ue-ns; nrLDPC_coding_decoder @ 0x000e9b30
  0.3 FAIL  — CXLMemSim WSL2 no PMU; Phase 4 latency sweep deferred per spec FAIL path
  0.4 INFO  — single-socket (WSL2), informational only
  1   PASS  — OpenCL LDPC decoder bit-exact (bit_diff_rate=0, BG1/BG2 LS=384,256)
  2   PASS  — bpftime uprobe intercepts LDPCdecoder; 5055 slot calls, 10090 CB calls,
              200 OpenCL decodes; ratio 2.0 CB/slot consistent (<0.25% error)
  3   PASS  — Sustained: 929,474 CB calls / 464,737 slots, ratio=2.000 exact;
              PERCPU counters close DEV-003; XDP 60,000 pkts/1535 ms continuous
  4   DEFERRED (Gate 0.3 FAIL path — WSL2 no PMU; CXLMemSim latency sweep not run)
  5   PASS  — Ablation: interception_only 2636 µs/slot, gpu_compute_full 163528 µs/slot
              (both measured as overhead_ns, N=1000 CB samples each); ocl_ns only =
              149548 µs/slot (sub-component, used for GPU projection).
              latency_ladder_v2.csv, comparison_table.csv, RESULTS_SUMMARY.md §5 written.
              DEV-014 logged. CC-004 corrections applied (overhead_ns not ocl_ns;
              projection 55.1× = 2636 + 149548/6, not 54.2×).
              Headline pair: 23.4× (fixed anchor) + 327× (measured end-to-end, CPU OCL/WSL2)
  6   CONFIRMED PASS (after CC-006) — Paper artifacts: 4 figures in paper/figures/.
              RESULTS_SUMMARY.md v2 (9 sections). CC-004 applied throughout.
              ops/cxl-poc-droplet committed (ff9ca6a): 16 files.
              CC-006 fix: emulation_mode.txt rewritten (v3→v4: OAI target, bpftime
              offsets e9b30/63110, netns/veth, PoCL CPU, CXL deferred).

Open deviations:
  DEV-001 (no larger droplet for 0.4)
  DEV-002 (ubpf JIT for bpftime)
  DEV-003 CLOSED — replaced by PERCPU_ARRAY counters (DEV-013)
  DEV-004 (CXLMemSim CLI mismatch)
  DEV-005 (CXLMemSim WSL2 no PMU)
  DEV-006 (gNB --rfsimulator.serveraddr flag unsupported, omitted)
  DEV-007 (UE PHY sync failure, not blocking)
  DEV-008 (runtime-oracle test vectors)
  DEV-009 (C≈2 CB/slot vs C=24; phy-test MCS — documented in all ablation source labels)
  DEV-010 (LLR payload in BPF map, not pointer)
  DEV-011 RESOLVED — LS_TO_IDX[Z] lookup applied in ldpc_measure.c; Z=224→iLS-3 correct
  DEV-012 (XDP slot period in aggregate rate, not individual IAs)
  DEV-013 (DEV-003 closed via PERCPU_ARRAY)
  DEV-014 (Phase 5 CPU OpenCL / 2ms poll / no CXL in path)

Phase 4: DEFERRED (Gate 0.3 FAIL — WSL2 no PMU; per spec FAIL path skip to Phase 5)
Next action: NONE — v4 run complete.
