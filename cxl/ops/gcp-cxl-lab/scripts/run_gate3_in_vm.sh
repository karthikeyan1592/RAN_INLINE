#!/usr/bin/env bash
# run_gate3_in_vm.sh — SSH into VM and execute Gate 3/4 end-to-end
#
# Run from the GCP HOST (not inside VM directly).
# Assumes:
#   - QEMU VM running on port 2222
#   - VM key at ~/.ssh/vm_key
#   - Source already rsynced to /root/cxl/ inside VM
#
# Usage: bash run_gate3_in_vm.sh [--skip-build]
set -euo pipefail

SKIP_BUILD=0
[[ "${1:-}" == "--skip-build" ]] && SKIP_BUILD=1

VM_SSH="ssh -i ~/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no root@localhost"
GATE_DIR=/root/cxl/cxl_ran_poc/phase5_cxl

PHASE5_SRC=/home/$(whoami)/cxl/cxl_ran_poc/phase5_cxl

echo "=== Syncing gate3 source files to VM ==="
scp -i ~/.ssh/vm_key -P 2222 -o StrictHostKeyChecking=no \
  "$PHASE5_SRC/llr_mover.bpf.c" \
  "$PHASE5_SRC/llr_gate3.c" \
  "$PHASE5_SRC/run_gate3_v7.sh" \
  "$PHASE5_SRC/Makefile" \
  root@localhost:$GATE_DIR/

echo "=== Ensuring vmlinux.h exists in VM ==="
$VM_SSH "
  if [[ ! -f $GATE_DIR/vmlinux.h ]]; then
    echo 'Generating vmlinux.h from BTF...'
    bpftool btf dump file /sys/kernel/btf/vmlinux format c > $GATE_DIR/vmlinux.h
  else
    echo 'vmlinux.h already present'
    wc -l $GATE_DIR/vmlinux.h
  fi
"

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  echo "=== Building gate3 inside VM ==="
  $VM_SSH "
    cd $GATE_DIR
    BPFTIME=/root/cxl/third_party/bpftime
    make gate3 \
      BPFTIME_V7=\$BPFTIME \
      LIBBPF_V7_A=\$BPFTIME/build/libbpf/libbpf/libbpf.a \
      LIBBPF_V7_INC=\$BPFTIME/build/libbpf 2>&1
    echo 'Build complete'
    ls -la llr_mover.bpf.o llr_gate3
  "
fi

echo ""
echo "=== Running Gate 3 v7 inside VM ==="
$VM_SSH "
  cd $GATE_DIR
  chmod +x run_gate3_v7.sh
  bash run_gate3_v7.sh 2>&1
" | tee /tmp/gate3_v7_run.log

echo ""
echo "=== Pulling e2e_gcp.csv back to local ==="
scp -i ~/.ssh/vm_key -P 2222 -o StrictHostKeyChecking=no \
  root@localhost:/root/cxl/paper/results/e2e_gcp.csv \
  /home/$(whoami)/cxl/paper/results/e2e_gcp.csv 2>/dev/null && \
  echo "e2e_gcp.csv synced" || echo "WARNING: e2e_gcp.csv not found in VM"

echo ""
echo "Full log: /tmp/gate3_v7_run.log"
echo "Run 'cat /tmp/gate3_v7_run.log' and paste into memory/v7_run/implementer/gate_3.md"
