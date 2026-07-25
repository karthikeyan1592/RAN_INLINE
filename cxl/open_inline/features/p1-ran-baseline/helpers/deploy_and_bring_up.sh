#!/usr/bin/env bash
# deploy_and_bring_up.sh — runs LOCALLY (this dev box). Pushes this repo's open_inline/ tree to a
# freshly-provisioned remote host over SSH, provisions that host (Docker/Compose/tcpdump/OCUDU
# checkout), and runs the full P1-G1+P1-G2 bring-up there, streaming output back and pulling the
# resulting pcap corpus + manifest into this repo's artifacts/ directory.
#
# Why rsync, not git clone: this project's git repo (/root/linux_env/cxl) has no remote configured
# (checked: `git remote -v` is empty) -- it has never been pushed anywhere. rsync of the working
# tree is the direct, no-new-infrastructure path; third_party/ocudu (which DOES have a real remote,
# gitlab.com/ocudu/ocudu) is cloned fresh on the remote host instead of transferred, both because
# it's faster (shallow clone at the pinned tag vs. copying a full checkout) and because a fresh
# clone is a stronger correctness guarantee than a copy.
#
# Usage: deploy_and_bring_up.sh <user>@<host> [--soak-seconds 600] [--identity ~/.ssh/id_ed25519]
# Exit 0 iff the remote bring_up.sh itself exits 0 (P1-G1+P1-G2 both green).
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: deploy_and_bring_up.sh <user>@<host> [--soak-seconds N] [--identity KEYFILE]" >&2
  exit 2
fi
TARGET="$1"; shift
SOAK_SECONDS=600
IDENTITY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --soak-seconds) SOAK_SECONDS="$2"; shift 2 ;;
    --identity) IDENTITY="-i $2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

LOCAL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # .../open_inline
SSH_OPTS="${IDENTITY} -o StrictHostKeyChecking=accept-new"
REMOTE_ROOT="~/oi-rig/open_inline"

echo "== [1/6] SSH reachability check =="
# shellcheck disable=SC2086
ssh $SSH_OPTS "$TARGET" 'echo ok' || { echo "error: cannot SSH to ${TARGET}" >&2; exit 1; }

echo "== [2/6] rsync open_inline/ -> ${TARGET}:${REMOTE_ROOT} (excluding local build artifacts,"
echo "   which are host-specific binaries best rebuilt fresh on the remote host) =="
# shellcheck disable=SC2086
ssh $SSH_OPTS "$TARGET" "mkdir -p ${REMOTE_ROOT}"
# shellcheck disable=SC2086
rsync -az --delete \
  --exclude='.build/' --exclude='build/' --exclude='__pycache__/' --exclude='*.pyc' \
  -e "ssh ${SSH_OPTS}" \
  "${LOCAL_ROOT}/" "${TARGET}:${REMOTE_ROOT}/"

echo "== [3/6] remote_provision.sh (Docker/Compose/tcpdump/OCUDU checkout, idempotent) =="
# shellcheck disable=SC2086
ssh $SSH_OPTS "$TARGET" "bash ${REMOTE_ROOT}/features/p1-ran-baseline/helpers/remote_provision.sh"

# Docker images are local to the machine that built them -- they do NOT travel with a git/rsync
# push of source. Without this step, `docker compose up` on a fresh host finds no matching image
# and rebuilds from source (a full OCUDU + UHD/DPDK/ROHC compile, tens of minutes) even when an
# already-built, already-verified image sits right here (found the hard way: the first two runs
# of this script silently triggered exactly that rebuild). `oi_p1-5gc:latest` already carries the
# exact tag `docker compose`'s auto-naming (`${COMPOSE_PROJECT_NAME}-5gc`, matching bring_up.sh's
# own `COMPOSE_PROJECT_NAME=oi_p1`) will look for; `ocudu/gnb:latest` is the explicit tag both
# `gnb` and `ru-emu` use. Only transfers what's missing on the remote (checked via a real `docker
# images -q` probe there, not assumed) and falls back to a normal source build for anything not
# available locally -- never silently skips a service.
GNB_IMG="ocudu/gnb:latest"
GC_IMG="oi_p1-5gc:latest"
echo "== [4/6] transferring pre-built images (skip if already present, either side) =="
NEED_TRANSFER=()
for IMG in "$GNB_IMG" "$GC_IMG"; do
  if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "  ${IMG}: not present locally -- will build from source on the remote instead"
    continue
  fi
  # shellcheck disable=SC2086
  REMOTE_HAS="$(ssh $SSH_OPTS "$TARGET" "sudo docker image inspect '$IMG' >/dev/null 2>&1 && echo yes || echo no")"
  if [ "$REMOTE_HAS" = "yes" ]; then
    echo "  ${IMG}: already present on remote, skipping transfer"
  else
    NEED_TRANSFER+=("$IMG")
  fi
done
if [ "${#NEED_TRANSFER[@]}" -gt 0 ]; then
  echo "  streaming ${NEED_TRANSFER[*]} to remote (docker save | gzip | ssh | gunzip | docker load)..."
  # shellcheck disable=SC2086
  docker save "${NEED_TRANSFER[@]}" | gzip | ssh $SSH_OPTS "$TARGET" "gunzip | sudo docker load"
fi

echo "== [5/6] bring_up.sh (P1-G1 + P1-G2, soak=${SOAK_SECONDS}s) =="
# shellcheck disable=SC2086
ssh $SSH_OPTS "$TARGET" \
  "bash ${REMOTE_ROOT}/features/p1-ran-baseline/helpers/bring_up.sh --soak-seconds ${SOAK_SECONDS}"
BRING_UP_RC=$?

echo "== [6/6] pulling back artifacts/p1/ (pcap corpus + manifests) =="
mkdir -p "${LOCAL_ROOT}/artifacts/p1"
# Real bug found + fixed (2026-07-25): this used to rsync from "${TARGET}:~/oi-rig/artifacts/p1/"
# -- but archive_pcap.sh's default OUT_ROOT is "${ROOT}/../../artifacts/p1/pcaps" where ROOT is
# .../open_inline/features/p1-ran-baseline, i.e. the real path is
# ~/oi-rig/open_inline/artifacts/p1/, one directory deeper than this script assumed. The old path
# silently matched nothing every single run (rsync's own "not fatal" fallback message masked it
# as a normal "soak didn't complete" case) -- P1-R10's pcap corpus was NEVER actually pulled back
# locally by this script, on any prior GCP run tonight, until this fix.
# shellcheck disable=SC2086
rsync -az -e "ssh ${SSH_OPTS}" \
  "${TARGET}:${REMOTE_ROOT}/artifacts/p1/" "${LOCAL_ROOT}/artifacts/p1/" 2>/dev/null || \
  echo "  (no artifacts/p1/ produced on remote yet, or soak didn't complete -- not fatal to this script)"

if [ "$BRING_UP_RC" -eq 0 ]; then
  echo "deploy_and_bring_up: PASS (P1-G1 + P1-G2 green on ${TARGET})"
else
  echo "deploy_and_bring_up: bring_up.sh exited ${BRING_UP_RC} on ${TARGET} -- see output above" >&2
fi
exit "$BRING_UP_RC"
