#!/usr/bin/env bash
# test_oracle_verdict.sh — P0-R5 test plan: PASS on known-good, FAIL on 1-bit-flip, exit 2 on
# missing case ID. Runs against the built oracle image's vector store + CLI.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VDIR="${ROOT}/docker/oracle/vectors"
CLI="${ROOT}/src/oracle_verdict/oracle_verdict.py"
KERNEL=ldpc
CASE=example_case_01

FAIL=0

echo "-- PASS case --"
python3 "$CLI" --kernel "$KERNEL" --case "$CASE" \
  --result "${VDIR}/${KERNEL}/${CASE}/expected.bin" --vector-dir "$VDIR"
[ $? -eq 0 ] || { echo "FAIL: expected exit 0 on known-good result"; FAIL=1; }

echo "-- FAIL case (1-bit-flipped) --"
TMP="$(mktemp)"
python3 -c "
d = bytearray(open('${VDIR}/${KERNEL}/${CASE}/expected.bin','rb').read())
d[0] ^= 0x01
open('${TMP}','wb').write(bytes(d))
"
python3 "$CLI" --kernel "$KERNEL" --case "$CASE" --result "$TMP" --vector-dir "$VDIR"
STATUS=$?
rm -f "$TMP"
[ "$STATUS" -eq 1 ] || { echo "FAIL: expected exit 1 on 1-bit-flipped result, got ${STATUS}"; FAIL=1; }

echo "-- missing case ID (exit 2) --"
python3 "$CLI" --kernel "$KERNEL" --case nonexistent_case \
  --result "${VDIR}/${KERNEL}/${CASE}/expected.bin" --vector-dir "$VDIR" 2>/dev/null
STATUS=$?
[ "$STATUS" -eq 2 ] || { echo "FAIL: expected exit 2 on missing case, got ${STATUS}"; FAIL=1; }

if [ "$FAIL" -eq 0 ]; then
  echo "test_oracle_verdict: ALL PASS"
  exit 0
else
  echo "test_oracle_verdict: FAILURES ABOVE"
  exit 1
fi
