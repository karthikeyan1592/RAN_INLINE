#!/bin/bash
# restore.sh — recreate the droplet from the most recent checkpoint snapshot
# Usage: restore.sh [--yes]
set -euo pipefail

CXL_DROPLET_NAME="${CXL_DROPLET_NAME:-cxl-poc}"
CXL_DROPLET_REGION="${CXL_DROPLET_REGION:-blr1}"
CXL_DROPLET_SIZE="${CXL_DROPLET_SIZE:-s-4vcpu-8gb}"

RUN=0
[ "${1:-}" = "--yes" ] && RUN=1

echo "=== Available cxl-poc-checkpoint snapshots ==="
doctl compute snapshot list --resource droplet

SNAP_ID=$(doctl compute snapshot list --resource droplet \
  --no-header --format ID,Name \
  | grep "cxl-poc-checkpoint" \
  | sort -k2 -r | head -1 | awk '{print $1}')

[ -z "$SNAP_ID" ] && { echo "ERROR: no cxl-poc-checkpoint snapshots found"; exit 1; }
SNAP_NAME=$(doctl compute snapshot list --resource droplet \
  --no-header --format ID,Name | grep "$SNAP_ID" | awk '{print $2}')

SSH_KEY_ID=$(doctl compute ssh-key list --no-header --format ID | head -1)

echo ""
echo "Most recent snapshot: $SNAP_NAME (ID: $SNAP_ID)"
echo ""
echo "Command to recreate droplet:"
echo "  doctl compute droplet create $CXL_DROPLET_NAME \\"
echo "    --region $CXL_DROPLET_REGION \\"
echo "    --size $CXL_DROPLET_SIZE \\"
echo "    --image $SNAP_ID \\"
echo "    --ssh-keys $SSH_KEY_ID \\"
echo "    --wait"
echo ""

if [ $RUN -eq 0 ]; then
  echo "Pass --yes to actually run this command."
  exit 0
fi

echo "Restoring from snapshot $SNAP_ID..."
doctl compute droplet create "$CXL_DROPLET_NAME" \
  --region "$CXL_DROPLET_REGION" \
  --size "$CXL_DROPLET_SIZE" \
  --image "$SNAP_ID" \
  --ssh-keys "$SSH_KEY_ID" \
  --wait

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NEW_IP=$(doctl compute droplet get "$CXL_DROPLET_NAME" \
  --format PublicIPv4 --no-header)
echo "$NEW_IP" > "$SCRIPT_DIR/.cxl_droplet_ip"
echo "Restored: $NEW_IP"
