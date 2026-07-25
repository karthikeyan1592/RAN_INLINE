#!/usr/bin/env bash
# smoke_up.sh — compose up + stability/log checks + SCTP precondition (P0-R9, P0-G2).
#
# usage: smoke_up.sh [--hold-seconds 60]
# Exit 0 iff: SCTP precheck passes; compose up succeeds; gnb+5gc running with 0 restarts for
# hold window; gnb log matched NG-setup-attempt pattern.
# Exit 3 = SCTP precondition failed (distinct, actionable).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKER_DIR="${ROOT}/docker"
HOLD=60

while [ $# -gt 0 ]; do
  case "$1" in
    --hold-seconds) HOLD="$2"; shift 2 ;;
    *) echo "usage: smoke_up.sh [--hold-seconds N]" >&2; exit 2 ;;
  esac
done

echo "== [1/4] SCTP precondition =="
if ! "$(dirname "${BASH_SOURCE[0]}")/check_sctp.sh"; then
  exit 3
fi

COMPOSE=(docker compose -f "${DOCKER_DIR}/upstream/docker/docker-compose.yml" -f "${DOCKER_DIR}/compose.sim.yml")

echo "== [2/4] compose up (5gc, gnb) =="
if ! "${COMPOSE[@]}" up -d 5gc gnb; then
  echo "error: docker compose up failed" >&2
  exit 1
fi

echo "== [3/4] Stability hold (${HOLD}s) =="
sleep "$HOLD"

FAIL=0
for svc in 5gc gnb; do
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

if [ "$FAIL" -ne 0 ]; then
  exit 1
fi

echo "== [4/4] gnb NG-setup-attempt log check =="
GNB_CID="$("${COMPOSE[@]}" ps -q gnb)"
# Capture-then-grep, not a live pipe (2026-07-25, ported from the p1-ran-baseline fix): under this
# script's own `set -o pipefail`, `docker logs C | grep -q PATTERN` can return 141 (SIGPIPE) even
# when grep DOES match -- grep -q exits the instant it finds a match, closing its read end, and
# `docker logs` gets SIGPIPE'd before finishing; pipefail then reports that 141 instead of grep's
# real 0. Deterministic once the log is large enough that the match line isn't near the tail, not a
# timing flake -- confirmed by direct reproduction on a GCP VM while bringing up p1-ran-baseline's
# rig (same gnb image/log shape). Untested here specifically (p0 has always been SCTP-blocked on
# every host used so far, so this line has never actually executed), fixed proactively since it's
# the identical bug pattern.
GNB_LOG_SNAPSHOT="$(docker logs "$GNB_CID" 2>&1)"
if ! grep -qiE 'ng[ _-]?setup|NGAP.*(request|initiat)' <<< "$GNB_LOG_SNAPSHOT"; then
  echo "error: no NG-setup-attempt evidence in gnb logs; last 200 lines:" >&2
  docker logs --tail 200 "$GNB_CID" >&2
  exit 1
fi

echo "smoke_up: PASS (5gc+gnb stable ${HOLD}s, 0 restarts, NG setup attempted)"
exit 0
