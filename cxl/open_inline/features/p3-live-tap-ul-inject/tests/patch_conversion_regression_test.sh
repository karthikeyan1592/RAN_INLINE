#!/usr/bin/env bash
# patch_conversion_regression_test.sh — regression test for the real bug found+fixed 2026-07-26,
# live on GCP: the patch added `oracle_injection` to BOTH `ru_emulator_ofh_appconfig` (the CLI11-
# parsed struct, already covered by patch_schema_regression_test.cpp) AND `ru_emulator_config`
# (the struct the ru_emulator class constructor actually reads) -- but the ORIGINAL patch never
# added the corresponding copy line in main()'s manual field-by-field conversion between the two.
# Every OTHER field (nof_prb, compr_params, vlan_tag, ru_mac, du_mac, timing_params, dl_eaxc,
# ul_eaxc, prach_eaxc, prach_format) was copied; oracle_injection silently was not, so a real,
# valid, `enabled: true` config never actually reached the runtime logic that reads it -- config
# parsing (13/13 in patch_schema_regression_test.cpp) and file presence (verified separately) both
# looked correct, but injection never happened. Found via, in order: (1) confirming the patched
# binary+image were genuinely running, (2) confirming the real config was genuinely loaded with
# the right values, (3) confirming the .osg files were genuinely present+valid, (4) a live
# wire-truth pcap_comparator run showing stock/random IQ instead of oracle content, (5) a
# corrupted-file negative control that should have made the process refuse to start (P3-R3/D4)
# but instead booted clean -- proving the loader code path (and therefore the whole
# `if (cfg.oracle_injection.enabled)` block) never executes at runtime despite a correct config,
# which is only possible if `emu_cfg.oracle_injection.enabled` is false at that point regardless
# of what was parsed -- traced to the missing conversion line.
#
# `ru_emulator_config` is defined inside ru_emulator.cpp itself (not exported via any header), and
# main()'s conversion block is inline application glue, not a separable unit -- unlike the CLI11
# schema (patch_schema_regression_test.cpp), this specific code is not practically unit-testable
# without a disproportionate upstream refactor this patch deliberately avoids (minimal upstream
# footprint, matching this project's own established discipline). This test instead applies the
# REAL patch to a REAL pristine copy of the pinned OCUDU checkout and directly inspects the
# resulting real source for the fix -- a structural regression test, not a runtime one; the real
# runtime/functional proof is the live wire-truth check (pcap_comparator), which needs the live
# rig and is documented separately (DEFERRED_LIVE_GATES.md / VERIFICATION.md).
set -uo pipefail

FEATURE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OCUDU_SRC="$(cd "${FEATURE_ROOT}/../../../third_party/ocudu" && pwd)"
PATCH="${FEATURE_ROOT}/patches/0001-oracle-grid-ul-injection.patch"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cp -r "$OCUDU_SRC" "$TMPDIR/ocudu"
cd "$TMPDIR/ocudu"
if ! git apply "$PATCH" 2>&1; then
  echo "FAIL: patch failed to apply to a fresh pristine copy" >&2
  exit 1
fi
echo "PASS: patch applies cleanly to a fresh pristine copy"

# Real, pristine checkout must remain untouched by this test (only the temp copy was patched).
if [ -n "$(git -C "$OCUDU_SRC" status --short)" ]; then
  echo "FAIL: this test left the real pristine third_party/ocudu checkout dirty" >&2
  exit 1
fi
echo "PASS: real pristine third_party/ocudu checkout untouched by this test"

RU_EMULATOR_CPP="apps/examples/ofh/ru_emulator.cpp"

# The exact fix: the conversion line must exist, and it must appear AFTER prach_format's own copy
# line and BEFORE the ru_emulators.push_back call that actually constructs the ru_emulator object
# -- i.e. inside main()'s per-cell conversion block, not just anywhere in the file.
PRACH_LINE="$(grep -n "emu_cfg.prach_format[[:space:]]*=[[:space:]]*ru_cfg.prach_format;" "$RU_EMULATOR_CPP" | head -1 | cut -d: -f1)"
ORACLE_LINE="$(grep -n "emu_cfg.oracle_injection[[:space:]]*=[[:space:]]*ru_cfg.oracle_injection;" "$RU_EMULATOR_CPP" | head -1 | cut -d: -f1)"
PUSH_LINE="$(grep -n "ru_emulators.push_back(std::make_unique<ru_emulator>(" "$RU_EMULATOR_CPP" | head -1 | cut -d: -f1)"

if [ -z "$PRACH_LINE" ]; then
  echo "FAIL: could not even find the prach_format conversion line -- patch context has drifted" >&2
  exit 1
fi
echo "PASS: found emu_cfg.prach_format's own conversion line at ${RU_EMULATOR_CPP}:${PRACH_LINE}"

if [ -z "$ORACLE_LINE" ]; then
  echo "FAIL: emu_cfg.oracle_injection = ru_cfg.oracle_injection; is MISSING from the patched" \
       "ru_emulator.cpp -- this is the exact real bug found live on GCP 2026-07-26, regressed" >&2
  exit 1
fi
echo "PASS: found emu_cfg.oracle_injection's own conversion line at ${RU_EMULATOR_CPP}:${ORACLE_LINE}"

if [ -z "$PUSH_LINE" ]; then
  echo "FAIL: could not find the ru_emulators.push_back call -- patch context has drifted" >&2
  exit 1
fi

if [ "$ORACLE_LINE" -le "$PRACH_LINE" ] || [ "$ORACLE_LINE" -ge "$PUSH_LINE" ]; then
  echo "FAIL: emu_cfg.oracle_injection's conversion line exists but is NOT between prach_format's" \
       "own conversion line ($PRACH_LINE) and the push_back call ($PUSH_LINE) -- it must be inside" \
       "the same per-cell conversion block to actually take effect before emu_cfg is used" >&2
  exit 1
fi
echo "PASS: emu_cfg.oracle_injection's conversion line is correctly positioned inside the per-cell conversion block (between line ${PRACH_LINE} and line ${PUSH_LINE})"

echo ""
echo "patch_conversion_regression_test: ALL PASS"
exit 0
