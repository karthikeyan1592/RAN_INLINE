#!/usr/bin/env bash
# ci_p0.sh — P0-R8 CI job: build all images from scratch -> P0-G1 (clinfo + LDPC suite) ->
# report. Required-green. Contains NO timing/perf threshold of any kind (P0-R8).
#
# This script is the CI entrypoint; it wraps the helpers, it does not reimplement them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HELPERS="${ROOT}/helpers"

echo "############################################"
echo "# P0 CI: build"
echo "############################################"
"${HELPERS}/build_images.sh" "${ROOT}/pins.json"

echo "############################################"
echo "# P0 CI: P0-G1 -- clinfo assertion (P0-R4)"
echo "############################################"
docker run --rm -e OI_CL_PLATFORM=pocl -v "${HELPERS}:/helpers:ro" \
  oi/gpu-phy:dev bash /helpers/assert_clinfo.sh

echo "############################################"
echo "# P0 CI: P0-G1 -- LDPC suite (P0-R6)"
echo "############################################"
docker run --rm -e OI_CL_PLATFORM=pocl -e OI_LOG_DIR=/oi/logs -v "${HELPERS}:/helpers:ro" \
  oi/gpu-phy:dev bash /helpers/run_ldpc_suite.sh

echo "############################################"
echo "# P0 CI: P0-R7 -- pins schema + label parity"
echo "############################################"
python3 "${ROOT}/tests/check_pins_schema.py" "${ROOT}/pins.json"

echo "############################################"
echo "# P0 CI: negative test -- P0-R4 bogus platform"
echo "############################################"
set +e
docker run --rm -e OI_CL_PLATFORM=bogus -v "${HELPERS}:/helpers:ro" \
  oi/gpu-phy:dev bash /helpers/assert_clinfo.sh
NEG_STATUS=$?
set -e
if [ "$NEG_STATUS" -eq 0 ]; then
  echo "error: assert_clinfo.sh with OI_CL_PLATFORM=bogus unexpectedly exited 0" >&2
  exit 1
fi
echo "  negative test correctly failed (exit ${NEG_STATUS})"

echo "############################################"
echo "# P0 CI: ALL GREEN"
echo "############################################"
