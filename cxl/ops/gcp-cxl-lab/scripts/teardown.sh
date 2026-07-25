#!/bin/bash
# teardown.sh — delete GCP CXL lab instance
# DESTRUCTIVE — deletes the instance and its disk.
set -euo pipefail

PROJECT="cxl-systems-lab-26"
ZONE="asia-south2-a"
INSTANCE="cxl-systems-lab"

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"

echo "=== GCP CXL Lab Teardown ==="
echo "  Project:  $PROJECT"
echo "  Zone:     $ZONE"
echo "  Instance: $INSTANCE"
echo ""

read -rp "Type the instance name to confirm deletion: " CONFIRM
[ "$CONFIRM" = "$INSTANCE" ] || { echo "Aborted."; exit 1; }

gcloud compute instances delete "$INSTANCE" \
  --zone="$ZONE" \
  --project="$PROJECT" \
  --quiet

rm -f "$IP_FILE"
echo ""
echo "Remaining instances:"
gcloud compute instances list --project="$PROJECT" --format="table(name,zone,machineType,status,networkInterfaces[0].accessConfigs[0].natIP)"
