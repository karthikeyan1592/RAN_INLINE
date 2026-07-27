#!/usr/bin/env bash
# gate_p4_ordering.sh — P4-G1 (P4-R6). Builds + runs the real synthetic out-of-order harness
# (tests/ordering_test.c): real ring, real reserve/publish/wait_status/release, 2 (rnti,harq_id)
# keys deliberately completing out of order across keys, per-key monotonicity asserted to hold.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make build/ordering_test
./build/ordering_test
