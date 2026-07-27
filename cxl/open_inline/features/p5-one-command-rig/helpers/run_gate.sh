#!/usr/bin/env bash
# run_gate.sh -- P5-R4/R5/R6/D3: external timeout wrapper + stdout/stderr capture for one gate.
#
# Does not classify PASS/FAIL/BLOCKED itself (that needs the JSON-verdict-line parse, done by
# ledger_build.py) -- this script only invokes, times out, and captures, per D3 (timeout applied
# externally so p0/p1/p3's existing scripts need zero changes).
set -u

ID=""
TYPE=""
TIMEOUT_S=""
ARTIFACTS_DIR=""

while [ $# -gt 0 ]; do
  case "$1" in
    --id) ID="$2"; shift 2 ;;
    --type) TYPE="$2"; shift 2 ;;
    --timeout-s) TIMEOUT_S="$2"; shift 2 ;;
    --artifacts-dir) ARTIFACTS_DIR="$2"; shift 2 ;;
    --) shift; break ;;
    *) echo "run_gate.sh: unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$ID" ] || [ -z "$TYPE" ] || [ -z "$TIMEOUT_S" ] || [ -z "$ARTIFACTS_DIR" ] || [ $# -lt 1 ]; then
  echo "usage: run_gate.sh --id ID --type unit|integration --timeout-s N --artifacts-dir DIR -- <script> [args...]" >&2
  exit 2
fi

mkdir -p "$ARTIFACTS_DIR"
SCRIPT="$1"
shift

if [ ! -x "$SCRIPT" ]; then
  echo "run_gate.sh: script not found or not executable: $SCRIPT" >&2 | tee "$ARTIFACTS_DIR/stderr.log" >/dev/null
  : > "$ARTIFACTS_DIR/stdout.log"
  echo "not found or not executable: $SCRIPT" > "$ARTIFACTS_DIR/stderr.log"
  echo 2 > "$ARTIFACTS_DIR/exit_code"
  echo 0 > "$ARTIFACTS_DIR/duration_s"
  exit 2
fi

START=$(date +%s.%N)
timeout --signal=TERM --kill-after=5 "${TIMEOUT_S}s" "$SCRIPT" "$@" \
  > "$ARTIFACTS_DIR/stdout.log" 2> "$ARTIFACTS_DIR/stderr.log"
CODE=$?
END=$(date +%s.%N)

echo "$CODE" > "$ARTIFACTS_DIR/exit_code"
awk -v s="$START" -v e="$END" 'BEGIN{printf "%.3f\n", (e-s)}' > "$ARTIFACTS_DIR/duration_s"

exit "$CODE"
