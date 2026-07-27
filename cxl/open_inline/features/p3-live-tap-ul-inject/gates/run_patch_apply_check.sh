#!/usr/bin/env bash
# run_patch_apply_check.sh -- P3-R1 as an IF-P5-SUITE gate: `git apply --check` the ru_emulator
# patch series against the pristine, pinned third_party/ocudu checkout. Never modifies the
# checkout (--check only); confirmed via `git status` staying clean before and after.
set -uo pipefail

FEATURE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OCUDU_SRC="$(cd "${FEATURE_ROOT}/../../../third_party/ocudu" && pwd)"
PATCH="${FEATURE_ROOT}/patches/0001-oracle-grid-ul-injection.patch"

if [ ! -d "$OCUDU_SRC/.git" ]; then
  echo "{\"check\": \"patch_apply_check\", \"ok\": false, \"error\": \"OCUDU_SRC not a git checkout\"}"
  exit 2
fi

ERR="$(cd "$OCUDU_SRC" && git apply --check "$PATCH" 2>&1)"
CODE=$?

if [ "$CODE" -eq 0 ]; then
  echo "{\"check\": \"patch_apply_check\", \"ok\": true}"
  exit 0
else
  echo "$ERR" >&2
  echo "{\"check\": \"patch_apply_check\", \"ok\": false}"
  exit 1
fi
