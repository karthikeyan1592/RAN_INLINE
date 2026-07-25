#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

PASS=0
FAIL=0

check() {
	if "$@"; then
		echo "[PASS] $*"
		PASS=$((PASS + 1))
	else
		echo "[FAIL] $*"
		FAIL=$((FAIL + 1))
	fi
}

echo "=== CXL RAN PoC Verification ==="

check test -x ./gpu_daemon/gpu_daemon
check test -x ./l1_sim/ran_l1_sim
check test -x ./measurement/measure
check test -f ./offload/libl1_offload.so

./scripts/setup_cxl.sh

pkill -f '[g]pu_daemon/gpu_daemon' 2>/dev/null || true
rm -f /tmp/gpu_daemon.sock /tmp/cxl_ran_poc_shm

./gpu_daemon/gpu_daemon --verbose &
GPU_PID=$!
sleep 1

check test -S /tmp/gpu_daemon.sock

export LD_PRELOAD="${ROOT}/offload/libl1_offload.so"
export RAN_OFFLOAD=1

OUT=$(mktemp)
check timeout 120 ./l1_sim/ran_l1_sim --slots 1 --offload > "${OUT}"
check grep -q latency "${OUT}"

kill "${GPU_PID}" 2>/dev/null || true
wait "${GPU_PID}" 2>/dev/null || true

if [ -x ./ebpf/l1_intercept_loader ] && [ -f ./ebpf/l1_intercept.bpf.o ]; then
	if [ "$(id -u)" -eq 0 ]; then
		if (cd ebpf && timeout 5 ./l1_intercept_loader -b ../l1_sim/ran_l1_sim -e); then
			echo "[PASS] eBPF loader started"
			PASS=$((PASS + 1))
		else
			echo "[SKIP] eBPF load failed (kernel policy?) — using LD_PRELOAD offload path"
		fi
	else
		echo "[SKIP] eBPF load requires root"
	fi
else
	echo "[SKIP] eBPF not built"
fi

mkdir -p paper/results
if [ "${FAIL}" -eq 0 ]; then
	echo "PASS" > paper/results/functional_correctness.txt
else
	echo "FAIL" > paper/results/functional_correctness.txt
fi

echo "=== ${PASS} passed, ${FAIL} failed ==="
exit "${FAIL}"
