#!/usr/bin/env bash
# gate_p4_integration.sh — P4-G4: full p3-injected-UL -> p2-pipeline -> ring -> l2-stub run.
# DEFERRED: needs the live rig (p3's patched ru-emu + real gnb + p2 pipeline), which this host
# cannot run (no SCTP). See ../../../DEFERRED_LIVE_GATES.md's p4 section for the exact command
# and pass criteria once run on the GCP VM. This script is written now (real, not a stub) so the
# GCP session has a single command to invoke rather than assembling one from scratch under time
# pressure -- but it CANNOT be exercised locally, and this repo's own discipline is to never claim
# a gate passed without actually running it, so this file is intentionally not invoked by `make
# test` or any local regression sweep.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

RING_PATH="${OI_SEAM_RING_PATH:-/oi/seam/ring.bin}"
RING_CAPACITY="${OI_SEAM_RING_CAPACITY:-64}"
CONSUMER_STATE_PATH="${OI_SEAM_CONSUMER_STATE_PATH:-/oi/seam/consumer_state.json}"
MIN_SLOTS="${OI_P4_MIN_SLOTS:-1000}"  # mirrors P3-R11's own >=1000-slot default (P4-G4 wording)

echo "== P4-G4: this expects gpu-phy (with p3's producer wired into its drain loop) and l2-stub" >&2
echo "   already running via compose.p4.yml layered on p1+p3's own compose files. ==" >&2
echo "== Running l2-stub's own verdict check directly (same binary the l2-stub container runs): ==" >&2

"${ROOT}/build/l2_stub_main" "$RING_PATH" "$RING_CAPACITY" "$CONSUMER_STATE_PATH" --max-slots "$MIN_SLOTS"
