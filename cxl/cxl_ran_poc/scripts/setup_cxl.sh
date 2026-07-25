#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS="${ROOT}/paper/results"
mkdir -p "${RESULTS}"

if command -v cxl >/dev/null 2>&1; then
	cxl list || true
fi

if ls /dev/dax* >/dev/null 2>&1; then
	echo "CXL/DAX device found: $(ls /dev/dax*)"
	echo "qemu-cxl-type3" > "${RESULTS}/emulation_mode.txt"
else
	echo "No DAX device — using shared mmap NUMA fallback"
	echo "mmap-shm-fallback" > "${RESULTS}/emulation_mode.txt"
	truncate -s 2G /tmp/cxl_ran_poc_shm 2>/dev/null || true
fi
