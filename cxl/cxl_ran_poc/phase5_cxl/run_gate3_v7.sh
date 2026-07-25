#!/usr/bin/env bash
# run_gate3_v7.sh — Gate 3 + 4 evidence collection (v7, bpftime path)
#
# Must run INSIDE the QEMU VM (root@localhost -p 2222).
# Requires:
#   - CXL NUMA node 1 live (vm_cxl_setup.sh already run)
#   - bpftime built: /root/cxl/third_party/bpftime/build/
#   - llr_mover.bpf.o and llr_gate3 already built (make gate3)
#   - ldpc_decoder_benchmark built
#
# Usage: sudo bash run_gate3_v7.sh [--gate 3|4|all]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

GATE="${1:-all}"
[[ "$GATE" == --gate ]] && GATE="${2:-all}"

CXL_DIR=/root/cxl
BPFTIME=$CXL_DIR/third_party/bpftime/build
AGENT=$BPFTIME/runtime/agent/libbpftime-agent.so
SERVER=$BPFTIME/runtime/syscall-server/libbpftime-syscall-server.so

BENCH=$(find $CXL_DIR/third_party/srsRAN_Project/build \
  -name ldpc_decoder_benchmark -type f 2>/dev/null | head -1)
if [[ -z "$BENCH" ]]; then
    echo "ERROR: ldpc_decoder_benchmark not found — rebuild srsRAN"
    exit 1
fi

OFFSET_FILE=/etc/cxl_poc_uprobe_offset
if [[ ! -f "$OFFSET_FILE" ]]; then
    echo "ERROR: $OFFSET_FILE missing — re-derive via:"
    echo "  nm $BENCH | grep -i 'ldpc_decoder_impl.*decode' | awk '{print \$1}' | head -1"
    exit 1
fi
OFFSET=$(cat "$OFFSET_FILE" | tr -d 'UPROBE_OFFSET= \n')

LOG_DIR=/tmp/gate3_v7
mkdir -p "$LOG_DIR"
mkdir -p $CXL_DIR/paper/results

echo "========================================================"
echo "Gate 3 v7 — bpftime LLR mover → CXL node 1 → OpenCL"
echo "========================================================"
echo ""

# ── System state (telemetry: gate_0, gate_2) ─────────────────────────────────
echo "=== [env] dmesg KVM check ==="
dmesg | grep -i "hypervisor\|kvm\|cxl\|virtual" | head -10

echo ""
echo "=== [env] numactl --hardware ==="
numactl --hardware

echo ""
echo "=== [env] uprobe offset ==="
echo "OFFSET_FILE=$OFFSET_FILE"
cat "$OFFSET_FILE"
echo "Parsed offset=$OFFSET"

echo ""
echo "=== [env] bpftime agent/server ==="
ls -la "$AGENT" "$SERVER"

echo ""
echo "=== [env] benchmark binary ==="
ls -la "$BENCH"

# ── Build gate3 artifacts ─────────────────────────────────────────────────────
echo ""
echo "=== [build] make gate3 ==="
BPFTIME=$CXL_DIR/third_party/bpftime
make gate3 \
  BPFTIME_V7=$BPFTIME \
  LIBBPF_V7_A=$BPFTIME/build/libbpf/libbpf/libbpf.a \
  LIBBPF_V7_INC=$BPFTIME/build/libbpf 2>&1
echo "[build] done"

# ── Verify BPF object ==========================================================
echo ""
echo "=== [verify] llr_mover.bpf.o sections ==="
llvm-readelf -S llr_mover.bpf.o | grep -E "uprobe|maps|SEC|Name"

# ── Gate 3: two-terminal bpftime run ─────────────────────────────────────────
echo ""
echo "=== [gate3] Starting ldpc_decoder_benchmark with bpftime agent ==="
LD_PRELOAD=$AGENT \
SPDLOG_LEVEL=warn \
  "$BENCH" -L 384 -I 5 -T avx2 -R 100 \
  > "$LOG_DIR/bench.log" 2>&1 &
BENCH_PID=$!
echo "[gate3] bench PID=$BENCH_PID"
sleep 1  # allow agent to initialise

echo ""
echo "=== [gate3] Starting llr_gate3 with bpftime syscall-server ==="
LD_PRELOAD=$SERVER \
SPDLOG_LEVEL=warn \
  ./llr_gate3 "$BENCH_PID" \
  2>&1 | tee "$LOG_DIR/gate3.log"
GATE3_EXIT=${PIPESTATUS[0]}

# Wait for benchmark to finish
wait "$BENCH_PID" 2>/dev/null || true

echo ""
echo "=== [gate3] benchmark stdout ==="
cat "$LOG_DIR/bench.log"

# ── Gate 4: assembled-run evidence ──────────────────────────────────────────
echo ""
echo "=== [gate4] simultaneous process evidence ==="
echo "[gate4] Launching fresh assembled run for pstree evidence..."

LD_PRELOAD=$AGENT \
SPDLOG_LEVEL=warn \
  "$BENCH" -L 384 -I 5 -T avx2 -R 1000 \
  > "$LOG_DIR/bench_g4.log" 2>&1 &
BENCH4_PID=$!
sleep 0.5

echo ""
echo "--- ps aux snapshot (both procs running) ---"
ps aux | grep -E "ldpc_decoder|llr_gate3" | grep -v grep

echo ""
echo "--- pstree ---"
pstree -p "$BENCH4_PID" 2>/dev/null || pstree | grep -A2 -B2 ldpc

echo ""
echo "--- /proc/${BENCH4_PID}/status ---"
cat "/proc/$BENCH4_PID/status" 2>/dev/null | head -10

kill "$BENCH4_PID" 2>/dev/null || true

# ── e2e_gcp.csv check ────────────────────────────────────────────────────────
echo ""
echo "=== [gate4] e2e_gcp.csv ==="
cat "$CXL_DIR/paper/results/e2e_gcp.csv" 2>/dev/null || echo "(not written — gate3 may have failed)"

# ── Final verdict ─────────────────────────────────────────────────────────────
echo ""
echo "========================================================"
if [[ "$GATE3_EXIT" -eq 0 ]]; then
    echo "GATE3 PASS (exit 0)"
else
    echo "GATE3 FAIL (exit $GATE3_EXIT)"
fi
echo "Logs: $LOG_DIR/"
echo "========================================================"
