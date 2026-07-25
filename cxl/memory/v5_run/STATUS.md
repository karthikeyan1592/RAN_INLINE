# v5 run status — last updated 2026-06-22

Current phase: 4 COMPLETE (partial pass) → awaiting Gate 4 telemetry GO for Phase 5
Current gate: Gate 4 SUBMITTED — awaiting telemetry verification

Architecture changes (v5 goal):
  A — numactl --membind CXL node: OAI LLR allocations land in /dev/dax0.0
  B — Descriptor-only uprobe: 52-byte descriptor (not payload copy) — DEV-017
  C — Pinned-core busy-poll consumer: sub-us detection vs v4's 2ms sleep

Gates completed:
  Gate 1 PASS (2026-06-20): SPSC ring + busy-poll consumer, p50=17 ns, 0 drops at K=1e6
  Gate 2 PASS (2026-06-21): CL_MEM_USE_HOST_PTR zero-copy CONFIRMED + bit_diff=0 all 4 cases
  Gate 3 PASS (2026-06-22): bpftime uprobe 200 desc in 24.9 s, 0 drops, MPSC confirmed (2+ TIDs)
  Gate 4 PARTIAL PASS (2026-06-22):
    interception_only: p50=1075 µs/CB (bpftime IPC floor, DEV-020); 2022 CBs; active_rate=613.7/s
    gpu_compute_full:  standalone OCL 141628 µs/CB; combined 142704 µs/CB (DEV-021)
    C_actual=2 confirmed; proj_c24 (interception) = 115,909 µs/slot; DEV-009 CLOSED
    latency_ladder_v2_v5.csv written (3 rows: baseline FIXED + 2 measured)

Open deviations (v5): DEV-015 through DEV-021 (see DEVIATIONS.md)

DEV-003 ghost: CLOSED (Gate 3). OAI calls LDPCdecoder from a thread pool (2+ concurrent
threads confirmed). BPF RINGBUF is the MPSC-safe IPC; relay→desc_ring is single-producer.

DEV-009 CLOSED (Gate 4): C_actual=2 for band66/106PRB phytest; C=24 requires PRIMARY_CONFIG
(MCS28/273PRB). Projection formula in ablation output: proj_c24 = mean_slot * 24/C_actual.

Cost discipline: Phases 1-4 on WSL2 (free). Phase 5 DO droplet only.
PRIMARY_CONFIG 23.4x anchor: UNCHANGED (fixed from v4).
  Gate 5 PARTIAL PASS (2026-06-22):
    verify_cxl_checks.sh 5/5 PASS (QEMU CXL VM, Ubuntu 6.8.0-124)
    PROOF 1 PASS: mbind(node1)+get_mempolicy → numa_node=1, cxl_node=YES
    PROOF 2 PASS (host-side): 50 CB OCL decode via CL_MEM_USE_HOST_PTR on /tmp/cxl_mem.img
    CXLMemSim sweep: FAIL (DEV-022: no PMU on DO KVM droplet)
    /dev/dax0.0 devdax mmap: DEV-023 (ENXIO from device_dax driver)
    DEV-024: system-ram and devdax modes are mutually exclusive
    Droplet cxl-poc: TORN DOWN (billing stopped)

Current phase: 5 COMPLETE → awaiting Gate 5 telemetry before Phase 6
Next action: Phase 6 — paper figures, RESULTS_SUMMARY v5, ops commit
