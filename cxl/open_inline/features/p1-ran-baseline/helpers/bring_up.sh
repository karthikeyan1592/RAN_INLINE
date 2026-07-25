#!/usr/bin/env bash
# bring_up.sh — full P1 bring-up + gate sequence (P1-G1 + P1-G2), the GCP-VM/SCTP-capable-host
# counterpart to p0's smoke_up.sh (same overall shape: SCTP precheck -> compose up -> stability
# hold -> log check), extended with P1's own ru-emu service, eCPRI assertion, 10-minute soak, and
# pcap archival.
#
# Usage: bring_up.sh [--hold-seconds 60] [--soak-seconds 600] [--run-id ID]
# Exit 0 iff: SCTP precheck passes; compose up succeeds (5gc, gnb, ru-emu); all three stable for
# the hold window; gnb NG-setup-attempt log line seen; eCPRI classifier (P1-R8) passes; soak
# (P1-R9) passes; pcap archived (P1-R10).
# Exit 3 = SCTP precondition failed (distinct, actionable, matches p0's own convention).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
P0_DOCKER="${ROOT}/../p0-rig-scaffold/docker"
export COMPOSE_PROJECT_NAME="oi_p1"

HOLD=60
SOAK_SECONDS=600
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"

while [ $# -gt 0 ]; do
  case "$1" in
    --hold-seconds) HOLD="$2"; shift 2 ;;
    --soak-seconds) SOAK_SECONDS="$2"; shift 2 ;;
    --run-id) RUN_ID="$2"; shift 2 ;;
    *) echo "usage: bring_up.sh [--hold-seconds N] [--soak-seconds N] [--run-id ID]" >&2; exit 2 ;;
  esac
done

echo "== [1/7] SCTP precondition =="
if ! "${ROOT}/helpers/check_sctp.sh"; then
  exit 3
fi

echo "== [2/7] rigcfg cross-consistency =="
if ! "${ROOT}/helpers/rigcfg_crosscheck.sh"; then
  echo "error: rig config cross-check failed -- refusing to bring up (config bug, not a RAN failure)" >&2
  exit 2
fi

export GNB_CONFIG_PATH="${ROOT}/docker/configs/gnb_ofh_testmode.yml"
export P1_RU_EMU_CONFIG_PATH="${ROOT}/docker/configs/ru_emu.yml"
COMPOSE=(docker compose -f "${P0_DOCKER}/upstream/docker/docker-compose.yml" \
                       -f "${P0_DOCKER}/compose.sim.yml" \
                       -f "${ROOT}/docker/compose.p1.yml")

echo "== [3/7] compose up (5gc, gnb, ru-emu) =="
# --force-recreate (2026-07-25, found on the real GCP run): without it, `docker compose up` finds
# containers already running with unchanged config (e.g. left over from a manual test earlier in
# the same session) and does nothing -- meaning P1-R7's "NG Setup within 60s of bring-up" check
# ends up scanning an old, long-running container's log instead of a fresh one. At real traffic
# rates gnb's log grows fast enough (both from legitimate per-slot OFH activity and, separately, a
# real "missed incoming User-Plane uplink" warning flood -- see VERIFICATION.md) that NG-Setup
# evidence from an old startup can become impractically expensive to find, or sit behind enough
# volume that `docker logs` itself becomes slow. A fresh recreate every run is also just the
# correct semantics for "bring up the rig" -- a repeatable, known-clean start, not "whatever
# happens to already be running."
if ! "${COMPOSE[@]}" up -d --force-recreate 5gc gnb ru-emu; then
  echo "error: docker compose up failed" >&2
  exit 1
fi

echo "== [4/7] Stability hold (${HOLD}s) =="
sleep "$HOLD"
FAIL=0
for svc in 5gc gnb ru-emu; do
  CID="$("${COMPOSE[@]}" ps -q "$svc")"
  if [ -z "$CID" ]; then
    echo "error: service '$svc' has no running container" >&2
    FAIL=1
    continue
  fi
  RESTARTS="$(docker inspect "$CID" --format '{{.RestartCount}}')"
  STATE="$(docker inspect "$CID" --format '{{.State.Status}}')"
  echo "  ${svc}: state=${STATE} restarts=${RESTARTS}"
  if [ "$RESTARTS" != "0" ] || [ "$STATE" != "running" ]; then
    echo "error: ${svc} unstable (state=${STATE} restarts=${RESTARTS}); last 200 log lines:" >&2
    docker logs --tail 200 "$CID" >&2
    FAIL=1
  fi
done
[ "$FAIL" -ne 0 ] && exit 1

echo "== [5/7] gnb NG-setup-attempt log check =="
# Root cause found for real (2026-07-25, confirmed via direct reproduction on the GCP VM -- see
# soak_stability.sh for the full writeup): under `set -o pipefail` (this script's own set -uo
# pipefail at the top), `docker logs C | grep -q PATTERN` can return 141 (SIGPIPE) even when grep
# DOES match, because grep -q exits the instant it finds a match, closing its read end, and
# `docker logs` gets SIGPIPE'd before finishing; pipefail then reports that 141 instead of grep's
# real 0. NOT a Docker log driver buffering issue as first suspected -- deterministic once the log
# is large enough that the match line isn't near the tail, not a timing flake. Fixed for real:
# capture the log into a variable first (command substitution fully drains its child before
# returning -- no live pipe for grep to short-circuit), then grep the captured text. The retry loop
# itself is kept (15 attempts, 3s apart, also re-fetching the container ID each attempt in case a
# recreate raced this check) since NG-Setup genuinely can take a moment after container start, but
# the SIGPIPE fix is what actually matters here, not the retry count.
NG_SETUP_FOUND=0
for _ in $(seq 1 15); do
  GNB_CID="$("${COMPOSE[@]}" ps -q gnb)"
  if [ -n "$GNB_CID" ]; then
    GNB_LOG_SNAPSHOT="$(docker logs "$GNB_CID" 2>&1)"
    if grep -qiE 'ng[ _-]?setup|NGAP.*(request|initiat)' <<< "$GNB_LOG_SNAPSHOT"; then
      NG_SETUP_FOUND=1
      break
    fi
  fi
  sleep 3
done
if [ "$NG_SETUP_FOUND" -ne 1 ]; then
  echo "error: no NG-setup-attempt evidence in gnb logs after retries; last 200 lines:" >&2
  docker logs --tail 200 "$GNB_CID" >&2
  exit 1
fi

echo "== [6/7] eCPRI assertion (P1-R8, 30s capture) =="
PCAP_CAPTURE="/tmp/p1_${RUN_ID}.pcap"
if ! "${ROOT}/helpers/assert_ecpri.sh" --project "$COMPOSE_PROJECT_NAME" --seconds 30 --pcap-out "$PCAP_CAPTURE"; then
  echo "error: eCPRI assertion failed (missing C-plane/U-plane class -- see stdout for which)" >&2
  exit 1
fi

echo "== [7/7] 10-minute soak (P1-R9) + pcap archive (P1-R10) =="
if ! "${ROOT}/helpers/soak_stability.sh" --seconds "$SOAK_SECONDS"; then
  echo "error: soak_stability.sh failed" >&2
  exit 1
fi
"${ROOT}/helpers/archive_pcap.sh" --run-id "$RUN_ID" --pcap-in "$PCAP_CAPTURE"

echo "bring_up: PASS (P1-G1 + P1-G2 green, run-id=${RUN_ID})"
exit 0
