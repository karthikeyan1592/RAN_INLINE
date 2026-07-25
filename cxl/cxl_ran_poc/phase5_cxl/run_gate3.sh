#!/usr/bin/env bash
# run_gate3.sh — Phase 3 (v5) Gate 3 execution script.
#
# Sets up network namespaces, starts the bpftime consumer and OAI gNB+UE,
# waits for N_TARGET descriptors, then prints the Gate 3 report.
#
# Usage:  sudo ./run_gate3.sh [N_TARGET]   (default 200)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

N_TARGET="${1:-200}"

OAI_BUILD=/root/linux_env/cxl/third_party/openairinterface5g/cmake_targets/ran_build/build
OAI_CONF=/root/linux_env/cxl/third_party/openairinterface5g/ci-scripts/conf_files
BPFTIME=/root/linux_env/cxl/third_party/bpftime/build
SYSCALL_SRV=$BPFTIME/runtime/syscall-server/libbpftime-syscall-server.so
AGENT=$BPFTIME/runtime/agent/libbpftime-agent.so

GATE3_DIR=/tmp/gate3
mkdir -p "$GATE3_DIR"

# ── 1. Build ──────────────────────────────────────────────────────────────────
echo "[gate3] Building phase3..."
make phase3 2>&1 | tee "$GATE3_DIR/build.log"
echo "[gate3] Build OK"

# ── 2. Network namespaces ─────────────────────────────────────────────────────
echo "[gate3] Setting up network namespaces..."

# Remove stale netns (ignore errors)
ip netns del ue-ns  2>/dev/null || true
ip netns del gnb-ns 2>/dev/null || true

ip netns add ue-ns
ip netns add gnb-ns
ip link add veth-ue  type veth peer name veth-gnb
ip link set veth-ue  netns ue-ns
ip link set veth-gnb netns gnb-ns
ip netns exec ue-ns  ip addr add 10.77.0.1/24 dev veth-ue
ip netns exec gnb-ns ip addr add 10.77.0.2/24 dev veth-gnb
ip netns exec ue-ns  ip link set veth-ue up
ip netns exec gnb-ns ip link set veth-gnb up
ip netns exec ue-ns  ip link set lo up
ip netns exec gnb-ns ip link set lo up

# Quick sanity
ip netns exec ue-ns  ping -c1 -W1 10.77.0.2 >/dev/null
echo "[gate3] netns OK (veth ping passed)"

# ── 3. Kill any stale OAI processes ──────────────────────────────────────────
pkill -f nr-softmodem  2>/dev/null || true
pkill -f nr-uesoftmodem 2>/dev/null || true
pkill -f ldpc_consumer_v5 2>/dev/null || true
sleep 0.5

# ── 4. Start consumer (bpftime syscall-server) ────────────────────────────────
echo "[gate3] Starting ldpc_consumer_v5 (bpftime syscall-server)..."
LD_PRELOAD=$SYSCALL_SRV \
SPDLOG_LEVEL=warn \
BPFTIME_VM_NAME=ubpf \
  ./ldpc_consumer_v5 "$N_TARGET" \
  > "$GATE3_DIR/consumer.log" 2>"$GATE3_DIR/consumer.err" &
CONSUMER_PID=$!
echo "[gate3] consumer PID=$CONSUMER_PID"
sleep 1   # give syscall-server time to initialise

# ── 5. Start gNB with bpftime agent ──────────────────────────────────────────
echo "[gate3] Starting nr-softmodem in gnb-ns..."
ip netns exec gnb-ns env \
    LD_PRELOAD=$AGENT \
    LD_LIBRARY_PATH=$OAI_BUILD \
    SPDLOG_LEVEL=warn \
  $OAI_BUILD/nr-softmodem \
    -O "$OAI_CONF/gnb.band66.106prb.rfsim.phytest-dora.conf" \
    --phy-test --rfsim --noS1 \
    '--rfsimulator.[0].wait_timeout' 20 \
    --log_config.global_log_level warn \
  > "$GATE3_DIR/gnb.log" 2>&1 &
GNB_PID=$!
echo "[gate3] gNB PID=$GNB_PID"
sleep 2   # give gNB time to bind port 4043

# ── 6. Start UE ──────────────────────────────────────────────────────────────
echo "[gate3] Starting nr-uesoftmodem in ue-ns..."
ip netns exec ue-ns env \
    LD_LIBRARY_PATH=$OAI_BUILD \
  $OAI_BUILD/nr-uesoftmodem \
    -O "$OAI_CONF/nrue.uicc.conf" \
    --phy-test --rfsim --noS1 \
    '--rfsimulator.[0].serveraddr' 10.77.0.2 \
    --reconfig-file "$OAI_BUILD/reconfig.raw" \
    --rbconfig-file "$OAI_BUILD/rbconfig.raw" \
    --log_config.global_log_level warn \
  > "$GATE3_DIR/ue.log" 2>&1 &
UE_PID=$!
echo "[gate3] UE PID=$UE_PID"

# ── 7. Wait for consumer to finish (or timeout) ───────────────────────────────
echo "[gate3] Waiting for consumer to collect $N_TARGET descriptors (60 s max)..."
for i in $(seq 1 60); do
    if ! kill -0 $CONSUMER_PID 2>/dev/null; then
        echo "[gate3] Consumer exited after ${i}s"
        break
    fi
    sleep 1
done

# ── 8. Collect logs and cleanup ───────────────────────────────────────────────
echo "[gate3] Stopping all processes..."
kill $CONSUMER_PID $GNB_PID $UE_PID 2>/dev/null || true
wait $CONSUMER_PID $GNB_PID $UE_PID 2>/dev/null || true

# ── 9. Print evidence ─────────────────────────────────────────────────────────
echo ""
echo "=== GATE 3 CONSUMER STDERR ==="
cat "$GATE3_DIR/consumer.err"
echo ""
echo "=== GATE 3 CONSUMER STDOUT ==="
cat "$GATE3_DIR/consumer.log"
echo ""
echo "=== gNB LOG (last 20 lines) ==="
tail -20 "$GATE3_DIR/gnb.log" 2>/dev/null || echo "(no gNB log)"

echo ""
echo "[gate3] All logs in $GATE3_DIR/"
