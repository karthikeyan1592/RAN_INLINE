#!/usr/bin/env bash
# run_assert_ecpri.sh -- IF-P5-SUITE addendum: resolves the archived-corpus --pcap-in path
# relative to this feature's own root (CWD-independent), then execs the real, unmodified
# helpers/assert_ecpri.sh (SPEC "Out of scope": no modification of that script itself).
set -uo pipefail

FEATURE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PCAP="${FEATURE_ROOT}/../../artifacts/p1/pcaps/20260725T180323Z/fronthaul.pcap"

exec "${FEATURE_ROOT}/helpers/assert_ecpri.sh" --pcap-in "$PCAP"
