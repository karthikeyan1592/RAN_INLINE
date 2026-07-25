#!/bin/bash
# Run INSIDE the QEMU VM to start all pipeline processes in order, then
# execute the measurement suite.
#
# Pipeline:
#   1. gpu_daemon       — waits on Unix socket + CXL shared memory
#   2. l1_intercept_loader — attaches eBPF uprobes to the L1 benchmark binary
#   3. measurement/measure (or srsRAN ldpc_decoder_benchmark) — the L1 workload
#
# Usage:
#   /opt/cxl_ran_poc/scripts/in_vm_run.sh [--slots N] [--srsran] [--baseline-only]
#
# Requires: in_vm_setup.sh to have been run first (as root)

set -euo pipefail

POC_DIR="/opt/cxl_ran_poc"
LOG_DIR="/var/log/cxl_ran_poc"
RESULTS_DIR="${POC_DIR}/paper/results"
SOCKET_PATH="/tmp/gpu_daemon.sock"
SHM_PATH="/tmp/cxl_ran_poc_shm"
SLOTS=1000
USE_SRSRAN=false
BASELINE_ONLY=false
VERBOSE=false

# ── parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --slots)         SLOTS="$2";        shift 2 ;;
        --srsran)        USE_SRSRAN=true;   shift   ;;
        --baseline-only) BASELINE_ONLY=true; shift  ;;
        --verbose)       VERBOSE=true;      shift   ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

mkdir -p "$LOG_DIR" "$RESULTS_DIR"

# eBPF uprobes require perf_event_paranoid <= 0.  Ubuntu 24.04 ships with 4.
if [[ $(cat /proc/sys/kernel/perf_event_paranoid) -gt 0 ]]; then
    sysctl -q kernel.perf_event_paranoid=-1 2>/dev/null || true
fi

echo "=== CXL RAN PoC — in-VM run  $(date -Iseconds) ==="
echo "    PoC dir : ${POC_DIR}"
echo "    Slots   : ${SLOTS}"
echo "    srsRAN  : ${USE_SRSRAN}"
echo ""

# ── helper: cleanup on exit ──────────────────────────────────────────────────
GPU_DAEMON_PID=""
EBPF_LOADER_PID=""

cleanup() {
    echo ""
    echo "=== Cleaning up ==="
    [[ -n "$EBPF_LOADER_PID" ]] && kill "$EBPF_LOADER_PID" 2>/dev/null || true
    [[ -n "$GPU_DAEMON_PID"  ]] && kill "$GPU_DAEMON_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$SOCKET_PATH"
}
trap cleanup EXIT INT TERM

# ── determine CXL device path ────────────────────────────────────────────────
CXL_PATH="/dev/dax0.0"
if [[ ! -c "$CXL_PATH" && ! -b "$CXL_PATH" ]]; then
    echo "WARN: ${CXL_PATH} not found — using mmap-shm fallback"
    CXL_PATH=""
    echo "mmap-shm-fallback" > "${RESULTS_DIR}/emulation_mode.txt"
else
    echo "CXL device: ${CXL_PATH}"
    echo "qemu-cxl-type3" > "${RESULTS_DIR}/emulation_mode.txt"
fi

# ── 1. start GPU daemon ──────────────────────────────────────────────────────
pkill -f "${POC_DIR}/gpu_daemon/gpu_daemon" 2>/dev/null || true
rm -f "$SOCKET_PATH" "$SHM_PATH"

DAEMON_CMD="${POC_DIR}/gpu_daemon/gpu_daemon --socket ${SOCKET_PATH}"
[[ -n "$CXL_PATH" ]]  && DAEMON_CMD+=" --cxl-path ${CXL_PATH}"
[[ "$VERBOSE" == true ]] && DAEMON_CMD+=" --verbose"

echo "[1/4] Starting GPU daemon..."
$DAEMON_CMD >"${LOG_DIR}/gpu_daemon.log" 2>&1 &
GPU_DAEMON_PID=$!

# wait for socket to appear
for i in $(seq 1 20); do
    [[ -S "$SOCKET_PATH" ]] && break
    sleep 0.5
    if [[ "$i" -eq 20 ]]; then
        echo "ERROR: GPU daemon socket not ready after 10 s" >&2
        cat "${LOG_DIR}/gpu_daemon.log" >&2
        exit 1
    fi
done
echo "    GPU daemon ready (pid=${GPU_DAEMON_PID})"

# ── 2. determine L1 benchmark binary ────────────────────────────────────────
if [[ "$USE_SRSRAN" == true ]]; then
    L1_BINARY=$(command -v ldpc_decoder_benchmark 2>/dev/null || \
                ls /opt/srsran/build/**/ldpc_decoder_benchmark 2>/dev/null | head -1 || true)
    if [[ -z "$L1_BINARY" ]]; then
        echo "ERROR: ldpc_decoder_benchmark not found. Did cloud-init provisioning complete?" >&2
        echo "       Check /var/log/cxl_provision/srsran_build.log" >&2
        exit 1
    fi
    echo "L1 binary (srsRAN): ${L1_BINARY}"
else
    L1_BINARY="${POC_DIR}/l1_sim/ran_l1_sim"
    echo "L1 binary (PoC sim): ${L1_BINARY}"
fi

# ── 3. attach eBPF uprobes ───────────────────────────────────────────────────
echo "[2/4] Attaching eBPF uprobes to ${L1_BINARY##*/}..."
EBPF_LOADER="${POC_DIR}/ebpf/l1_intercept_loader"

if [[ ! -x "$EBPF_LOADER" ]]; then
    echo "WARN: eBPF loader not built — skipping uprobe attachment"
    echo "      Run: make ebpf  inside ${POC_DIR}"
elif [[ "$(id -u)" -ne 0 ]]; then
    echo "WARN: eBPF requires root — skipping uprobe attachment (re-run as root)"
else
    (cd "${POC_DIR}/ebpf" && \
     "${EBPF_LOADER}" \
        -b "${L1_BINARY}" \
        -s "${SOCKET_PATH}" \
        -e \
     >"${LOG_DIR}/ebpf_loader.log" 2>&1) &
    EBPF_LOADER_PID=$!
    sleep 0.5
    if kill -0 "$EBPF_LOADER_PID" 2>/dev/null; then
        echo "    eBPF loader running (pid=${EBPF_LOADER_PID})"
    else
        echo "WARN: eBPF loader exited — check ${LOG_DIR}/ebpf_loader.log"
        EBPF_LOADER_PID=""
    fi
fi

# ── 4. run measurements ──────────────────────────────────────────────────────
MEASURE="${POC_DIR}/measurement/measure"
export LD_PRELOAD="${POC_DIR}/offload/libl1_offload.so"

echo "[3/4] Running calibration..."
"$MEASURE" --mode calibration

if [[ "$BASELINE_ONLY" == false ]]; then
    echo "[4/4a] CPU-only baseline (${SLOTS} slots)..."
    unset RAN_OFFLOAD
    "$MEASURE" --mode baseline --slots "${SLOTS}" \
        --output "${RESULTS_DIR}/baseline_latency.csv" \
        --label "cpu-baseline"

    echo "[4/4b] eBPF + CXL offload path (${SLOTS} slots)..."
    export RAN_OFFLOAD=1
    "$MEASURE" --mode offload --slots "${SLOTS}" \
        --output "${RESULTS_DIR}/offload_latency.csv" \
        --label "ebpf-cxl-offload"

    echo "[4/4c] eBPF overhead proxy (500 samples)..."
    "$MEASURE" --mode ebpf-overhead --slots 500 \
        --output "${RESULTS_DIR}/ebpf_overhead_only.csv"
else
    echo "[4/4] CPU-only baseline (${SLOTS} slots)..."
    unset RAN_OFFLOAD
    "$MEASURE" --mode baseline --slots "${SLOTS}" \
        --output "${RESULTS_DIR}/baseline_latency.csv" \
        --label "cpu-baseline"
fi

# ── 5. optional: srsRAN benchmark comparison ─────────────────────────────────
if [[ "$USE_SRSRAN" == true && -x "$L1_BINARY" ]]; then
    echo ""
    echo "=== srsRAN ldpc_decoder_benchmark ==="
    "$L1_BINARY" --nof-repetitions 1000 2>&1 | tee "${RESULTS_DIR}/srsran_ldpc_benchmark.txt" || true
fi

# ── 6. generate paper notes + figures ────────────────────────────────────────
echo ""
echo "=== Generating paper artefacts ==="
python3 "${POC_DIR}/measurement/generate_notes.py" || true
python3 "${POC_DIR}/measurement/plot_results.py" \
    --baseline "${RESULTS_DIR}/baseline_latency.csv" \
    --offload  "${RESULTS_DIR}/offload_latency.csv" \
    --output-dir "${POC_DIR}/paper/figures/" || true

echo ""
echo "=== Run complete ==="
echo "Results : ${RESULTS_DIR}/"
echo "Figures : ${POC_DIR}/paper/figures/"
echo "Logs    : ${LOG_DIR}/"
