#!/bin/bash
# checkpoint.sh — shut down the droplet and take a snapshot
set -euo pipefail

CXL_DROPLET_NAME="${CXL_DROPLET_NAME:-cxl-poc}"
SNAP_NAME="cxl-poc-checkpoint-$(date +%Y%m%d-%H%M)"

echo "=== Checkpoint: $CXL_DROPLET_NAME ==="
echo ""
echo "Shutting down droplet..."
doctl compute droplet-action shutdown "$CXL_DROPLET_NAME" --wait

echo ""
echo "Taking snapshot: $SNAP_NAME"
doctl compute droplet-action snapshot "$CXL_DROPLET_NAME" \
  --snapshot-name "$SNAP_NAME" --wait

echo ""
echo "=== Available snapshots ==="
doctl compute snapshot list --resource droplet
