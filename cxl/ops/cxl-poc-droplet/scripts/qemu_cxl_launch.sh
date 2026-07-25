#!/bin/bash
# qemu_cxl_launch.sh — launch QEMU with the verified CXL+KVM configuration
#
# DO NOT change -cpu host,-hypervisor
# See references/cxl_qemu_kvm_gotchas.md §1-2 for why this flag is required.
# Removing -hypervisor causes "Failed to synchronize CPU cache state"
# at drivers/cxl/core/region.c:131 and cxl create-region returns ENXIO.
# CONFIG_CXL_REGION_INVALIDATION_TEST=y would bypass this without the flag,
# but Ubuntu generic kernels (6.8.0-71+) do NOT set this option (verified 2026-06-14).
#
# FIX (2026-06-14): virtio devices must use explicit bus=pcie.0
#   Using -drive if=virtio routes the device to the CXL bus (pxb-cxl) by default,
#   causing "PCI: Only PCI/PCIe bridges can be plugged into pxb-cxl". Fixed by
#   using if=none + separate -device virtio-blk-pci,bus=pcie.0.
#
# FIX (2026-06-14): removed cxl-fmw.0.interleave-ways=1
#   QEMU 8.2 does not support the interleave-ways parameter and returns
#   "Parameter 'cxl-fmw.0.interleave-ways' is unexpected". Removed.
#
# Usage: qemu_cxl_launch.sh <disk.qcow2> [extra qemu args...]
set -euo pipefail

DISK="${1:-}"
shift || true

if [ -z "$DISK" ]; then
  echo "Usage: $0 <disk.qcow2> [extra qemu args...]"
  echo "  Download a base image: wget -nc https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img -O /tmp/test.qcow2"
  echo "  Resize:                qemu-img resize /tmp/test.qcow2 10G"
  exit 1
fi

[ -f "$DISK" ] || { echo "ERROR: disk image not found: $DISK"; exit 1; }

CXL_MEM=/tmp/cxl_mem.img
if [ ! -f "$CXL_MEM" ]; then
  echo "Creating CXL backing file: $CXL_MEM (2G)"
  truncate -s 2G "$CXL_MEM"
fi

echo "=== Launching QEMU CXL VM ==="
echo "  Disk:    $DISK"
echo "  CXL mem: $CXL_MEM"
echo "  SSH fwd: localhost:2222 -> VM:22"
echo ""
echo "Press Ctrl-A X to exit QEMU console"
echo ""

exec qemu-system-x86_64 \
  -enable-kvm \
  -cpu host,-hypervisor \
  -smp 4 \
  -m 4G,slots=8,maxmem=16G \
  -M q35,cxl=on \
  -drive file="$DISK",if=none,id=disk0,format=qcow2 \
  -device virtio-blk-pci,drive=disk0,bus=pcie.0 \
  -object memory-backend-file,id=cxl-mem0,share=on,mem-path="$CXL_MEM",size=2G,align=256M,pmem=off \
  -object memory-backend-ram,id=cxl-lsa0,size=256M \
  -device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 \
  -device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 \
  -device cxl-type3,bus=rp0,persistent-memdev=cxl-mem0,lsa=cxl-lsa0,id=cxl-pmem0 \
  -M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,bus=pcie.0 \
  -nographic -serial mon:stdio \
  "$@"
