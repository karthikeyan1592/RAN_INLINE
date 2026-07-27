#!/usr/bin/env bash
# gate_p4_wrap.sh — P4-G2 (P4-R7). Builds + runs the real wrap harness (tests/wrap_test.c): a
# real producer thread hammering a small (capacity=4) real ring while the main thread pauses as
# the consumer, asserting the producer genuinely blocks (no overwrite/drop) then drains correctly.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make build/wrap_test
./build/wrap_test
