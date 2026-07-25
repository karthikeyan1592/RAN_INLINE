#!/bin/bash
# teardown.sh — delete the cxl-poc droplet
# Tip: run checkpoint.sh first to snapshot before deleting.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.cxl_droplet_ip"
CXL_DROPLET_NAME="${CXL_DROPLET_NAME:-cxl-poc}"

echo "=== Teardown: $CXL_DROPLET_NAME ==="
echo ""
echo "WARNING: this permanently deletes the droplet."
echo "Run checkpoint.sh first if you want to preserve state."
echo ""
read -rp "Type the droplet name to confirm: " CONFIRM
[ "$CONFIRM" != "$CXL_DROPLET_NAME" ] && { echo "Aborted."; exit 1; }

doctl compute droplet delete "$CXL_DROPLET_NAME" --force
rm -f "$IP_FILE"

echo ""
echo "=== Remaining droplets ==="
doctl compute droplet list --format Name,PublicIPv4,Status
