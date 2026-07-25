#!/bin/bash
# run_e2e_test.sh — CXL PoC v6 end-to-end test trigger (GCP)
#
# Runs the full v6 pipeline against the QEMU CXL VM on the GCP host:
#   Gate 0 — CXL NUMA topology: node 1 reachable, zero-copy via CL_MEM_USE_HOST_PTR
#   Gate 1 — Uprobe fires: kernel tracefs uprobe on ldpc_decoder_benchmark
#   Gate 2 — Full pipeline: uprobe → move_pages → /proc/mem → OpenCL over CXL
#
# Prerequisites (run once before first use):
#   ./provision.sh → ./install_deps.sh → ./rsync_source.sh → ./build_tools.sh
#   ./prepare_vm.sh → ./launch_vm.sh
#
# Usage:
#   ./run_e2e_test.sh                  # full run: all three gates
#   ./run_e2e_test.sh --gate 0         # single gate
#   ./run_e2e_test.sh --gate 2
#   ./run_e2e_test.sh --rebuild-vm     # force rebuild of VM binaries before test
#   ./run_e2e_test.sh --skip-rsync     # skip source rsync (binaries already in VM)
#
# Results:
#   Collected to paper/results/droplet/e2e_gcp.csv (on GCP host)
#   Rsync'd back to local: paper/results/droplet/e2e_gcp.csv
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first (.gcp_instance_ip missing)"; exit 1; }
GCP_IP=$(cat "$IP_FILE")
GCP_USER="${GCP_USER:-karthix25}"
GCP_KEY="${GCP_KEY:-$HOME/.ssh/id_ed25519}"
VM_KEY_PATH="/home/karthix25/.ssh/vm_key"   # path ON GCP host

GCP_SSH="ssh -i $GCP_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=15 -o ServerAliveInterval=30"
# VM SSH proxied through GCP host (port 2222 on GCP host)
VM_SSH_PROXY="$GCP_SSH -o ProxyJump=${GCP_USER}@${GCP_IP} -i $GCP_KEY"
VM_SSH_VIA_GCP="$GCP_SSH ${GCP_USER}@${GCP_IP} ssh -i $VM_KEY_PATH -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@localhost"

# Parse args
GATE_ONLY=""
REBUILD_VM=0
SKIP_RSYNC=0
while [ $# -gt 0 ]; do
  case "$1" in
    --gate)       GATE_ONLY="$2"; shift ;;
    --rebuild-vm) REBUILD_VM=1 ;;
    --skip-rsync) SKIP_RSYNC=1 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
  shift
done

# ── Helpers ──────────────────────────────────────────────────────────────────
vm_run() {
  # Run command string inside QEMU VM via GCP host
  $GCP_SSH "${GCP_USER}@${GCP_IP}" \
    "ssh -i $VM_KEY_PATH -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@localhost $*"
}

vm_run_script() {
  # Pipe heredoc into VM via GCP host
  $GCP_SSH "${GCP_USER}@${GCP_IP}" \
    "ssh -i $VM_KEY_PATH -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@localhost bash -s"
}

echo "=== CXL PoC v6 E2E Test ==="
echo "  GCP host: $GCP_IP"
[ -n "$GATE_ONLY" ] && echo "  Gate only: $GATE_ONLY" || echo "  Gates: 0, 1, 2"
echo ""

# ── Step 1: Verify GCP host reachable ────────────────────────────────────────
echo "── Check 1: GCP host SSH ──"
$GCP_SSH "${GCP_USER}@${GCP_IP}" uname -r
echo "  GCP host: OK"

# ── Step 2: Verify QEMU VM is running ────────────────────────────────────────
echo ""
echo "── Check 2: QEMU VM ──"
QEMU_STATUS=$($GCP_SSH "${GCP_USER}@${GCP_IP}" \
  'pgrep qemu-system > /dev/null 2>&1 && echo running || echo stopped')

if [ "$QEMU_STATUS" = "stopped" ]; then
  echo "  QEMU not running — launching VM (this takes ~2-3 min for boot)"
  "$SCRIPT_DIR/scripts/launch_vm.sh"
else
  echo "  QEMU: running (PID $($GCP_SSH ${GCP_USER}@${GCP_IP} 'cat /tmp/qemu.pid 2>/dev/null || pgrep qemu-system | head -1'))"
fi

echo "  VM SSH test..."
vm_run "uname -r"
echo "  VM: OK"

# ── Step 3: Verify CXL topology ──────────────────────────────────────────────
echo ""
echo "── Check 3: CXL NUMA topology ──"
NUMA_NODES=$(vm_run "numactl --hardware 2>/dev/null | grep '^available:' | awk '{print \$2}'" 2>/dev/null || echo "0")
if [ "$NUMA_NODES" != "2" ]; then
  echo "  Only $NUMA_NODES NUMA node(s) found — running vm_cxl_setup.sh..."
  vm_run_script << 'INNER'
bash /root/cxl/ops/gcp-cxl-lab/scripts/vm_cxl_setup.sh 2>&1 | tail -10
INNER
  NUMA_NODES=$(vm_run "numactl --hardware 2>/dev/null | grep '^available:' | awk '{print \$2}'" || echo "0")
fi
if [ "$NUMA_NODES" != "2" ]; then
  echo "  ERROR: CXL node 1 not available after vm_cxl_setup. Check QEMU logs."
  exit 1
fi
echo "  NUMA nodes: $NUMA_NODES (DRAM + CXL) — OK"

# ── Step 4: Rsync source tree GCP host → VM ──────────────────────────────────
if [ "$SKIP_RSYNC" -eq 0 ]; then
  echo ""
  echo "── Step 4: Rsync source to VM ──"
  $GCP_SSH "${GCP_USER}@${GCP_IP}" 'bash -s' << 'REMOTE'
set -euo pipefail
rsync -az --progress \
  --exclude '*/build' --exclude '*/build_*' \
  --exclude '.git' --exclude '__pycache__' \
  --exclude '*.o' --exclude '*.a' --exclude '*.so' \
  -e "ssh -i /home/karthix25/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no" \
  /home/karthix25/cxl/ \
  root@localhost:/root/cxl/ \
  2>&1 | tail -4
echo "  rsync: done"
REMOTE
else
  echo "── Step 4: rsync skipped (--skip-rsync) ──"
fi

# ── Step 5: Build VM-specific binaries ───────────────────────────────────────
echo ""
echo "── Step 5: Build VM binaries ──"

BUILD_NEEDED=0
if [ "$REBUILD_VM" -eq 1 ]; then
  BUILD_NEEDED=1
else
  # Check if gate binaries already exist inside VM
  HAVE_GATE0=$(vm_run "test -f /root/cxl/cxl_ran_poc/phase5_cxl/gate0_option_a && echo yes || echo no" 2>/dev/null || echo no)
  HAVE_GATE2=$(vm_run "test -f /root/cxl/cxl_ran_poc/phase5_cxl/gate2_xproc && echo yes || echo no" 2>/dev/null || echo no)
  HAVE_BENCH=$(vm_run "test -f /usr/local/bin/ldpc_decoder_benchmark && echo yes || echo no" 2>/dev/null || echo no)
  [ "$HAVE_GATE0" = "no" ] || [ "$HAVE_GATE2" = "no" ] || [ "$HAVE_BENCH" = "no" ] && BUILD_NEEDED=1
  echo "  gate0_option_a: $HAVE_GATE0 | gate2_xproc: $HAVE_GATE2 | ldpc_bench: $HAVE_BENCH"
fi

if [ "$BUILD_NEEDED" -eq 1 ]; then
  echo "  Building inside VM (first run takes ~20 min for srsRAN)..."
  vm_run_script << 'INNER'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
BASE=/root/cxl

# Install VM build deps (idempotent)
apt-get install -y -qq \
  build-essential cmake ninja-build libelf-dev zlib1g-dev \
  libboost-all-dev libfftw3-dev libgtest-dev libssl-dev \
  libnuma-dev libOpenCL-dev 2>/dev/null | tail -3 || \
  apt-get install -y -qq \
  build-essential cmake libelf-dev zlib1g-dev \
  libboost-all-dev libfftw3-dev libssl-dev \
  libnuma-dev opencl-dev 2>/dev/null | tail -3 || true

# srsRAN ldpc_decoder_benchmark (the Gate 1/2 workload binary)
if [ ! -f /usr/local/bin/ldpc_decoder_benchmark ]; then
  echo "[vm-build] srsRAN ldpc_decoder_benchmark (~15 min)..."
  cd "$BASE/third_party/srsRAN_Project"
  mkdir -p build && cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    -DENABLE_EXPORT=ON -DENABLE_UHD=OFF -DENABLE_ZEROMQ=OFF \
    -DENABLE_FFTW=ON 2>&1 | tail -3
  make -j$(nproc) ldpc_decoder_benchmark 2>&1 | tail -10
  BENCH=$(find . -name ldpc_decoder_benchmark -type f | head -1)
  ln -sf "$(pwd)/$BENCH" /usr/local/bin/ldpc_decoder_benchmark
  echo "[vm-build] srsRAN: done  →  /usr/local/bin/ldpc_decoder_benchmark"
else
  echo "[vm-build] srsRAN: already built"
fi

# Derive uprobe offset for THIS VM build
OFFSET=$(nm /usr/local/bin/ldpc_decoder_benchmark 2>/dev/null | \
  grep -i "ldpc_decoder_impl.*decode\b" | awk '{print $1}' | head -1)
if [ -n "$OFFSET" ]; then
  echo "UPROBE_OFFSET=0x${OFFSET}" > /etc/cxl_poc_uprobe_offset
  echo "[vm-build] uprobe offset: 0x${OFFSET} (saved to /etc/cxl_poc_uprobe_offset)"
else
  echo "[vm-build] WARNING: could not derive uprobe offset"
fi

# gate0_option_a
cd "$BASE/cxl_ran_poc/phase5_cxl"
if [ ! -f gate0_option_a ] || [ "$(find . -name gate0_option_a.c -newer gate0_option_a)" ]; then
  echo "[vm-build] gate0_option_a..."
  gcc -O2 -Wall gate0_option_a.c -lnuma -lOpenCL -o gate0_option_a 2>&1
  echo "[vm-build] gate0_option_a: done"
fi

# gate2_xproc
if [ ! -f gate2_xproc ] || [ "$(find . -name gate2_xproc.c -newer gate2_xproc)" ]; then
  echo "[vm-build] gate2_xproc..."
  gcc -O2 -Wall gate2_xproc.c -lnuma -lOpenCL -lpthread -o gate2_xproc 2>&1
  echo "[vm-build] gate2_xproc: done"
fi
INNER
  echo "  VM build complete"
else
  echo "  VM binaries up to date — skipping build (use --rebuild-vm to force)"
fi

# ── Derive uprobe offset ──────────────────────────────────────────────────────
echo ""
echo "── Uprobe offset ──"
UPROBE_OFFSET=$(vm_run 'cat /etc/cxl_poc_uprobe_offset 2>/dev/null' | grep -oP '0x[0-9a-f]+' | head -1 || echo "")
if [ -z "$UPROBE_OFFSET" ]; then
  UPROBE_OFFSET=$(vm_run "nm /usr/local/bin/ldpc_decoder_benchmark 2>/dev/null | grep -i 'ldpc_decoder_impl.*decode\b' | awk '{print \"0x\"\$1}' | head -1" || echo "")
fi
[ -z "$UPROBE_OFFSET" ] && { echo "  ERROR: could not determine uprobe offset — rebuild with --rebuild-vm"; exit 1; }
echo "  UPROBE_OFFSET=$UPROBE_OFFSET"

# ── Gate 0: CXL zero-copy ─────────────────────────────────────────────────────
run_gate0() {
  echo ""
  echo "════════════════════════════════════════"
  echo "  GATE 0 — CXL zero-copy topology check"
  echo "════════════════════════════════════════"
  vm_run_script << 'INNER'
set -euo pipefail
cd /root/cxl/cxl_ran_poc/phase5_cxl

[ -f gate0_option_a ] || { echo "ERROR: gate0_option_a not found — run with --rebuild-vm"; exit 1; }

echo "[gate0] running gate0_option_a..."
./gate0_option_a 2>&1
RET=$?
[ $RET -eq 0 ] && echo "[gate0] GATE0 PASS" || echo "[gate0] GATE0 FAIL (exit=$RET)"
INNER
}

# ── Gate 1: uprobe fires ──────────────────────────────────────────────────────
run_gate1() {
  echo ""
  echo "════════════════════════════════════════"
  echo "  GATE 1 — Uprobe fires on srsRAN"
  echo "════════════════════════════════════════"
  vm_run_script << INNER
set -euo pipefail
BENCH=/usr/local/bin/ldpc_decoder_benchmark
OFFSET=${UPROBE_OFFSET}

echo "[gate1] uprobe offset: \$OFFSET"
echo "[gate1] binary: \$BENCH"

# Mount tracefs if not mounted
mount | grep -q tracefs || mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || true

# Clear any stale uprobe
echo "" > /sys/kernel/tracing/uprobe_events 2>/dev/null || true
sleep 0.1

# Register uprobe
echo "p:gate1_check \${BENCH}:\${OFFSET}" > /sys/kernel/tracing/uprobe_events
echo 1 > /sys/kernel/tracing/events/uprobes/gate1_check/enable
echo 1 > /sys/kernel/tracing/tracing_on
echo "" > /sys/kernel/tracing/trace

# Run benchmark (3 iterations, 4 CB variants = 12 events expected)
"\$BENCH" -L 384 -I 5 -T avx2 -R 3 -L 384 2>/dev/null || \
"\$BENCH" -L 384 -I 5 -T avx2 -R 3 2>/dev/null || true

sleep 0.2
HITS=\$(grep -c "gate1_check" /sys/kernel/tracing/trace 2>/dev/null || echo 0)
echo "[gate1] uprobe_hits: \$HITS"

# Cleanup: disable event first, then remove with -: prefix (avoids EBUSY)
echo 0 > /sys/kernel/tracing/tracing_on
echo 0 > /sys/kernel/tracing/events/uprobes/gate1_check/enable 2>/dev/null || true
echo "-:uprobes/gate1_check" >> /sys/kernel/tracing/uprobe_events 2>/dev/null || true

if [ "\$HITS" -gt 0 ]; then
  echo "[gate1] GATE1 PASS (hits=\$HITS at \$OFFSET)"
else
  echo "[gate1] GATE1 FAIL (hits=0 — check offset or tracefs mount)"
  exit 1
fi
INNER
}

# ── Gate 2: full pipeline ─────────────────────────────────────────────────────
run_gate2() {
  echo ""
  echo "════════════════════════════════════════"
  echo "  GATE 2 — Full pipeline: uprobe → CXL → OCL"
  echo "════════════════════════════════════════"
  echo "  (Expected: ~5-10 min — OCL JIT in QEMU-TCG is slow)"
  vm_run_script << INNER
set -euo pipefail
cd /root/cxl/cxl_ran_poc/phase5_cxl

[ -f gate2_xproc ] || { echo "ERROR: gate2_xproc not found — run with --rebuild-vm"; exit 1; }
[ -f /usr/local/bin/ldpc_decoder_benchmark ] || { echo "ERROR: ldpc_decoder_benchmark not found"; exit 1; }

export UPROBE_OFFSET=${UPROBE_OFFSET}
export LDPC_BENCH=/usr/local/bin/ldpc_decoder_benchmark

echo "[gate2] running gate2_xproc..."
echo "[gate2] uprobe offset: \${UPROBE_OFFSET}"

./gate2_xproc 2>&1
RET=\$?

# Collect result
CSV_SRC=\$(ls /root/cxl/cxl_ran_poc/paper/results/droplet/e2e_droplet.csv 2>/dev/null || \
           ls /root/cxl/cxl_ran_poc/paper/results/e2e_droplet.csv 2>/dev/null || echo "")
if [ -n "\$CSV_SRC" ] && [ -f "\$CSV_SRC" ]; then
  # Copy to GCP-specific result file
  mkdir -p /root/cxl/cxl_ran_poc/paper/results/droplet
  cp "\$CSV_SRC" /root/cxl/cxl_ran_poc/paper/results/droplet/e2e_gcp.csv
  echo "[gate2] results written to paper/results/droplet/e2e_gcp.csv"
  cat /root/cxl/cxl_ran_poc/paper/results/droplet/e2e_gcp.csv
fi

exit \$RET
INNER
}

# ── Run selected gates ────────────────────────────────────────────────────────
case "${GATE_ONLY}" in
  0) run_gate0 ;;
  1) run_gate1 ;;
  2) run_gate2 ;;
  "")
    run_gate0
    run_gate1
    run_gate2
    ;;
  *) echo "Unknown gate: $GATE_ONLY. Choose 0, 1, or 2."; exit 1 ;;
esac

# ── Step 6: Collect results back to local ────────────────────────────────────
echo ""
echo "── Collecting results ──"
LOCAL_RESULTS="$(dirname $SCRIPT_DIR)/../cxl_ran_poc/paper/results/droplet/"
mkdir -p "$LOCAL_RESULTS" 2>/dev/null || true

# Pull e2e_gcp.csv from VM → GCP host → local
$GCP_SSH "${GCP_USER}@${GCP_IP}" \
  "scp -i $VM_KEY_PATH -P 2222 -o StrictHostKeyChecking=no \
   root@localhost:/root/cxl/cxl_ran_poc/paper/results/droplet/e2e_gcp.csv \
   /home/karthix25/cxl/cxl_ran_poc/paper/results/droplet/e2e_gcp.csv 2>/dev/null || true"

rsync -az \
  -e "ssh -i $GCP_KEY -o StrictHostKeyChecking=no" \
  "${GCP_USER}@${GCP_IP}:/home/karthix25/cxl/cxl_ran_poc/paper/results/droplet/e2e_gcp.csv" \
  "$LOCAL_RESULTS" 2>/dev/null && \
  echo "  e2e_gcp.csv pulled to $LOCAL_RESULTS" || \
  echo "  (result pull skipped — no e2e_gcp.csv found yet)"

echo ""
echo "════════════════════════════════════════"
echo "  run_e2e_test.sh complete"
echo "  Next: ./teardown.sh when done with the instance"
echo "════════════════════════════════════════"
