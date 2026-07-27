#!/usr/bin/env bash
# run_python_script.sh -- IF-P5-SUITE addendum: same rationale as run_make_target.sh, for tests
# invoked directly via `python3 <script>` rather than through a Makefile target (e.g.
# tests/integration/pipeline_test.py, which is CWD-independent on its own but prints plain
# PASS/FAIL text, not a single JSON verdict line).
set -uo pipefail

FEATURE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_REL="${1:?usage: run_python_script.sh SCRIPT_PATH_RELATIVE_TO_FEATURE_ROOT [args...]}"
shift

OUT="$(cd "$FEATURE_ROOT" && python3 "$SCRIPT_REL" "$@" 2>&1)"
CODE=$?
echo "$OUT" >&2

if [ "$CODE" -eq 0 ]; then
  echo "{\"check\": \"${SCRIPT_REL}\", \"ok\": true}"
  exit 0
elif [ "$CODE" -eq 1 ]; then
  echo "{\"check\": \"${SCRIPT_REL}\", \"ok\": false}"
  exit 1
else
  echo "{\"check\": \"${SCRIPT_REL}\", \"ok\": false, \"exit_code\": ${CODE}}"
  exit 2
fi
