#!/usr/bin/env bash
# run_gate4.sh — Phase 4 (v5) Gate 4 evidence collection
#
# Runs TWO modes sequentially (each with a fresh bpftime + gNB + UE):
#   1. interception_only  — descriptor + busy-poll, no OCL
#   2. gpu_compute_full   — same + OpenCL LDPC decode over CXL stand-in
#
# Gate 4 PASS criteria:
#   - interception_only median (p50) improved vs v4's 2636us; document actual number
#   - OAI gNB logs show ULSCH CRC ok (gNB's own decoder succeeds)
#   - gpu_compute_full bit_density > 0.05 (non-trivial OCL decoded output)
#   - CSV latency_ladder_v2_v5.csv written with all 3 rows (baseline FIXED)
#   - C_actual=2 used, DEV-009 projection formula shown
#
# Usage: sudo bash run_gate4.sh [interception_only|gpu_compute_full|all]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BPFTIME=/root/linux_env/cxl/third_party/bpftime/build
OAI_BUILD=/root/linux_env/cxl/third_party/openairinterface5g/cmake_targets/ran_build/build
OAI_CONF=/root/linux_env/cxl/third_party/openairinterface5g/ci-scripts/conf_files
CL_PATH="$SCRIPT_DIR/../gpu_daemon/ldpc_cl/ldpc_decode.cl"
LOG_DIR=/tmp/gate4
N_CBS=2000
C_ACTUAL=2    # confirmed from v4 Gate 2 (band66/106PRB phytest)

TARGET="${1:-all}"

mkdir -p "$LOG_DIR" ../paper/results

cleanup_bpftime() {
    echo "[gate4] Cleaning bpftime shared memory..."
    pkill -f "libbpftime-syscall-server|nr-softmodem|nr-uesoftmodem|ablation" 2>/dev/null || true
    rm -f /dev/shm/bpftime_* /dev/shm/*bpftime* /dev/shm/*uprobe* 2>/dev/null || true
    sleep 1
}

setup_netns() {
    ip netns del ue-ns  2>/dev/null || true
    ip netns del gnb-ns 2>/dev/null || true
    ip netns add ue-ns && ip netns add gnb-ns
    ip link add veth-ue type veth peer name veth-gnb
    ip link set veth-ue netns ue-ns && ip link set veth-gnb netns gnb-ns
    ip netns exec ue-ns  ip addr add 10.77.0.1/24 dev veth-ue
    ip netns exec gnb-ns ip addr add 10.77.0.2/24 dev veth-gnb
    ip netns exec ue-ns  ip link set veth-ue up && ip netns exec gnb-ns ip link set veth-gnb up
    ip netns exec ue-ns  ip link set lo up      && ip netns exec gnb-ns ip link set lo up
}

run_mode() {
    local MODE="$1"
    echo ""
    echo "[gate4] ===== MODE: $MODE ====="

    cleanup_bpftime
    setup_netns

    # Remove stale CSV row for this mode to allow fresh append
    # (first call writes header; subsequent calls append mode row)

    # Start ablation harness (bpftime syscall-server)
    LD_PRELOAD="$BPFTIME/runtime/syscall-server/libbpftime-syscall-server.so" \
    SPDLOG_LEVEL=warn BPFTIME_VM_NAME=ubpf \
        ./ablation \
          --mode "$MODE" \
          --n-cbs "$N_CBS" \
          --c-actual "$C_ACTUAL" \
          --cl-path "$CL_PATH" \
        > "$LOG_DIR/${MODE}.log" 2> "$LOG_DIR/${MODE}.err" &
    ABLATION_PID=$!

    sleep 5  # allow syscall-server to initialise and register uprobe in shm

    # gNB with bpftime agent in gnb-ns
    ip netns exec gnb-ns env \
        LD_PRELOAD="$BPFTIME/runtime/agent/libbpftime-agent.so" \
        LD_LIBRARY_PATH="$OAI_BUILD" SPDLOG_LEVEL=warn \
        "$OAI_BUILD/nr-softmodem" \
          -O "$OAI_CONF/gnb.band66.106prb.rfsim.phytest-dora.conf" \
          --phy-test --rfsim --noS1 \
          '--rfsimulator.[0].wait_timeout' 20 \
          --log_config.global_log_level info \
        >> "$LOG_DIR/${MODE}_gnb.log" 2>&1 &
    GNB_PID=$!

    sleep 2  # gNB binds port 4043

    # UE in ue-ns (no bpftime agent)
    ip netns exec ue-ns env LD_LIBRARY_PATH="$OAI_BUILD" \
        "$OAI_BUILD/nr-uesoftmodem" \
          -O "$OAI_CONF/nrue.uicc.conf" \
          --phy-test --rfsim --noS1 \
          '--rfsimulator.[0].serveraddr' 10.77.0.2 \
          --reconfig-file "$OAI_BUILD/reconfig.raw" \
          --rbconfig-file "$OAI_BUILD/rbconfig.raw" \
          --log_config.global_log_level info \
        >> "$LOG_DIR/${MODE}_ue.log" 2>&1 &
    UE_PID=$!

    echo "[gate4] ablation=$ABLATION_PID  gNB=$GNB_PID  UE=$UE_PID"

    local TIMEOUT=300
    local ELAPSED=0
    while kill -0 "$ABLATION_PID" 2>/dev/null && [ "$ELAPSED" -lt "$TIMEOUT" ]; do
        sleep 5; ELAPSED=$((ELAPSED + 5))
        echo "[gate4] ... ${ELAPSED}s — $(grep 'n_received' "$LOG_DIR/${MODE}.err" 2>/dev/null | tail -1 || true)"
    done

    if kill -0 "$ABLATION_PID" 2>/dev/null; then
        echo "[gate4] WARN: ablation timed out after ${TIMEOUT}s"
        kill "$ABLATION_PID" 2>/dev/null || true
    fi

    # Kill gNB + UE (suppress non-zero exit from killed processes)
    kill "$GNB_PID" "$UE_PID" 2>/dev/null || true
    wait "$GNB_PID" 2>/dev/null || true
    wait "$UE_PID"  2>/dev/null || true

    echo ""
    echo "[gate4] $MODE result:"
    cat "$LOG_DIR/${MODE}.err"
}

echo "[gate4] Building phase4..."
make phase4 2>&1 | tail -3

# Remove old CSV to start fresh
rm -f ../paper/results/latency_ladder_v2_v5.csv

# Update CSV path in ablation to use v5-specific file
# (We patch the CSV name here via env var; ablation uses fixed path — copy after)

if [ "$TARGET" = "all" ] || [ "$TARGET" = "interception_only" ]; then
    run_mode interception_only
fi

if [ "$TARGET" = "all" ] || [ "$TARGET" = "gpu_compute_full" ]; then
    run_mode gpu_compute_full
fi

# Rename CSV to v5-specific name
mv -f ../paper/results/latency_ladder_v2.csv ../paper/results/latency_ladder_v2_v5.csv 2>/dev/null || true

echo ""
echo "[gate4] ===== GATE 4 SUMMARY ====="
echo ""
echo "--- OAI CRC evidence (gNB log, interception_only run) ---"
grep -i "CRC\|harq\|ulsch" "$LOG_DIR/interception_only_gnb.log" 2>/dev/null | \
  grep -v "^#\|HARQ feedback enabled" | tail -20 || echo "(no ULSCH CRC lines)"
echo ""
echo "--- latency_ladder_v2_v5.csv ---"
cat ../paper/results/latency_ladder_v2_v5.csv 2>/dev/null || echo "(CSV not found)"
echo ""
echo "[gate4] Logs: $LOG_DIR/"
