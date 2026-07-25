#!/bin/bash
# delete_snapshot.sh — delete a droplet snapshot by ID or interactive selection
# Usage: delete_snapshot.sh [snapshot-id]
set -euo pipefail

echo "=== CXL PoC Droplet Snapshots ==="
doctl compute snapshot list --resource droplet
echo ""

SNAP_ID="${1:-}"

if [ -z "$SNAP_ID" ]; then
  read -rp "Enter snapshot ID to delete: " SNAP_ID
fi

[ -z "$SNAP_ID" ] && { echo "No ID provided. Aborted."; exit 1; }

echo "Deleting snapshot $SNAP_ID..."
doctl compute snapshot delete "$SNAP_ID" --force
echo "Deleted."
echo ""
echo "=== Remaining snapshots ==="
doctl compute snapshot list --resource droplet
