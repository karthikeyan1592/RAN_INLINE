#!/usr/bin/env bash
# assert_clinfo.sh — IF-P0-CLPLATFORM assertion (P0-R4).
# Runs INSIDE the gpu-phy container (or via `docker compose exec gpu-phy ...`).
#
# usage: assert_clinfo.sh [--platform-regex REGEX]   # default: 'Portable Computing Language'
# Exit 0 iff exactly one platform matches REGEX and $OI_CL_PLATFORM resolves to it.
# Exit != 0 otherwise, printing all platforms found (Error handling table, LLD).
set -u

REGEX='Portable Computing Language'
while [ $# -gt 0 ]; do
  case "$1" in
    --platform-regex) REGEX="$2"; shift 2 ;;
    *) echo "usage: assert_clinfo.sh [--platform-regex REGEX]" >&2; exit 2 ;;
  esac
done

if ! command -v clinfo >/dev/null 2>&1; then
  echo "error: clinfo not found in image" >&2
  exit 1
fi

ALL_PLATFORMS="$(clinfo -l 2>/dev/null)"
MATCHES="$(printf '%s\n' "$ALL_PLATFORMS" | grep -c "$REGEX" || true)"

if [ "$MATCHES" -ne 1 ]; then
  echo "error: expected exactly 1 platform matching '$REGEX', found $MATCHES" >&2
  echo "--- all platforms ---" >&2
  printf '%s\n' "$ALL_PLATFORMS" >&2
  exit 1
fi

# OI_CL_PLATFORM resolution check: 'pocl' must map to the PoCL platform found above.
case "${OI_CL_PLATFORM:-}" in
  pocl)
    ;;
  rocm|nvidia|intel-neo)
    echo "error: OI_CL_PLATFORM='${OI_CL_PLATFORM}' is a PHYSICAL-tier value; SIM only resolves 'pocl'" >&2
    exit 1
    ;;
  "")
    echo "error: OI_CL_PLATFORM is unset" >&2
    exit 1
    ;;
  *)
    echo "error: unknown platform '${OI_CL_PLATFORM}'" >&2
    exit 1
    ;;
esac

DEVICE_COUNT="$(printf '%s\n' "$ALL_PLATFORMS" | grep -c 'Device #' || true)"
echo "platform=${REGEX} devices=${DEVICE_COUNT}"
exit 0
