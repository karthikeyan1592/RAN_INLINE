#!/usr/bin/env bash
# run_soak_stability.sh -- IF-P5-SUITE addendum wrapping helpers/soak_stability.sh. That script
# already emits a JSON result on stdout on its normal (0/1) path, but writes its precondition-
# failure JSON to stderr on the 2/3 paths (predates this contract) -- this wrapper guarantees a
# valid JSON verdict line on stdout for every exit code, without modifying soak_stability.sh
# itself (SPEC "Out of scope").
set -uo pipefail

FEATURE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUT="$("${FEATURE_ROOT}/helpers/soak_stability.sh" "$@" 2>&1)"
CODE=$?
echo "$OUT" >&2

case "$CODE" in
  0) echo "{\"check\": \"soak_stability\", \"ok\": true}"; exit 0 ;;
  1) echo "{\"check\": \"soak_stability\", \"ok\": false}"; exit 1 ;;
  3) echo "{\"check\": \"soak_stability\", \"ok\": false, \"reason\": \"sctp_precondition_failed\"}"; exit 3 ;;
  *) echo "{\"check\": \"soak_stability\", \"ok\": false, \"unexpected_exit\": ${CODE}}"; exit 2 ;;
esac
