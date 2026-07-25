#!/bin/bash
# provision.sh — create GCP instance for CXL simulation lab
# Replaces ops/cxl-poc-droplet/scripts/provision.sh (DigitalOcean version)
#
# Prerequisites:
#   gcloud auth login && gcloud config set project cxl-systems-lab-26
#   Local SSH key at ~/.ssh/id_ed25519 added to instance (one-time, done manually)
#
# Usage: ./provision.sh
set -euo pipefail

PROJECT="cxl-systems-lab-26"
ZONE="asia-south2-a"
INSTANCE="cxl-systems-lab"
MACHINE="n2-standard-4"
IMAGE_FAMILY="ubuntu-2404-lts-amd64"
IMAGE_PROJECT="ubuntu-os-cloud"
DISK_SIZE="40GB"
DISK_TYPE="pd-balanced"

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
GCP_USER="${GCP_USER:-karthix25}"

echo "=== CXL Lab GCP Provision ==="
echo "  Project:  $PROJECT"
echo "  Zone:     $ZONE"
echo "  Instance: $INSTANCE"
echo "  Machine:  $MACHINE"
echo ""

gcloud config set project "$PROJECT" --quiet

# ── Idempotent: reuse existing instance ───────────────────────
EXISTING_IP=$(gcloud compute instances describe "$INSTANCE" \
  --zone="$ZONE" --format="get(networkInterfaces[0].accessConfigs[0].natIP)" 2>/dev/null || true)

if [ -n "$EXISTING_IP" ]; then
  echo "Instance '$INSTANCE' already exists: $EXISTING_IP"
else
  echo "Creating instance '$INSTANCE'..."
  gcloud compute instances create "$INSTANCE" \
    --zone="$ZONE" \
    --machine-type="$MACHINE" \
    --image-family="$IMAGE_FAMILY" \
    --image-project="$IMAGE_PROJECT" \
    --boot-disk-size="$DISK_SIZE" \
    --boot-disk-type="$DISK_TYPE" \
    --enable-nested-virtualization \
    --project="$PROJECT"

  EXISTING_IP=$(gcloud compute instances describe "$INSTANCE" \
    --zone="$ZONE" --format="get(networkInterfaces[0].accessConfigs[0].natIP)")
  echo "Created: $EXISTING_IP"

  echo ""
  echo "Adding SSH key for automated access..."
  gcloud compute ssh "$INSTANCE" --zone="$ZONE" --command="
    mkdir -p /home/${GCP_USER}/.ssh
    echo '$(cat ${SSH_KEY}.pub)' >> /home/${GCP_USER}/.ssh/authorized_keys
    chmod 600 /home/${GCP_USER}/.ssh/authorized_keys
  "
fi

echo "$EXISTING_IP" > "$IP_FILE"
echo ""
echo "IP written to $IP_FILE"

# ── Poll SSH until reachable ──────────────────────────────────
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=5 -o BatchMode=yes"
echo "Waiting for SSH on $EXISTING_IP..."
for i in $(seq 1 30); do
  if ssh $SSH_OPTS "${GCP_USER}@${EXISTING_IP}" true 2>/dev/null; then
    echo "SSH reachable"
    break
  fi
  [ $i -eq 30 ] && { echo "ERROR: SSH not reachable after 150s"; exit 1; }
  sleep 5
done

# ── Sanity checks ─────────────────────────────────────────────
echo ""
echo "=== Sanity checks ==="
VMX=$(ssh $SSH_OPTS "${GCP_USER}@${EXISTING_IP}" 'grep -c vmx /proc/cpuinfo')
KVM=$(ssh $SSH_OPTS "${GCP_USER}@${EXISTING_IP}" 'ls /dev/kvm 2>/dev/null && echo YES || echo NO')
KERNEL=$(ssh $SSH_OPTS "${GCP_USER}@${EXISTING_IP}" 'uname -r')
echo "  Kernel:      $KERNEL"
echo "  VMX count:   $VMX  (must be > 0 for nested KVM)"
echo "  /dev/kvm:    $KVM"

[ "$VMX" -gt 0 ] || { echo "ERROR: vmx not exposed — nested KVM unavailable"; exit 1; }

echo ""
echo "Instance ready: $EXISTING_IP"
echo ""
echo "══════════════════════════════════════════════════"
echo "  Full setup sequence (run in order):"
echo ""
echo "  1. Provision (done):    ./provision.sh"
echo "  2. Install packages:    ./install_deps.sh"
echo "  3. rsync source:        ./rsync_source.sh"
echo "  4. Build from source:   ./build_tools.sh          (~30-40 min)"
echo "  5. Prepare VM image:    ./prepare_vm.sh           (~10-15 min)"
echo "  6. Boot VM:             ./launch_vm.sh"
echo "  7. Verify CXL topology: ./verify_cxl.sh"
echo "  8. Run v6 pipeline:     (see v6 spec)"
echo "  9. Teardown:            ./teardown.sh"
echo "══════════════════════════════════════════════════"
