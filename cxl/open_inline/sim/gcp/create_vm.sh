#!/usr/bin/env bash
# Open Inline SIM tier — create/stop/delete the GCP KVM VM (ARCHITECTURE_v3_SIM.md §1).
# Prereqs (one-time, interactive): install gcloud SDK + `gcloud auth login`
#   + `gcloud config set project cxl-systems-lab-26`
# Usage: ./create_vm.sh [create|start|stop|delete|ssh]
set -euo pipefail

PROJECT="${OI_PROJECT:-cxl-systems-lab-26}"
ZONE="${OI_ZONE:-asia-south2-a}"
NAME="${OI_VM:-openinline-sim}"
MACHINE="${OI_MACHINE:-n2-standard-16}"
IMAGE_FAMILY="ubuntu-2404-lts-amd64"
IMAGE_PROJECT="ubuntu-os-cloud"
DISK_GB=100

cmd="${1:-create}"
case "$cmd" in
  create)
    gcloud compute instances create "$NAME" \
      --project="$PROJECT" --zone="$ZONE" \
      --machine-type="$MACHINE" \
      --image-family="$IMAGE_FAMILY" --image-project="$IMAGE_PROJECT" \
      --boot-disk-size="${DISK_GB}GB" --boot-disk-type=pd-balanced \
      --metadata=enable-oslogin=TRUE
    echo ">> Created. Next: ./create_vm.sh ssh  then run setup_sim.sh on the VM."
    echo ">> REMEMBER: ./create_vm.sh stop when idle (~\$0.8/hr while running)."
    ;;
  start|stop|delete)
    gcloud compute instances "$cmd" "$NAME" --project="$PROJECT" --zone="$ZONE"
    ;;
  ssh)
    gcloud compute ssh "$NAME" --project="$PROJECT" --zone="$ZONE"
    ;;
  *)
    echo "usage: $0 [create|start|stop|delete|ssh]" >&2; exit 1
    ;;
esac
