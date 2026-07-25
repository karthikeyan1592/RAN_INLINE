#!/usr/bin/env bash
# remote_provision.sh — runs ON the target host (not locally). Installs everything P1's bring-up
# needs that isn't already inside a container image: Docker + Compose v2, tcpdump (P1-R8 capture),
# python3 + PyYAML (rigcfg_crosscheck.sh, archive_pcap.sh, assert_ecpri.sh's classifier), git.
# Then clones the pinned OCUDU checkout at the exact tag this project targets, into the sibling
# directory layout every relative path in this repo (docker/upstream symlink, Makefiles'
# ../../../third_party/ocudu) already assumes.
#
# Usage (on the remote host, after deploy_and_bring_up.sh has rsynced open_inline/ into place):
#   ~/oi-rig/open_inline/features/p1-ran-baseline/helpers/remote_provision.sh
#
# Idempotent: safe to re-run (skips steps whose target already exists/is correct).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # .../oi-rig/open_inline
SIBLING_ROOT="$(cd "${ROOT}/.." && pwd)"                        # .../oi-rig
OCUDU_DIR="${SIBLING_ROOT}/third_party/ocudu"
OCUDU_TAG="release_26_04"   # pinned tag (research/ocudu_repin.md) — must match p0's own pin

# GCP's default user setup varies (root directly on some images, a passwordless-sudo non-root
# user on stock Ubuntu images) -- handle both without assuming which.
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi

echo "== [1/5] apt packages (docker, compose plugin, tcpdump, python3-yaml, git) =="
if ! command -v docker >/dev/null 2>&1; then
  curl -fsSL https://get.docker.com | $SUDO sh
fi
$SUDO apt-get update -qq
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  docker-compose-plugin tcpdump python3 python3-yaml git libcap2-bin >/dev/null

if [ -n "$SUDO" ]; then
  # Group membership only takes effect on a NEW login session -- fine here, since this script's
  # caller (deploy_and_bring_up.sh) always opens a fresh ssh connection for the next step
  # (bring_up.sh), not a continuation of this same shell.
  $SUDO usermod -aG docker "$(id -un)" || true
fi

echo "== [2/5] tcpdump raw-capture capability (P1-R8/P1-R10 need this as a non-root user) =="
# Found on the real GCP run (2026-07-25): this image's tcpdump package does NOT setcap
# cap_net_raw automatically the way some Ubuntu/Debian tcpdump packages do -- `assert_ecpri.sh`
# (called by bring_up.sh without sudo, matching how it must also work for a real p3/p6
# ingest_backend eventually) got "You don't have permission to perform this capture on that
# device (socket: Operation not permitted)" as a plain non-root user despite passwordless sudo
# being available. Grant the two capabilities tcpdump actually needs (NET_RAW to open the packet
# socket, NET_ADMIN for promiscuous-mode/interface manipulation) directly on the binary --
# narrower than running the whole capture via sudo, and works for any user, not just this one.
TCPDUMP_BIN="$(command -v tcpdump)"
if ! getcap "$TCPDUMP_BIN" 2>/dev/null | grep -q cap_net_raw; then
  $SUDO setcap cap_net_raw,cap_net_admin+eip "$TCPDUMP_BIN"
fi

echo "== [3/5] docker daemon reachable =="
if ! $SUDO docker info >/dev/null 2>&1; then
  echo "error: docker daemon not reachable even via sudo -- check Docker install" >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  echo "  note: 'docker' needs sudo/group membership in THIS session; a fresh ssh session (e.g." \
       "the next step's separate ssh call) should pick up the new docker-group membership above."
fi
$SUDO docker compose version

echo "== [4/5] OCUDU checkout at ${OCUDU_TAG} (sibling of open_inline/, matches every relative"
echo "   path this repo already assumes: docker/upstream symlink, Makefiles' ../../../third_party/ocudu)"
mkdir -p "${SIBLING_ROOT}/third_party"
if [ -d "${OCUDU_DIR}/.git" ]; then
  CURRENT_TAG="$(git -C "${OCUDU_DIR}" describe --tags --exact-match 2>/dev/null || echo UNPINNED)"
  if [ "$CURRENT_TAG" = "$OCUDU_TAG" ]; then
    echo "  already present at ${OCUDU_TAG}, skipping clone"
  else
    echo "error: ${OCUDU_DIR} exists but is at '${CURRENT_TAG}', not '${OCUDU_TAG}' -- resolve manually" >&2
    exit 1
  fi
else
  git clone --depth 1 --branch "${OCUDU_TAG}" https://gitlab.com/ocudu/ocudu.git "${OCUDU_DIR}"
fi

echo "== [5/5] SCTP precondition (the whole reason for moving off the dev box) =="
if bash "${ROOT}/features/p1-ran-baseline/helpers/check_sctp.sh"; then
  echo "  SCTP: available -- P1-R7/R9/G2 are unblocked on this host"
else
  echo "  SCTP: still unavailable on this host -- P1-R7/R9/G2 will remain blocked here too" >&2
fi

echo "remote_provision: DONE"
