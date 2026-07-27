#!/usr/bin/env bash
# run_du_undisturbed_check.sh — M6, P3-R12. Per LLD §Module breakdown: "reuses P1's KPI/log-diff
# scripts unmodified". This is deliberately a thin pass-through, not new logic: P3-R12's pass
# criteria ("gnb shows no crash, no new ERROR-level logs vs the P1 baseline, stable KPI counters")
# is byte-for-byte identical to P1-R9/P1-G2's own soak_stability.sh gate -- the only difference is
# WHEN it's run (concurrently with a P3-R11 injection session instead of P1's standalone soak).
# No p3-owned copy of the script exists; calling p1's directly avoids the exact kind of drift risk
# this project's shared-library precedents (oi_oracle_pack, oi_osg_schedule) already guard against
# elsewhere.
set -euo pipefail

P1_HELPERS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../p1-ran-baseline/helpers" && pwd)"

echo "== M6/P3-R12: DU-undisturbed check (delegating to p1-ran-baseline/helpers/soak_stability.sh, unmodified) =="
exec "${P1_HELPERS}/soak_stability.sh" "$@"
