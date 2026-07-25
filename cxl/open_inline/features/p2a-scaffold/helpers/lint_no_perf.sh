#!/usr/bin/env bash
# lint_no_perf.sh — P2-R16 no-perf-threshold CI gate (shared pattern with p0/p1).
#
# Greps the p2 sub-feature family's gating code (src/, tests/ — NOT README.md prose, which may
# legitimately discuss performance context) for threshold-bearing keys. Zero hits required.
# Kernel-time printouts are permitted only as unasserted debug/log lines — this script excludes
# lines tagged `debug`/`log` from the ban, matching LLD §7's carve-out.
#
# Usage: lint_no_perf.sh [FEATURES_ROOT]
set -uo pipefail

FEATURES_ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# Patterns that would indicate a latency/throughput value used as a pass/fail operand.
PATTERN='(_max_us\b|_min_us\b|latency_threshold|throughput_threshold|assert.*\blatency\b|assert.*\bthroughput\b|\bmax_latency\b|\bmin_throughput\b)'

HITS=0
while IFS= read -r -d '' f; do
  # Exclude lines explicitly tagged as debug/log output (LLD §7 carve-out: "unasserted debug
  # output only" is permitted) and exclude this script itself (it necessarily mentions the
  # pattern names).
  MATCHES="$(grep -nE "$PATTERN" "$f" 2>/dev/null | grep -viE '(debug|log)' || true)"
  if [ -n "$MATCHES" ]; then
    echo "FAIL: $f:" >&2
    echo "$MATCHES" | sed 's/^/  /' >&2
    HITS=$((HITS + 1))
  fi
done < <(find "$FEATURES_ROOT" \
          \( -path "*/p2*/src/*" -o -path "*/p2*/tests/*" -o -path "*/p2*/helpers/*" \) \
          -type f \( -name "*.py" -o -name "*.cl" -o -name "*.h" -o -name "*.cpp" -o -name "*.sh" \) \
          -not -name "lint_no_perf.sh" -print0 2>/dev/null)

if [ "$HITS" -gt 0 ]; then
  echo "lint_no_perf: ${HITS} file(s) with threshold-bearing perf keys used as gating operands" >&2
  exit 1
fi

echo "PASS: lint_no_perf — zero perf-threshold hits across p2 src/tests/helpers"
exit 0
