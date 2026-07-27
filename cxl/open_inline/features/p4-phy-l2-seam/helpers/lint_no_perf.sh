#!/usr/bin/env bash
# lint_no_perf.sh — P4-R13 no-perf-threshold CI gate (shared pattern with p0/p1/p2/p3; see
# p2a-scaffold/helpers/lint_no_perf.sh's own header for the identical rationale). Each feature
# owns its own copy, scoped to its own tree (p2a's copy is hardcoded to `p2*` paths and would
# silently scan nothing here -- see p3's own lint_no_perf.sh header for that real, found gap).
#
# P4-R13's own carve-out: `t_enqueue_ns` (oi_seam_slot_t) is observational only and must never be
# a gate operand -- this script's own PATTERN wouldn't match a plain field name anyway, but the
# exclusion list still keeps "not quotable"/"debug"/"log"/"note"-tagged lines permitted, matching
# every other feature's convention.
set -uo pipefail

FEATURE_ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

PATTERN='(_max_us\b|_min_us\b|latency_threshold|throughput_threshold|assert.*\blatency\b|assert.*\bthroughput\b|\bmax_latency\b|\bmin_throughput\b|\bgbps\b|\bmbps\b)'

HITS=0
while IFS= read -r -d '' f; do
  MATCHES="$(grep -nE "$PATTERN" "$f" 2>/dev/null | grep -viE '(note|debug|log|not quotable)' || true)"
  if [ -n "$MATCHES" ]; then
    echo "FAIL: $f:" >&2
    echo "$MATCHES" | sed 's/^/  /' >&2
    HITS=$((HITS + 1))
  fi
done < <(find "$FEATURE_ROOT" \
          \( -path "*/src/*" -o -path "*/docker/*" -o -path "*/helpers/*" -o -path "*/tests/*" -o -path "*/gates/*" \) \
          -type f \( -name "*.sh" -o -name "*.py" -o -name "*.yml" -o -name "*.yaml" -o -name "*.c" -o -name "*.h" \) \
          -not -name "lint_no_perf.sh" -print0 2>/dev/null)

if [ "$HITS" -gt 0 ]; then
  echo "lint_no_perf: ${HITS} file(s) with threshold-bearing perf keys used as gating operands" >&2
  exit 1
fi

echo "PASS: lint_no_perf — zero perf-threshold hits across p4's src/docker/helpers/tests"
exit 0
