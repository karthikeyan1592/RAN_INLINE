#!/bin/bash
# rsync_source.sh — sync local CXL source tree to GCP instance
# Run after: install_deps.sh
# Run before: build_tools.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first (.gcp_instance_ip missing)"; exit 1; }
INSTANCE_IP=$(cat "$IP_FILE")

SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
GCP_USER="${GCP_USER:-karthix25}"
SOURCE_DIR="${SOURCE_DIR:-/root/linux_env/cxl/}"
REMOTE_DIR="/home/${GCP_USER}/cxl/"

echo "=== rsync source to $INSTANCE_IP ==="
echo "  Source: $SOURCE_DIR"
echo "  Dest:   ${GCP_USER}@${INSTANCE_IP}:${REMOTE_DIR}"
echo ""

rsync -az --progress \
  --exclude 'build/' \
  --exclude 'build_*/' \
  --exclude '.git' \
  --exclude '__pycache__' \
  --exclude '*.o' \
  --exclude '*.a' \
  --exclude '*.so' \
  -e "ssh -i $SSH_KEY -o StrictHostKeyChecking=no" \
  "$SOURCE_DIR" \
  "${GCP_USER}@${INSTANCE_IP}:${REMOTE_DIR}" \
  2>&1 | tail -5

echo ""
echo "rsync complete — next: ./build_tools.sh"
