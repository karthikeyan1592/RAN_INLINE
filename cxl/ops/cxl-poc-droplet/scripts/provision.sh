#!/bin/bash
# provision.sh — create or reuse the cxl-poc DigitalOcean droplet
# Idempotent: reuses an existing droplet with the same name.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.cxl_droplet_ip"

CXL_DROPLET_NAME="${CXL_DROPLET_NAME:-cxl-poc}"
CXL_DROPLET_REGION="${CXL_DROPLET_REGION:-blr1}"
CXL_DROPLET_SIZE="${CXL_DROPLET_SIZE:-s-4vcpu-8gb}"
CXL_DROPLET_IMAGE="${CXL_DROPLET_IMAGE:-ubuntu-24-04-x64}"
CXL_SSH_KEY_ID="${CXL_SSH_KEY_ID:-$(doctl compute ssh-key list --no-header --format ID | head -1)}"

echo "=== CXL PoC Droplet Provision ==="
echo "  Name:   $CXL_DROPLET_NAME"
echo "  Region: $CXL_DROPLET_REGION  Size: $CXL_DROPLET_SIZE"
echo ""

# ── Idempotent: reuse existing droplet ───────────────────────
EXISTING_IP=$(doctl compute droplet get "$CXL_DROPLET_NAME" \
  --format PublicIPv4 --no-header 2>/dev/null || true)

if [ -n "$EXISTING_IP" ] && [ "$EXISTING_IP" != "nil" ]; then
  echo "Droplet '$CXL_DROPLET_NAME' already exists: $EXISTING_IP"
  echo "$EXISTING_IP" > "$IP_FILE"
else
  echo "Creating droplet '$CXL_DROPLET_NAME'..."
  doctl compute droplet create "$CXL_DROPLET_NAME" \
    --region "$CXL_DROPLET_REGION" \
    --image "$CXL_DROPLET_IMAGE" \
    --size "$CXL_DROPLET_SIZE" \
    --ssh-keys "$CXL_SSH_KEY_ID" \
    --wait
  EXISTING_IP=$(doctl compute droplet get "$CXL_DROPLET_NAME" \
    --format PublicIPv4 --no-header)
  echo "$EXISTING_IP" > "$IP_FILE"
  echo "Created: $EXISTING_IP"
fi

DROPLET_IP="$EXISTING_IP"
echo ""
echo "IP written to $IP_FILE"

# ── Poll SSH until reachable ──────────────────────────────────
echo ""
echo "Waiting for SSH on $DROPLET_IP..."
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5 -o BatchMode=yes"
for i in $(seq 1 30); do
  if ssh $SSH_OPTS root@"$DROPLET_IP" true 2>/dev/null; then
    echo "SSH reachable after $((i*5))s"
    break
  fi
  [ $i -eq 30 ] && { echo "ERROR: SSH not reachable after 150s"; exit 1; }
  sleep 5
done

# ── Sanity checks ─────────────────────────────────────────────
echo ""
echo "=== Sanity checks ==="
KERNEL=$(ssh $SSH_OPTS root@"$DROPLET_IP" 'uname -r')
KVM=$(ssh $SSH_OPTS root@"$DROPLET_IP" '[ -e /dev/kvm ] && echo YES || echo NO')
AVX2=$(ssh $SSH_OPTS root@"$DROPLET_IP" 'grep -qo avx2 /proc/cpuinfo && echo avx2 || echo NO')
echo "  Kernel: $KERNEL"
echo "  KVM:    $KVM"
echo "  AVX2:   $AVX2"

FAIL=0
[ "$KVM" != "YES" ] && { echo "WARNING: /dev/kvm missing — this region/size does not support KVM"; FAIL=1; }
[ "$AVX2" != "avx2" ] && { echo "WARNING: AVX2 missing — srsRAN will SIGILL"; FAIL=1; }
[ $FAIL -eq 1 ] && exit 1

echo ""
echo "Droplet ready: $DROPLET_IP"
echo ""
echo "══════════════════════════════════════════════════"
echo "  Full setup sequence (run in order):"
echo ""
echo "  1. Provision (done):       ./provision.sh"
echo "  2. System packages:        ./install_deps.sh"
echo "  3. rsync source:           rsync -az --exclude '*/build' --exclude '.git' \\"
echo "                               /root/linux_env/cxl/ root@$DROPLET_IP:/root/cxl/"
echo "  4. Build from source:      ./build_tools.sh          (~30-40 min)"
echo "     Optional OAI gNB:       ./build_tools.sh --with-oai  (~+45 min)"
echo "  5. Prepare VM image:       ./prepare_vm.sh           (~10-15 min)"
echo "  6. Boot VM:                ssh root@$DROPLET_IP \\"
echo "                               '/root/cxl/ops/cxl-poc-droplet/scripts/qemu_cxl_launch.sh /tmp/cxl_vm.qcow2 &'"
echo "  7. Verify CXL topology:    ./verify_cxl_checks.sh"
echo "  8. Run v6 pipeline:        (see v6 spec)"
echo "  9. Teardown:               ./teardown.sh"
echo "══════════════════════════════════════════════════"
