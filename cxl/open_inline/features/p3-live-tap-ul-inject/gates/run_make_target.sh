#!/usr/bin/env bash
# run_make_target.sh -- IF-P5-SUITE addendum: thin wrapper translating this feature's existing
# `make run-X-test` convention into the p5 gate contract (one JSON verdict line + 0/1/2/3 exit),
# without modifying the Makefile or any test binary itself (SPEC "Out of scope": an addendum each
# feature adds at its own implementation time, not a modification of already-written scripts).
set -uo pipefail

FEATURE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:?usage: run_make_target.sh MAKE_TARGET}"

OUT="$(cd "$FEATURE_ROOT" && make "$TARGET" 2>&1)"
CODE=$?
echo "$OUT" >&2

if [ "$CODE" -ne 0 ] && [ "$CODE" -ne 1 ]; then
  # Build/bootstrap failure (missing OCUDU bootstrap, compile error, etc.) -- a setup error,
  # never conflated with a real test-assertion FAIL (LLD error-handling convention).
  echo "{\"check\": \"${TARGET}\", \"ok\": false, \"make_exit_code\": ${CODE}}"
  exit 2
fi

if [ "$CODE" -eq 0 ]; then
  echo "{\"check\": \"${TARGET}\", \"ok\": true}"
else
  echo "{\"check\": \"${TARGET}\", \"ok\": false}"
fi
exit "$CODE"
