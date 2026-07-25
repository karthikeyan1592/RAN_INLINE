#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

NUMA_NODES=$(numactl --hardware 2>/dev/null | grep "available:" | awk '{print $2}' || echo 1)
SLOTS=${SLOTS:-1000}

export LD_PRELOAD="${ROOT}/offload/libl1_offload.so"
export RAN_OFFLOAD=1

./gpu_daemon/gpu_daemon --verbose &
GPU_PID=$!
sleep 1

echo "NUMA nodes available: ${NUMA_NODES}"

if [ "${NUMA_NODES}" -lt 2 ]; then
	echo "WARN: Single NUMA node — local memory only"
	numactl --cpunodebind=0 --membind=0 \
		./measurement/measure --mode offload --slots "${SLOTS}" \
		--output paper/results/numa_latency_0ns.csv \
		--label "local-dram-0ns"
	echo "Latency sensitivity analysis skipped — single NUMA node host" \
		>> paper/results/emulation_mode.txt
	kill "${GPU_PID}" 2>/dev/null || true
	exit 0
fi

numactl --cpunodebind=0 --membind=0 \
	./measurement/measure --mode offload --slots "${SLOTS}" \
	--output paper/results/numa_latency_0ns.csv --label "local-dram-0ns"

numactl --cpunodebind=0 --membind=1 \
	./measurement/measure --mode offload --slots "${SLOTS}" \
	--output paper/results/numa_latency_142ns.csv \
	--label "numa-emulated-cxl-142ns"

numactl --cpunodebind=0 --membind=1 \
	./measurement/measure --mode offload --slots "${SLOTS}" \
	--output paper/results/numa_latency_255ns.csv \
	--label "numa-emulated-cxl-255ns"

kill "${GPU_PID}" 2>/dev/null || true
echo "NUMA sweep complete."
