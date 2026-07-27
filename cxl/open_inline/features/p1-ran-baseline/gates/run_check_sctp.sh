#!/usr/bin/env bash
# run_check_sctp.sh -- IF-P5-SUITE addendum wrapping helpers/check_sctp.sh (shared with p0), which
# predates this contract and emits plain text (not a JSON verdict line) on stdout. This wrapper
# adds the one required JSON line without modifying check_sctp.sh itself (SPEC "Out of scope").
set -uo pipefail

FEATURE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUT="$("${FEATURE_ROOT}/helpers/check_sctp.sh" 2>&1)"
CODE=$?
echo "$OUT" >&2

case "$CODE" in
  0) echo "{\"check\": \"check_sctp\", \"ok\": true}"; exit 0 ;;
  3) echo "{\"check\": \"check_sctp\", \"ok\": false, \"reason\": \"sctp_unavailable\"}"; exit 3 ;;
  *) echo "{\"check\": \"check_sctp\", \"ok\": false, \"unexpected_exit\": ${CODE}}"; exit 2 ;;
esac
