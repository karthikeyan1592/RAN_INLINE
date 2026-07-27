#!/usr/bin/env bash
# lint_no_perf.sh — P1-R12 no-perf-threshold CI gate (shared pattern with p0/p2; see
# p2a-scaffold/helpers/lint_no_perf.sh's own header for the identical rationale).
#
# Greps this feature's gating code (docker/, helpers/, tests/ — NOT README.md/spec/ prose, which
# may legitimately discuss timing context) for threshold-bearing keys. Zero hits required.
# Timing-flavored KPIs (ru_emulator rx-window early/on-time/late, D6/kpi_snapshot.sh's `note`-
# tagged fields) are permitted only as unasserted observations — this script excludes lines tagged
# `note`/`debug`/`log` from the ban, matching LLD's D6 carve-out.
set -uo pipefail

FEATURE_ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

PATTERN='(_max_us\b|_min_us\b|latency_threshold|throughput_threshold|assert.*\blatency\b|assert.*\bthroughput\b|\bmax_latency\b|\bmin_throughput\b|\bgbps\b|\bmbps\b)'

HITS=0
while IFS= read -r -d '' f; do
  MATCHES="$(grep -nE "$PATTERN" "$f" 2>/dev/null | grep -viE '(note|debug|log)' || true)"
  if [ -n "$MATCHES" ]; then
    echo "FAIL: $f:" >&2
    echo "$MATCHES" | sed 's/^/  /' >&2
    HITS=$((HITS + 1))
  fi
done < <(find "$FEATURE_ROOT" \
          \( -path "*/docker/*" -o -path "*/helpers/*" -o -path "*/tests/*" -o -path "*/tools/*" -o -path "*/gates/*" \) \
          -type f \( -name "*.sh" -o -name "*.py" -o -name "*.yml" -o -name "*.yaml" -o -name "*.cpp" \) \
          -not -name "lint_no_perf.sh" -print0 2>/dev/null)

if [ "$HITS" -gt 0 ]; then
  echo "lint_no_perf: ${HITS} file(s) with threshold-bearing perf keys used as gating operands" >&2
  exit 1
fi

echo "PASS: lint_no_perf — zero perf-threshold hits across p1's docker/helpers/tests/tools"
exit 0
