#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

echo "=== CXL RAN PoC Integration Test ==="

./scripts/setup_cxl.sh

echo "[1/6] Building..."
make all

echo "[2/6] Starting GPU accelerator daemon..."
pkill -f '[g]pu_daemon/gpu_daemon' 2>/dev/null || true
rm -f /tmp/gpu_daemon.sock /tmp/cxl_ran_poc_shm
./gpu_daemon/gpu_daemon --cxl-path /dev/dax0.0 --socket /tmp/gpu_daemon.sock --verbose &
GPU_DAEMON_PID=$!
sleep 1

echo "[3/6] Calibration..."
export LD_PRELOAD="${ROOT}/offload/libl1_offload.so"
./measurement/measure --mode calibration

SLOTS=${SLOTS:-1000}

echo "[4/6] Baseline (CPU only)..."
unset RAN_OFFLOAD
./measurement/measure --mode baseline --slots "${SLOTS}" \
	--output paper/results/baseline_latency.csv

echo "[5/6] Offload path..."
export RAN_OFFLOAD=1
./measurement/measure --mode offload --slots "${SLOTS}" \
	--output paper/results/offload_latency.csv

echo "[6/6] eBPF overhead proxy..."
./measurement/measure --mode ebpf-overhead --slots 500 \
	--output paper/results/ebpf_overhead_only.csv

kill "${GPU_DAEMON_PID}" 2>/dev/null || true
wait "${GPU_DAEMON_PID}" 2>/dev/null || true

python3 measurement/plot_results.py \
	--baseline paper/results/baseline_latency.csv \
	--offload paper/results/offload_latency.csv \
	--output-dir paper/figures/

python3 measurement/generate_notes.py || true

echo "=== Done. Results in paper/results/, figures in paper/figures/ ==="
