#!/usr/bin/env bash
# run_e2e_v8.sh — v8 end-to-end pipeline runner (inside QEMU VM)
#
# Runs phases 0-4 in sequence, prints gate evidence after each.
# Must run as root inside the VM (ssh root@localhost -p 2222).
#
# Usage: bash run_e2e_v8.sh [--phase 0|1|2|3|4|all] [--skip-build]
set -euo pipefail

PHASE="${1:-all}"
[[ "$PHASE" == --phase ]] && PHASE="${2:-all}"
SKIP_BUILD=0
[[ "${*}" =~ --skip-build ]] && SKIP_BUILD=1

CXL_DIR=/root/cxl
BPFTIME=$CXL_DIR/third_party/bpftime/build
GATE_DIR=$CXL_DIR/cxl_ran_poc/phase5_cxl
SERVER=$BPFTIME/runtime/syscall-server/libbpftime-syscall-server.so
LOG=/tmp/v8_run.log

mkdir -p $CXL_DIR/paper/results
cd "$GATE_DIR"

ts() { date '+%H:%M:%S'; }
sep() { echo; echo "── $(ts) [$1] ──────────────────────────────────────────"; }

# ── Phase 0: system environment checks ───────────────────────────────────
phase0() {
    sep "PHASE 0: system environment"

    echo "=== dmesg: hypervisor ==="
    dmesg | grep -i "hypervisor\|kvm" | head -5 || echo "(no hypervisor/kvm messages in dmesg)"

    echo ""
    echo "=== /proc/cpuinfo: vmx/kvm ==="
    grep -m1 -E "vmx|hypervisor" /proc/cpuinfo || echo "(no vmx/hypervisor flag)"
    echo "/dev/kvm: $(ls /dev/kvm 2>/dev/null && echo present || echo absent)"
    echo "systemd-detect-virt: $(systemd-detect-virt 2>/dev/null || echo unknown)"

    echo ""
    echo "=== srsRAN benchmark ==="
    BENCH=$(find $CXL_DIR/third_party/srsRAN_Project/build \
      -name ldpc_decoder_benchmark -type f 2>/dev/null | head -1 || true)
    # Fall back to symlink installed by build_tools.sh
    [[ -z "$BENCH" ]] && BENCH=$(command -v ldpc_decoder_benchmark 2>/dev/null || true)
    [[ -z "$BENCH" ]] && BENCH=/usr/local/bin/ldpc_decoder_benchmark
    echo "  path: $BENCH"
    ls -la "$BENCH"
    "$BENCH" -L 384 -I 5 -T avx2 -R 1 2>&1 | tail -5

    echo ""
    echo "=== bpftime agent/server ==="
    ls -la $BPFTIME/runtime/agent/libbpftime-agent.so
    ls -la $BPFTIME/runtime/syscall-server/libbpftime-syscall-server.so

    echo ""
    echo "=== PoCL clinfo ==="
    clinfo 2>/dev/null | grep -E "Platform|Device Name|Type" | head -6 || echo "(clinfo unavailable)"

    echo ""
    echo "=== uprobe offset ==="
    cat /etc/cxl_poc_uprobe_offset

    echo ""
    # KVM evidence: /dev/kvm present OR vmx in cpuinfo OR systemd-detect-virt says kvm/qemu
    KVM_OK=0
    [ -e /dev/kvm ] && KVM_OK=1
    grep -q vmx /proc/cpuinfo 2>/dev/null && KVM_OK=1
    echo "GATE 0: /dev/kvm=$(ls /dev/kvm 2>/dev/null && echo present || echo absent) vmx=$(grep -c vmx /proc/cpuinfo 2>/dev/null || echo 0) → $([ $KVM_OK -eq 1 ] && echo PASS || echo FAIL)"
}

# ── Phase 1: CXL NUMA topology ────────────────────────────────────────────
phase1() {
    sep "PHASE 1: CXL NUMA topology"

    echo "=== numactl --hardware ==="
    numactl --hardware

    echo ""
    echo "=== daxctl list ==="
    daxctl list 2>/dev/null || echo "(daxctl not available)"

    echo ""
    echo "=== numactl --membind=1 test ==="
    numactl --membind=1 ls /tmp 2>&1 && echo "membind=1: OK" \
      || echo "SIGILL: mbind shim required (DEV-033 expected — llr_consumer_v8 handles this)"

    echo ""
    echo "=== gate0_option_a (mbind shim test) ==="
    ./gate0_option_a 2>&1 | head -10

    NODE1_MB=$(numactl --hardware | grep "^node 1 size" | awk '{print $4}' || true)
    echo ""
    echo "GATE 1: node_1_mb=${NODE1_MB} $([ "${NODE1_MB:-0}" -ge 1800 ] && echo PASS || echo FAIL)"
}

# ── Build ─────────────────────────────────────────────────────────────────
do_build() {
    sep "BUILD"
    if [[ "$SKIP_BUILD" -eq 1 ]]; then
        echo "Skipping build (--skip-build)"
        ls -la lddc_llr_mover.bpf.o llr_consumer_v8
        return
    fi

    # Ensure vmlinux.h exists
    if [[ ! -f vmlinux.h ]]; then
        echo "Generating vmlinux.h..."
        bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
    fi

    BPFTIME_SRC=$CXL_DIR/third_party/bpftime
    # Check which libbpf path exists (build path varies by bpftime version)
    LIBBPF_A=""
    for p in \
        "$BPFTIME_SRC/build/libbpf/libbpf/libbpf.a" \
        "$BPFTIME_SRC/build/libbpf/libbpf.a" \
        "$BPFTIME_SRC/build/libbpf/liblibbpf.a" \
        "/usr/lib/$(uname -m)-linux-gnu/libbpf.a"; do
        [[ -f "$p" ]] && LIBBPF_A="$p" && break
    done
    [[ -z "$LIBBPF_A" ]] && { echo "ERROR: libbpf.a not found"; exit 1; }
    echo "Using libbpf: $LIBBPF_A"

    make gate-v8 \
        BPFTIME_V7=$BPFTIME_SRC \
        LIBBPF_V7_A=$LIBBPF_A \
        LIBBPF_V7_INC=$BPFTIME_SRC/build/libbpf \
        2>&1
    echo "Build OK"
    ls -la lddc_llr_mover.bpf.o cxl_init.so llr_consumer_v8
}

# ── Phase 2 + 3 + 4: run consumer (under bpftime syscall-server) ──────────
phase_e2e() {
    sep "PHASE 2/3/4: CXL + bpftime + OCL assembled run"

    echo "Starting llr_consumer_v8 under bpftime syscall-server..."
    echo "(consumer will fork benchmark, attach uprobe, busy-poll, write CSV)"
    echo ""

    # DEV-038: wipe stale bpftime shm so server creates a fresh one; otherwise
    # agent connects to old maps from a prior run and misses the new uprobe entry.
    rm -f /dev/shm/bpftime_maps_shm
    echo "[e2e] cleared stale bpftime shm"

    LD_PRELOAD=$SERVER \
    SPDLOG_LEVEL=warn \
    BPFTIME_VM_NAME=ubpf \
        ./llr_consumer_v8 2>&1 | tee /tmp/v8_consumer.log

    echo ""
    echo "=== e2e_gcp.csv (first 5 data rows) ==="
    head -6 $CXL_DIR/paper/results/e2e_gcp.csv 2>/dev/null || echo "(CSV not written)"

    echo ""
    echo "=== e2e_gcp.csv row count ==="
    wc -l $CXL_DIR/paper/results/e2e_gcp.csv 2>/dev/null || true
}

# ── Execute requested phases ──────────────────────────────────────────────
case "$PHASE" in
    0) phase0 ;;
    1) phase1 ;;
    4|e2e) do_build; phase_e2e ;;
    all)
        phase0
        phase1
        do_build
        phase_e2e
        ;;
    *)
        echo "Usage: $0 [--phase 0|1|4|all] [--skip-build]"
        exit 1
        ;;
esac

echo ""
echo "Full log: /tmp/v8_consumer.log"
echo "CSV: $CXL_DIR/paper/results/e2e_gcp.csv"
