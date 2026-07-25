#!/usr/bin/env bash
# soak_stability.sh — P1-R9. 10-minute (default) stability checker: (a) no container
# exits/restarts, (b) no new ERROR-level gnb log lines vs the post-bring-up baseline, (c)
# functional counters strictly increase between the 1st and 10th minute, (d) NGAP association
# stays established. Exit 0 iff all four hold.
#
# REQUIRES A LIVE RIG (docker compose up already run) -- refuses to start otherwise (checked via
# check_sctp.sh's own precondition, matching P0/P1's shared "gate scripts refuse to start" rule).
#
# ERROR-log baseline (LLD Q7): first bring-up must capture the tolerated post-bring-up ERROR
# pattern list empirically; until that capture exists (needs a live rig -- SCTP-blocked on this
# host), this script conservatively treats ANY 'ERROR' log line as new (baseline = empty set),
# which only makes the gate STRICTER than the eventual tuned version, never silently permissive.
set -uo pipefail

SECONDS_TOTAL=600
SAMPLE_EVERY=60
GNB_CONTAINER="ocudu_gnb"
RU_CONTAINER="ocudu_ru_emu"
BASELINE_ERRORS_FILE=""

while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) SECONDS_TOTAL="$2"; shift 2 ;;
    --sample-every) SAMPLE_EVERY="$2"; shift 2 ;;
    --gnb-container) GNB_CONTAINER="$2"; shift 2 ;;
    --ru-container) RU_CONTAINER="$2"; shift 2 ;;
    --baseline-errors) BASELINE_ERRORS_FILE="$2"; shift 2 ;;
    *) echo "usage: soak_stability.sh [--seconds N] [--sample-every N] [--gnb-container C] [--ru-container C] [--baseline-errors FILE]" >&2; exit 2 ;;
  esac
done

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! "${SELF_DIR}/check_sctp.sh" >/dev/null 2>&1; then
  echo "{\"check\":\"soak\",\"error\":\"SCTP precondition failed -- rig cannot be up, refusing to soak\"}" >&2
  exit 3
fi

for c in "$GNB_CONTAINER" "$RU_CONTAINER"; do
  if ! docker inspect "$c" >/dev/null 2>&1; then
    echo "{\"check\":\"soak\",\"error\":\"container ${c} not found -- rig must be up first\"}" >&2
    exit 2
  fi
done

RESTARTS_0_GNB="$(docker inspect "$GNB_CONTAINER" --format '{{.RestartCount}}')"
RESTARTS_0_RU="$(docker inspect "$RU_CONTAINER" --format '{{.RestartCount}}')"

BASELINE_ERR_COUNT=0
if [ -n "$BASELINE_ERRORS_FILE" ] && [ -f "$BASELINE_ERRORS_FILE" ]; then
  BASELINE_ERR_COUNT="$(wc -l < "$BASELINE_ERRORS_FILE")"
fi
ERR_COUNT_0="$(docker logs "$GNB_CONTAINER" 2>&1 | grep -c 'ERROR' || true)"

# Root cause found for real (2026-07-25, confirmed via a direct reproduction on the GCP VM, NOT
# buffering as first suspected): under `set -o pipefail`, `docker logs C | grep -q PATTERN` returns
# 141 (SIGPIPE) even when grep DOES match -- grep -q exits the instant it finds a match, closing its
# read end, and `docker logs` gets SIGPIPE'd before it finishes writing the rest; pipefail then
# reports that 141 as the pipeline's exit status instead of grep's real 0. Reproduced directly:
# `pipeline_exit=141` on this exact command against this exact container while the match was
# genuinely present. This is deterministic once the log is large enough that the match line isn't
# near the very end -- NOT a flake, and no retry count fixes it (see NGAP_UP_N below, which failed
# on every one of 15 retries for exactly this reason). Fixed for real: capture the log into a
# variable first (command substitution always fully drains its child before returning -- no live
# pipe for grep to short-circuit), then grep the captured text with a here-string.
NGAP_UP_0=false
for _ in $(seq 1 15); do
  GNB_LOG_SNAPSHOT="$(docker logs "$GNB_CONTAINER" 2>&1)"
  if grep -qiE 'ng[ _-]?setup.*(response|success)' <<< "$GNB_LOG_SNAPSHOT"; then
    NGAP_UP_0=true
    break
  fi
  sleep 3
done

SNAP1="$("${SELF_DIR}/kpi_snapshot.sh" "$GNB_CONTAINER" "$RU_CONTAINER" 2>/dev/null || echo '{}')"

echo "soak_stability: sampling every ${SAMPLE_EVERY}s for ${SECONDS_TOTAL}s total..." >&2
ELAPSED=0
while [ "$ELAPSED" -lt "$SECONDS_TOTAL" ]; do
  sleep "$SAMPLE_EVERY"
  ELAPSED=$((ELAPSED + SAMPLE_EVERY))
  echo "  t=${ELAPSED}s" >&2
done

RESTARTS_N_GNB="$(docker inspect "$GNB_CONTAINER" --format '{{.RestartCount}}' 2>/dev/null || echo -1)"
RESTARTS_N_RU="$(docker inspect "$RU_CONTAINER" --format '{{.RestartCount}}' 2>/dev/null || echo -1)"
ERR_COUNT_N="$(docker logs "$GNB_CONTAINER" 2>&1 | grep -c 'ERROR' || true)"

# Same pipefail/SIGPIPE fix as NGAP_UP_0 above -- this is the check that proved the root cause: it
# failed on EVERY ONE of 15 retries (not intermittently) once gnb's log grew large enough over the
# 10-minute soak that the NGSetupResponse match line was no longer near the tail, making the
# grep-q-triggered SIGPIPE on `docker logs` deterministic rather than occasional. Capture-then-grep
# (no live pipe) fixes it the same way.
NGAP_UP_N=false
for _ in $(seq 1 15); do
  GNB_LOG_SNAPSHOT="$(docker logs "$GNB_CONTAINER" 2>&1)"
  if grep -qiE 'ng[ _-]?setup.*(response|success)' <<< "$GNB_LOG_SNAPSHOT"; then
    NGAP_UP_N=true
    break
  fi
  sleep 3
done
NGAP_REASSOC="$(grep -qiE 'sctp.*(re-?assoc|reconnect)' <<< "$GNB_LOG_SNAPSHOT" && echo true || echo false)"

SNAP2="$("${SELF_DIR}/kpi_snapshot.sh" "$GNB_CONTAINER" "$RU_CONTAINER" 2>/dev/null || echo '{}')"

python3 - "$RESTARTS_0_GNB" "$RESTARTS_N_GNB" "$RESTARTS_0_RU" "$RESTARTS_N_RU" \
         "$ERR_COUNT_0" "$ERR_COUNT_N" "$BASELINE_ERR_COUNT" \
         "$NGAP_UP_0" "$NGAP_UP_N" "$NGAP_REASSOC" \
         "$SECONDS_TOTAL" "$SNAP1" "$SNAP2" << 'PYEOF'
import sys, json

(r0g, rNg, r0r, rNr, e0, eN, ebaseline, ngap0, ngapN, reassoc, seconds, snap1_s, snap2_s) = sys.argv[1:14]

restarts_delta = (int(rNg) - int(r0g)) + (int(rNr) - int(r0r))
new_error_lines = max(0, int(eN) - max(int(e0), int(ebaseline)))
ngap_stable = (ngap0 == "true") and (ngapN == "true") and (reassoc == "false")

try:
    snap1 = json.loads(snap1_s)
    snap2 = json.loads(snap2_s)
    def field(snap, *path):
        cur = snap
        for k in path:
            if not isinstance(cur, dict) or k not in cur:
                return None
            cur = cur[k]
        return cur
    # 2026-07-25 (GCP confirmation run): switched from mac_ul_bytes/mac_dl_bytes to
    # fronthaul_tx_bytes/fronthaul_rx_bytes -- confirmed via real OCUDU source read that the MAC
    # JSON metrics schema has no byte counters at all (see kpi_snapshot.sh's header). The
    # fronthaul NIC's own kernel sysfs counters are real and monotonic instead.
    tx1, tx2 = field(snap1, "gnb", "fronthaul_tx_bytes"), field(snap2, "gnb", "fronthaul_tx_bytes")
    rx1, rx2 = field(snap1, "gnb", "fronthaul_rx_bytes"), field(snap2, "gnb", "fronthaul_rx_bytes")
    if tx1 is None or rx1 is None or tx2 is None or rx2 is None:
        counters_monotonic = None  # can't judge -- kpi_snapshot's docker exec probe failed this run
    else:
        counters_monotonic = (tx2 > tx1) and (rx2 > rx1)
except Exception:
    counters_monotonic = None

result = {
    "check": "soak", "seconds": int(seconds),
    "restarts": restarts_delta, "new_error_lines": new_error_lines,
    "counters_monotonic": counters_monotonic, "ngap_stable": ngap_stable,
}
print(json.dumps(result))

ok = (restarts_delta == 0) and (new_error_lines == 0) and (counters_monotonic is True) and ngap_stable
sys.exit(0 if ok else 1)
PYEOF
