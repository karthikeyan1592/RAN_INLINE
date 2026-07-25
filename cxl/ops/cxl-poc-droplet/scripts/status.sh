#!/bin/bash
# status.sh — show current droplet and snapshot state
set -euo pipefail

CXL_DROPLET_NAME="${CXL_DROPLET_NAME:-cxl-poc}"

echo "=== Droplets ==="
doctl compute droplet list --format Name,PublicIPv4,Status,Memory,Disk

echo ""
echo "=== Snapshots (droplet) ==="
doctl compute snapshot list --resource droplet

echo ""
# Cost reminder if our droplet is active
STATUS=$(doctl compute droplet list --no-header --format Name,Status \
  | grep "^$CXL_DROPLET_NAME " | awk '{print $2}' || true)
if [ "$STATUS" = "active" ]; then
  echo "Cost reminder: s-4vcpu-8gb = ~\$0.071/hr (~\$48/mo) while active."
  echo "Run checkpoint.sh + teardown.sh when done for the session."
fi
