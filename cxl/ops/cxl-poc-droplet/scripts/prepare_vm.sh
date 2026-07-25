#!/bin/bash
# prepare_vm.sh — download and configure the QEMU CXL VM image on the droplet
#
# What this does (all on the droplet host):
#   1. Download Ubuntu Noble cloud image → /tmp/cxl_vm.qcow2
#   2. Resize to 20 GiB
#   3. Generate VM SSH keypair (droplet's own key, NOT the user's incoming key)
#   4. Inject SSH key + enable root login via virt-customize (offline, no boot needed)
#   5. Create 2 GiB CXL backing file → /tmp/cxl_mem.img
#   6. Install CXL-related packages INSIDE the image offline (via virt-customize)
#
# After this runs: boot with qemu_cxl_launch.sh, run verify_cxl_checks.sh
# Expected runtime: ~10-15 min (image download + virt-customize)
#
# v5 gotchas baked in:
#   - VM SSH key is the DROPLET's own ed25519 key (not the user's authorized_keys)
#   - PermitRootLogin yes patched in sshd_config offline
#   - linux-modules-extra installed offline (CXL modules are in modules-extra)
#   - daxctl installed separately (not bundled with ndctl in cloud image)
#   - memory blocks need re-onlining on 2nd+ boot (rc.local handles this)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.cxl_droplet_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first"; exit 1; }
DROPLET_IP=$(cat "$IP_FILE")
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=30 -o ServerAliveInterval=60"

echo "=== Preparing QEMU CXL VM image on $DROPLET_IP ==="
echo "    Expected: ~10-15 minutes"
echo ""

# ── Step 1-3: Download, resize, generate keypair ─────────────
ssh $SSH_OPTS root@"$DROPLET_IP" 'bash -s' << 'SETUP'
set -euo pipefail

IMAGE_URL="https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img"
DISK=/tmp/cxl_vm.qcow2
CXL_BACKING=/tmp/cxl_mem.img
VM_KEY=/root/.ssh/vm_id_ed25519

echo "── Step 1: Download Ubuntu Noble cloud image ──"
if [ -f "$DISK" ]; then
  echo "  $DISK already exists ($(du -sh $DISK | cut -f1)), skipping download"
else
  echo "  Downloading from $IMAGE_URL..."
  wget -q --show-progress -O "$DISK" "$IMAGE_URL"
fi

echo ""
echo "── Step 2: Resize to 20 GiB ──"
CURRENT=$(qemu-img info "$DISK" | grep "virtual size" | awk '{print $3}')
echo "  current size: $CURRENT"
qemu-img resize "$DISK" 20G
echo "  resized to: $(qemu-img info $DISK | grep 'virtual size' | awk '{print $3}')"

echo ""
echo "── Step 3: VM SSH keypair ──"
if [ -f "$VM_KEY" ]; then
  echo "  $VM_KEY already exists, reusing"
else
  ssh-keygen -t ed25519 -f "$VM_KEY" -N "" -q
  echo "  generated: $VM_KEY"
fi
echo "  pub: $(cat ${VM_KEY}.pub | cut -c1-72)..."
SETUP

# ── Step 4: virt-customize — inject key + sshd + modules ─────
echo ""
echo "── Step 4: virt-customize (SSH key + sshd_config + packages) ──"
echo "    This takes ~5-8 minutes..."

VM_PUBKEY=$(ssh $SSH_OPTS root@"$DROPLET_IP" 'cat /root/.ssh/vm_id_ed25519.pub')

ssh $SSH_OPTS root@"$DROPLET_IP" "bash -s" << VIRT
set -euo pipefail
DISK=/tmp/cxl_vm.qcow2

# SUPERMIN_KERNEL_VERSION avoids virt-customize scanning all kernels (speeds up)
export SUPERMIN_KERNEL_VERSION=\$(uname -r)
export LIBGUESTFS_BACKEND=direct

echo "  injecting SSH key, sshd config, and first-boot setup script..."
# virt-customize has no network inside the appliance, so package install
# is deferred to first-boot rc.local which runs with full VM networking.
virt-customize -a "\$DISK" \
  --mkdir /root/.ssh \
  --write "/root/.ssh/authorized_keys:${VM_PUBKEY}" \
  --chmod "0700:/root/.ssh" \
  --chmod "0600:/root/.ssh/authorized_keys" \
  --run-command "sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config" \
  --run-command "sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config" \
  --write '/etc/rc.local:#!/bin/bash
# First-boot: install CXL + OpenCL packages (needs VM network, not offline)
if [ ! -f /etc/cxl_setup_done ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq \
    ndctl daxctl numactl libnuma-dev \
    gcc g++ make libelf-dev zlib1g-dev \
    ocl-icd-opencl-dev pocl-opencl-icd \
    iproute2 pciutils
  # CXL kernel modules are in linux-modules-extra; try versioned then generic
  KVER=\$(uname -r)
  apt-get install -y -qq "linux-modules-extra-\${KVER}" 2>/dev/null || \
    apt-get install -y -qq linux-modules-extra-generic 2>/dev/null || true
  touch /etc/cxl_setup_done
fi
# Online CXL memory blocks (needed on 2nd+ boot after system-ram mode set)
for f in /sys/devices/system/memory/memory*/state; do
  [ -f "\$f" ] && echo online > "\$f" 2>/dev/null || true
done
exit 0
' \
  --chmod "0755:/etc/rc.local" \
  --run-command "systemctl enable rc-local 2>/dev/null || true" \
  2>&1 | grep -v "^\[" | tail -10

echo "  virt-customize complete (packages deferred to first-boot rc.local)"
VIRT

# ── Step 5: CXL backing file ─────────────────────────────────
echo ""
echo "── Step 5: CXL backing file ──"
ssh $SSH_OPTS root@"$DROPLET_IP" 'bash -s' << 'BACKING'
CXL_BACKING=/tmp/cxl_mem.img
if [ -f "$CXL_BACKING" ]; then
  echo "  $CXL_BACKING already exists ($(du -sh $CXL_BACKING | cut -f1))"
else
  truncate -s 2G "$CXL_BACKING"
  echo "  created: $CXL_BACKING (2 GiB)"
fi
BACKING

# ── Final summary ─────────────────────────────────────────────
echo ""
echo "── Summary ──"
ssh $SSH_OPTS root@"$DROPLET_IP" 'bash -s' << 'SUMMARY'
echo "  VM disk:        $(ls -lh /tmp/cxl_vm.qcow2 | awk '{print $5, $9}')"
echo "  CXL backing:    $(ls -lh /tmp/cxl_mem.img | awk '{print $5, $9}')"
echo "  VM SSH key:     /root/.ssh/vm_id_ed25519"
echo "  VM SSH pub:     $(cat /root/.ssh/vm_id_ed25519.pub | cut -c1-60)..."
SUMMARY

echo ""
echo "prepare_vm.sh complete"
echo ""
echo "Next steps:"
echo "  1. Boot VM:       ssh root@$DROPLET_IP './cxl/ops/cxl-poc-droplet/scripts/qemu_cxl_launch.sh /tmp/cxl_vm.qcow2'"
echo "  2. Verify CXL:    ssh root@$DROPLET_IP './cxl/ops/cxl-poc-droplet/scripts/verify_cxl_checks.sh'"
