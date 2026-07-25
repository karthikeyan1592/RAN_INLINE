#!/bin/bash
# prepare_vm.sh — prepare QEMU CXL VM image on GCP instance
#
# What this does (all runs on GCP host via SSH):
#   1. Download Ubuntu 22.04 (jammy) cloud image  → /tmp/cxl_vm.qcow2
#   2. Resize to 15 GiB
#   3. Generate VM SSH keypair (vm_key) on GCP host
#   4. Create cloud-init user-data / meta-data ISO (injects vm_key + HWE kernel install)
#   5. Create 2 GiB CXL backing file  → /tmp/cxl_mem.img
#
# After this runs: boot with launch_vm.sh (which handles first-boot kernel upgrade,
# ndctl v80 install, and CXL topology setup automatically).
#
# Why Ubuntu 22.04 (jammy) not 24.04:
#   Jammy's 5.15 kernel + linux-hwe-22.04 gives 6.8 with full CXL module support.
#   Noble ships 6.8 directly but its ndctl (80.x) packaging differs from the
#   source-built path verified against the DO droplet run.
#
# Idempotent: skips steps that already completed (checks /tmp paths).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first (.gcp_instance_ip missing)"; exit 1; }
GCP_IP=$(cat "$IP_FILE")
GCP_USER="${GCP_USER:-karthix25}"
GCP_KEY="${GCP_KEY:-$HOME/.ssh/id_ed25519}"
SSH_OPTS="-i $GCP_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=30 -o ServerAliveInterval=60"

echo "=== CXL Lab: Prepare QEMU VM Image ==="
echo "  GCP host: $GCP_IP"
echo "  Expected time: 3-6 min (image download dominates)"
echo ""

# ── Step 1: Download Ubuntu 22.04 cloud image ─────────────────────────────
echo "── Step 1: Ubuntu 22.04 cloud image ──"
ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" 'bash -s' << 'REMOTE'
set -euo pipefail
DISK=/tmp/cxl_vm.qcow2
IMAGE_URL="https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-amd64.img"

if [ -f "$DISK" ]; then
  echo "  $DISK already exists ($(du -sh $DISK | cut -f1)) — skipping download"
else
  echo "  Downloading from $IMAGE_URL ..."
  wget -q --show-progress -O "$DISK" "$IMAGE_URL" 2>&1 | tail -3
  echo "  Downloaded: $(du -sh $DISK | cut -f1)"
fi
REMOTE

# ── Step 2: Resize to 15 GiB ──────────────────────────────────────────────
echo ""
echo "── Step 2: Resize to 15 GiB ──"
ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" 'bash -s' << 'REMOTE'
set -euo pipefail
DISK=/tmp/cxl_vm.qcow2
CURRENT_SIZE=$(qemu-img info "$DISK" | awk '/virtual size/{print $3}')
echo "  current: $CURRENT_SIZE"
qemu-img resize "$DISK" 15G > /dev/null
echo "  resized: $(qemu-img info $DISK | awk '/virtual size/{print $3}')"
REMOTE

# ── Step 3: VM SSH keypair ─────────────────────────────────────────────────
echo ""
echo "── Step 3: VM SSH keypair ──"
ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" 'bash -s' << 'REMOTE'
set -euo pipefail
VM_KEY=/home/karthix25/.ssh/vm_key
if [ -f "$VM_KEY" ]; then
  echo "  $VM_KEY already exists — reusing"
else
  mkdir -p /home/karthix25/.ssh
  ssh-keygen -t ed25519 -f "$VM_KEY" -N "" -q
  echo "  generated: $VM_KEY"
fi
echo "  pub: $(cat ${VM_KEY}.pub | cut -c1-72)..."
REMOTE

# ── Step 4: Cloud-init seed ISO ───────────────────────────────────────────
echo ""
echo "── Step 4: Cloud-init seed ISO ──"
VM_PUBKEY=$(ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" 'cat /home/karthix25/.ssh/vm_key.pub')

ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" "bash -s" << REMOTE_EOF
set -euo pipefail

if [ -f /tmp/cidata.iso ]; then
  echo "  /tmp/cidata.iso already exists — skipping cloud-init generation"
  exit 0
fi

mkdir -p /tmp/cidata

# user-data: inject vm_key, root login, HWE kernel install on first boot
python3 - << 'PYEOF'
import os

vm_key = "${VM_PUBKEY}"
user_data = f"""#cloud-config
disable_root: false
ssh_pwauth: false
users:
  - name: root
    ssh_authorized_keys:
      - {vm_key}
    lock_passwd: false
chpasswd:
  expire: false
  list: |
    root:cxlroot
runcmd:
  - systemctl enable ssh
  - systemctl start ssh
  - export DEBIAN_FRONTEND=noninteractive
  - apt-get update -qq
  - apt-get install -y -qq linux-image-generic-hwe-22.04 linux-headers-generic-hwe-22.04 linux-modules-extra-generic-hwe-22.04
  - update-grub
  - reboot
"""

meta_data = """instance-id: cxl-lab-vm-v7
local-hostname: cxl-vm
"""

with open("/tmp/cidata/user-data", "w") as f:
    f.write(user_data)
with open("/tmp/cidata/meta-data", "w") as f:
    f.write(meta_data)
print("  cloud-init files written")
PYEOF

cloud-localds /tmp/cidata.iso /tmp/cidata/user-data /tmp/cidata/meta-data
echo "  /tmp/cidata.iso created ($(du -sh /tmp/cidata.iso | cut -f1))"
REMOTE_EOF

# ── Step 5: CXL backing file ──────────────────────────────────────────────
echo ""
echo "── Step 5: CXL backing file ──"
ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" 'bash -s' << 'REMOTE'
set -euo pipefail
CXL_MEM=/tmp/cxl_mem.img
if [ -f "$CXL_MEM" ]; then
  echo "  $CXL_MEM already exists ($(du -sh $CXL_MEM | cut -f1))"
else
  truncate -s 2G "$CXL_MEM"
  echo "  created: $CXL_MEM (2 GiB)"
fi
REMOTE

# ── Summary ───────────────────────────────────────────────────────────────
echo ""
echo "── Summary ──"
ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" 'bash -s' << 'REMOTE'
ok()  { printf "  [OK]  %-28s %s\n" "$1" "$2"; }
fail(){ printf "  [!!]  %-28s MISSING\n" "$1"; }

[ -f /tmp/cxl_vm.qcow2 ]           && ok "VM disk image"    "$(du -sh /tmp/cxl_vm.qcow2  | cut -f1)" || fail "VM disk image"
[ -f /tmp/cxl_mem.img ]             && ok "CXL backing file" "$(du -sh /tmp/cxl_mem.img   | cut -f1)" || fail "CXL backing file"
[ -f /tmp/cidata.iso ]              && ok "cloud-init ISO"   "$(du -sh /tmp/cidata.iso    | cut -f1)" || fail "cloud-init ISO"
[ -f /home/karthix25/.ssh/vm_key ]  && ok "VM SSH key"       "/home/karthix25/.ssh/vm_key"            || fail "VM SSH key"
REMOTE

echo ""
echo "prepare_vm.sh complete"
echo ""
echo "Next:"
echo "  ./launch_vm.sh         # boot VM, wait for SSH, upgrade kernel 5.15→6.8,"
echo "                         # install ndctl v80, set up CXL topology"
echo "  ./run_e2e_test.sh      # run gate0/1/2 and collect results"
