#!/bin/bash
# launch_vm.sh — launch QEMU CXL VM on GCP host and bring up CXL NUMA node 1
#
# Run from LOCAL machine OR directly on GCP host (as karthix25).
# Handles: QEMU start → wait for SSH → CXL setup → smoke test
#
# Usage:
#   ./launch_vm.sh           # launch and setup
#   ./launch_vm.sh --kill    # kill existing QEMU first, then launch
#   ./launch_vm.sh --status  # show VM status only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: .gcp_instance_ip missing — run provision.sh first"; exit 1; }
GCP_IP=$(cat "$IP_FILE")
GCP_USER="${GCP_USER:-karthix25}"
GCP_KEY="${GCP_KEY:-$HOME/.ssh/id_ed25519}"
VM_KEY="${VM_KEY:-/home/karthix25/.ssh/vm_key}"  # path ON GCP host

GCP_SSH="ssh -i $GCP_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=10"
VM_SSH_FROM_GCP="ssh -i $VM_KEY -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=5 -o BatchMode=yes root@localhost"

# ── Argument handling ────────────────────────────────────────────────────────
case "${1:-}" in
  --kill)
    echo "Killing existing QEMU..."
    $GCP_SSH $GCP_USER@$GCP_IP 'sudo kill $(pgrep qemu-system 2>/dev/null) 2>/dev/null; sleep 2; echo done'
    ;;
  --status)
    echo "=== GCP host ==="
    $GCP_SSH $GCP_USER@$GCP_IP 'pgrep qemu-system > /dev/null && echo "QEMU: running (PID $(cat /tmp/qemu.pid 2>/dev/null))" || echo "QEMU: stopped"'
    echo "=== VM SSH ==="
    $GCP_SSH $GCP_USER@$GCP_IP "$VM_SSH_FROM_GCP uname -r && numactl --hardware 2>/dev/null | head -5" 2>/dev/null || echo "VM: not reachable"
    exit 0
    ;;
esac

# ── Launch QEMU if not running ───────────────────────────────────────────────
QEMU_RUNNING=$($GCP_SSH $GCP_USER@$GCP_IP 'pgrep qemu-system > /dev/null 2>&1 && echo yes || echo no')

if [ "$QEMU_RUNNING" = "yes" ]; then
  echo "QEMU already running — skip launch (use --kill to restart)"
else
  echo "Launching QEMU CXL VM on $GCP_IP..."
  $GCP_SSH $GCP_USER@$GCP_IP 'bash -s' << 'LAUNCH'
set -e
QEMU=$(command -v /usr/local/bin/qemu-system-x86_64 2>/dev/null || command -v qemu-system-x86_64)
nohup sudo "$QEMU" \
  -enable-kvm \
  -cpu host,-hypervisor \
  -smp 4 \
  -m 4G,slots=8,maxmem=16G \
  -M q35,cxl=on \
  -drive file=/tmp/cxl_vm.qcow2,if=none,id=disk0,format=qcow2 \
  -device virtio-blk-pci,drive=disk0,bus=pcie.0 \
  -drive file=/tmp/cidata.iso,if=none,id=cidata,media=cdrom,format=raw \
  -device virtio-blk-pci,drive=cidata,bus=pcie.0 \
  -object memory-backend-file,id=cxl-mem0,share=on,mem-path=/tmp/cxl_mem.img,size=2G,align=256M,pmem=off \
  -object memory-backend-ram,id=cxl-lsa0,size=256M \
  -device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 \
  -device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 \
  -device cxl-type3,bus=rp0,persistent-memdev=cxl-mem0,lsa=cxl-lsa0,id=cxl-pmem0 \
  -M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,bus=pcie.0 \
  -nographic -serial mon:stdio > /tmp/qemu.log 2>&1 &
sleep 2
pgrep -x qemu-system-x86 > /tmp/qemu.pid || pgrep qemu-system > /tmp/qemu.pid
echo "QEMU PID: $(cat /tmp/qemu.pid)"
LAUNCH
fi

# ── Wait for first VM SSH ────────────────────────────────────────────────────
echo "Waiting for VM SSH on port 2222 (first boot may take 2-3 min)..."
$GCP_SSH $GCP_USER@$GCP_IP \
  "until $VM_SSH_FROM_GCP true 2>/dev/null; do printf '.'; sleep 5; done; echo ' VM up (boot 1)'"

# ── Handle first-boot kernel upgrade ─────────────────────────────────────────
# cloud-init runcmd installs linux-hwe-22.04 (6.8) and reboots. If we land on
# 5.15 it means first boot is in progress. Wait for cloud-init to finish, then
# wait for the VM to come back up on 6.8 before running CXL setup.
echo ""
echo "Checking kernel version..."
VM_KERNEL=$($GCP_SSH $GCP_USER@$GCP_IP "$VM_SSH_FROM_GCP uname -r" 2>/dev/null || echo "unknown")
echo "  VM kernel: $VM_KERNEL"

if echo "$VM_KERNEL" | grep -q "^5\."; then
  echo ""
  echo "  Kernel 5.x detected — waiting for cloud-init HWE upgrade + reboot..."
  echo "  (cloud-init installs linux-hwe-22.04 and reboots to 6.8)"
  echo "  This takes 5-8 min. Watching for reboot..."

  # Wait for VM to go away (reboot starts)
  $GCP_SSH $GCP_USER@$GCP_IP 'bash -s' << WAITREBOOT
VM_SSH="$VM_SSH_FROM_GCP"
# Poll until VM stops responding (reboot in progress)
for i in \$(seq 1 120); do
  \$VM_SSH true 2>/dev/null || { echo "  VM rebooting..."; break; }
  [ \$i -eq 120 ] && echo "  WARNING: VM did not reboot within 10 min — check cloud-init logs"
  sleep 5
done
# Wait for VM to come back
echo "  Waiting for VM to come back (6.8 kernel)..."
for i in \$(seq 1 60); do
  \$VM_SSH true 2>/dev/null && echo "  VM up (boot 2)" && break
  [ \$i -eq 60 ] && { echo "ERROR: VM did not come back within 5 min"; exit 1; }
  printf '.'; sleep 5
done
WAITREBOOT

  VM_KERNEL=$($GCP_SSH $GCP_USER@$GCP_IP "$VM_SSH_FROM_GCP uname -r" 2>/dev/null || echo "unknown")
  echo "  VM kernel after upgrade: $VM_KERNEL"

  if echo "$VM_KERNEL" | grep -q "^5\."; then
    echo "  WARNING: still on 5.x — HWE install may have failed. Check /tmp/qemu.log"
    echo "  Continuing (CXL setup will likely fail without 6.8+)..."
  fi

  # Sync ops scripts into VM so vm_install_ndctl80.sh and vm_cxl_setup.sh are reachable
  echo ""
  echo "Syncing ops scripts into VM..."
  $GCP_SSH $GCP_USER@$GCP_IP "bash -s" << SYNC
# Create target dir inside VM first, then scp into it
ssh -i $VM_KEY -p 2222 -o StrictHostKeyChecking=no -o BatchMode=yes root@localhost \
  "mkdir -p /root/cxl/ops/gcp-cxl-lab/scripts"
scp -P 2222 -i $VM_KEY \
    -o StrictHostKeyChecking=no -o BatchMode=yes \
    /home/${GCP_USER}/cxl/ops/gcp-cxl-lab/scripts/*.sh \
    root@localhost:/root/cxl/ops/gcp-cxl-lab/scripts/
echo "  scripts synced"
SYNC

  # Install ndctl v80 on first 6.8 boot (skipped on subsequent boots by idempotency check)
  echo ""
  echo "Installing ndctl v80 inside VM (first 6.8 boot only)..."
  $GCP_SSH $GCP_USER@$GCP_IP \
    "$VM_SSH_FROM_GCP bash /root/cxl/ops/gcp-cxl-lab/scripts/vm_install_ndctl80.sh"
fi

# ── CXL setup inside VM ──────────────────────────────────────────────────────
echo ""
echo "Running CXL setup inside VM..."
$GCP_SSH $GCP_USER@$GCP_IP \
  "$VM_SSH_FROM_GCP bash /root/cxl/ops/gcp-cxl-lab/scripts/vm_cxl_setup.sh"

echo ""
echo "=== VM ready ==="
echo "  Kernel: $($GCP_SSH $GCP_USER@$GCP_IP "$VM_SSH_FROM_GCP uname -r" 2>/dev/null)"
echo "  To SSH in from GCP host:"
echo "    ssh -i ~/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no root@localhost"
