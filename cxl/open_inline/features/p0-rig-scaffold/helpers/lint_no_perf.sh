#!/usr/bin/env bash
# lint_no_perf.sh — P1-R12-class no-perf-threshold CI gate, p0's own feature-scoped copy (see
# p1-ran-baseline/helpers/lint_no_perf.sh's header for the identical rationale; this copy exists
# because the shared p2a-scaffold copy's find pattern is hardcoded to */p2*/... and silently
# never scanned p0 — the same per-feature-copy gap found and fixed for p3 and p4 this session).
#
# Greps this feature's gating code (docker/, helpers/, tests/ — NOT README.md/spec/ prose, which
# may legitimately discuss timing context) for threshold-bearing keys. Zero hits required.
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
          \( -path "*/docker/*" -o -path "*/helpers/*" -o -path "*/tests/*" -o -path "*/tools/*" \) \
          -type f \( -name "*.sh" -o -name "*.py" -o -name "*.yml" -o -name "*.yaml" -o -name "*.cpp" \) \
          -not -name "lint_no_perf.sh" -print0 2>/dev/null)

if [ "$HITS" -gt 0 ]; then
  echo "lint_no_perf: ${HITS} file(s) with threshold-bearing perf keys used as gating operands" >&2
  exit 1
fi

echo "PASS: lint_no_perf — zero perf-threshold hits across p0's docker/helpers/tests/tools"
exit 0
