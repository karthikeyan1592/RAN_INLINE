#!/usr/bin/env bash
# run_ldpc_suite.sh — IF-P0-SUITE wrapper (P0-R6). Runs INSIDE gpu-phy.
#
# usage: run_ldpc_suite.sh [--vectors DIR] [--messages N] [--iters N]
# Exit 0 iff suite reports 0 bit mismatches. Stdout ends with one JSON line:
#   {"suite":"ldpc","cases":N,"mismatches":0,"platform":"pocl"}
#
# Note: bit_diff_test.cpp is self-contained (generates its own random messages via OCUDU's LDPC
# encoder at runtime; --vectors is accepted for interface symmetry with the other helpers but
# unused by this suite — see docker/gpu-phy/ldpc_suite/MODIFICATIONS.md).
set -u

MESSAGES=200
ITERS=6
VECTORS="${OI_VECTOR_DIR:-/oi/vectors}/ldpc"

while [ $# -gt 0 ]; do
  case "$1" in
    --vectors) VECTORS="$2"; shift 2 ;;
    --messages) MESSAGES="$2"; shift 2 ;;
    --iters) ITERS="$2"; shift 2 ;;
    *) echo "usage: run_ldpc_suite.sh [--vectors DIR] [--messages N] [--iters N]" >&2; exit 2 ;;
  esac
done

BIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUITE_BIN="/oi/bin/bit_diff_test"
[ -x "$SUITE_BIN" ] || SUITE_BIN="${BIN_DIR}/docker/gpu-phy/ldpc_suite/bit_diff_test"

if [ ! -x "$SUITE_BIN" ]; then
  echo "error: bit_diff_test binary not found (expected at /oi/bin/bit_diff_test in-container)" >&2
  exit 2
fi

cd "$(dirname "$SUITE_BIN")" || exit 2
OUT="$("$SUITE_BIN" "$MESSAGES" "$ITERS" 2>&1)"
STATUS=$?
printf '%s\n' "$OUT"

TOTAL_MISMATCH=0
CASES=0
while IFS=, read -r bg ls ls_idx n_iter n_msg n_bits mismatches rate verdict; do
  [ "$bg" = "bg" ] && continue   # header row
  [ -z "$bg" ] && continue
  CASES=$((CASES + 1))
  TOTAL_MISMATCH=$((TOTAL_MISMATCH + mismatches))
done < "${OI_LOG_DIR:-/oi/logs}/bit_correctness.csv" 2>/dev/null

echo "{\"suite\":\"ldpc\",\"cases\":${CASES},\"mismatches\":${TOTAL_MISMATCH},\"platform\":\"${OI_CL_PLATFORM:-pocl}\"}"

if [ "$STATUS" -ne 0 ] || [ "$TOTAL_MISMATCH" -ne 0 ]; then
  exit 1
fi
exit 0
