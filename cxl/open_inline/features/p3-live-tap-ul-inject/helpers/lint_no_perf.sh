#!/usr/bin/env bash
# lint_no_perf.sh — P3-R14 no-perf-threshold CI gate (shared pattern with p0/p1/p2; see
# p2a-scaffold/helpers/lint_no_perf.sh's own header for the identical rationale).
#
# Real gap found + fixed (2026-07-26): this feature's earlier regression sweeps only ran p2a's OWN
# copy of this script, whose `find` pattern is hardcoded to `*/p2*/...` paths -- it silently never
# scanned ANYTHING under p3-live-tap-ul-inject/ at all (no path segment matches "p2*"). Every
# per-feature lint_no_perf.sh copy must be its own, scoped to its own tree, matching p1's
# established precedent -- this file is that copy for p3, created now, not assumed already covered.
#
# Greps this feature's gating code (src/, docker/, helpers/, tests/, tools/, patches/ — NOT
# README.md/spec/ prose) for threshold-bearing keys. Zero hits required. P3-R14's own carve-out:
# any timing figure printed by M4/M5 tooling must carry the literal string "SIM — not quotable" --
# this script's exclusion list matches that (excludes lines containing "not quotable"/"debug"/
# "log"/"note", same class of carve-out p0/p1/p2 already established).
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
          \( -path "*/src/*" -o -path "*/docker/*" -o -path "*/helpers/*" -o -path "*/tests/*" -o -path "*/tools/*" -o -path "*/patches/*" -o -path "*/gates/*" \) \
          -type f \( -name "*.sh" -o -name "*.py" -o -name "*.yml" -o -name "*.yaml" -o -name "*.cpp" -o -name "*.h" \) \
          -not -name "lint_no_perf.sh" -print0 2>/dev/null)

if [ "$HITS" -gt 0 ]; then
  echo "lint_no_perf: ${HITS} file(s) with threshold-bearing perf keys used as gating operands" >&2
  exit 1
fi

echo "PASS: lint_no_perf — zero perf-threshold hits across p3's src/docker/helpers/tests/tools/patches"
exit 0
