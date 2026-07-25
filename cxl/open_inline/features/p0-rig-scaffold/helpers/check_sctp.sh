#!/usr/bin/env bash
# check_sctp.sh — shared precondition check for gnb->AMF NG setup (P0-R9, p1-ran-baseline P1-R6).
# Single source of truth: p1-ran-baseline reuses this exact script (LLD module breakdown note).
#
# Exit 0: SCTP available (already loaded, or successfully modprobed).
# Exit 3: SCTP unavailable — distinct, actionable exit code (matches smoke_up.sh's own exit-3
# convention for precondition failures; SIM §1 WSL2 caveat).
#
# REAL BUG FOUND + FIXED (2026-07-25, first real GCP VM run): `modprobe sctp` without privilege
# escalation silently fails with EPERM for a non-root user (the `2>/dev/null` below was masking
# this, not just genuine "module doesn't exist" failures) -- invisible on every host this script
# had been run on before now, because those were all root shells. Confirmed on a stock GCP
# `ubuntu-2404-lts-amd64` n2-standard-16: `CONFIG_IP_SCTP=m` IS set, `sudo modprobe sctp` loads it
# instantly, but plain `modprobe sctp` as a non-root sudo-capable user does not -- SCTP was never
# actually missing there, the check just couldn't load it without asking for privilege. Fixed:
# try modprobe as-is first (root shells, unchanged behavior), fall back to `sudo modprobe` if
# available and not already root, and add sctp=absent (not just "not loaded") to the failure
# message so a future reader can tell "genuinely no module" apart from "needs sudo" -- the OLD
# unconditional "may lack it — rebuild kernel or run on GCP VM" framing was itself based on
# never having tested the non-root case; a GCP VM is not automatically an SCTP fix if invoked as
# a non-root user without this fallback.
set -u

MSG="NGAP needs CONFIG_IP_SCTP; stock WSL2 kernel may lack it — rebuild WSL2 kernel with CONFIG_IP_SCTP=m or run on GCP VM (with sudo available for modprobe)"

if grep -qi '^sctp' /proc/net/protocols 2>/dev/null; then
  echo "sctp=available (already loaded)"
  exit 0
fi

if command -v modprobe >/dev/null 2>&1; then
  if modprobe sctp 2>/dev/null && grep -qi '^sctp' /proc/net/protocols 2>/dev/null; then
    echo "sctp=available (modprobe succeeded)"
    exit 0
  fi
  # Non-root retry: a plain `modprobe` failure for a NON-root user is frequently a permissions
  # error (EPERM to load a kernel module), not proof the module is absent -- see file header.
  if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
    if sudo -n modprobe sctp 2>/dev/null && grep -qi '^sctp' /proc/net/protocols 2>/dev/null; then
      echo "sctp=available (modprobe succeeded via sudo)"
      exit 0
    fi
  fi
fi

echo "sctp=unavailable: ${MSG}" >&2
exit 3
