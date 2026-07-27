#!/usr/bin/env bash
# gate_p4_restart.sh — P4-G3 (P4-R8/R9). Builds + runs the real restart harness
# (tests/restart_test.c): (a) producer re-open against an existing ring -> epoch bump + consumer
# resync; (b) consumer re-open (simulated restart) -> resumes at the persisted tail, no dup/loss.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make build/restart_test
./build/restart_test
