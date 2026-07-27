#!/usr/bin/env bash
# lint_ledger_no_perf.sh -- P5-R8/R13: rollup no-perf lint, defense-in-depth on top of each
# feature's own lint_no_perf.sh. Scans the built ledger.json plus every captured stdout/stderr
# log under the run's artifacts directory for forbidden latency/throughput-threshold patterns
# used as an asserted operand rather than an observational note (same pattern list as every
# feature's own lint_no_perf.sh -- see p1-ran-baseline/helpers/lint_no_perf.sh's header).
#
# Applies only at --tier sim (P5-R13); the --tier physical path explicitly skips this (not
# silently -- the caller must pass --tier physical to get that skip, logged below).
set -uo pipefail

ARTIFACTS_DIR="${1:?usage: lint_ledger_no_perf.sh ARTIFACTS_DIR [--tier sim|physical]}"
TIER="sim"
if [ "${2:-}" = "--tier" ]; then
  TIER="${3:-sim}"
fi

if [ "$TIER" = "physical" ]; then
  echo "SKIP: lint_ledger_no_perf -- --tier physical explicitly exempted (README carve-out, P5-R13)"
  exit 0
fi

PATTERN='(_max_us\b|_min_us\b|latency_threshold|throughput_threshold|assert.*\blatency\b|assert.*\bthroughput\b|\bmax_latency\b|\bmin_throughput\b|\bgbps\b|\bmbps\b)'

HITS=0
while IFS= read -r -d '' f; do
  MATCHES="$(grep -nE "$PATTERN" "$f" 2>/dev/null | grep -viE '(note|debug|log)' || true)"
  if [ -n "$MATCHES" ]; then
    echo "FAIL: $f:" >&2
    echo "$MATCHES" | sed 's/^/  /' >&2
    HITS=$((HITS + 1))
  fi
done < <(find "$ARTIFACTS_DIR" -type f \( -name "*.json" -o -name "*.log" -o -name "*.md" \) -print0 2>/dev/null)

if [ "$HITS" -gt 0 ]; then
  echo "lint_ledger_no_perf: ${HITS} file(s) with threshold-bearing perf keys used as gating operands" >&2
  exit 1
fi

echo "PASS: lint_ledger_no_perf — zero perf-threshold hits across ${ARTIFACTS_DIR}"
exit 0
