#!/usr/bin/env python3
"""pipeline_test.py -- P2-R1/P2-R15/P2-R16/P2-R17 growing-pipeline integration gate.

Drives the REAL oi_p2_host pipeline (all 7 real kernels + LDPC + CPU tail, wired 2026-07-23 --
see p2a-scaffold/src/host/oi_p2_host.cpp and VERIFICATION.md) via the compiled `pipeline_runner`
binary, which stands in for a production ingest_backend (p3/p6): it only ever calls
oi_p2_setup/write_arena/feed/launch_slot/drain/tap/teardown (P2-R17's frozen surface, now
including the 2026-07-23 oi_p2_write_arena addition). This script owns the pass/fail judgment;
pipeline_runner itself is judgment-free (it just runs the chain and reports what happened).

Two pcap classes per P2-R15:
  (a) P1-captured -- protocol-real, data-synthetic (ru_emulator static IQ, DEV-044: no TB ground
      truth exists for these). Gate: structural only (pipeline runs to completion, taps readable).
      RUNS FOR REAL as of 2026-07-25: p1-ran-baseline reached a fully green P1-G1/P1-G2 deploy on
      GCP and archived a real corpus (see P1_PCAP_ROOT below) -- the class-(a) gate's documented
      "add once p1's pcaps exist" deferral (p2f-integration/README.md) is now resolved. Only
      SKIPPED (not failed) if no run-id subdirectory exists under artifacts/p1/pcaps/ yet.
  (b) oracle-packed -- pcap_packer.py (which itself wraps oracle_tx_gen.cpp, a real, self-
      verified OCUDU TX-chain oracle) builds a real, wire-valid transmission with a KNOWN ground
      truth TB. Gate: CRC24A pass + decoded TB bit-exact vs the packer's own oracle TB, for all
      three MVP MCS points. This is the actual pass/fail decode gate P2-R15 calls out as the one
      that matters; run below as REQUIRED (not skippable).

P2-R1 (prefix buildability) scoping note: p2a's original stub-chain design let each pipeline
PREFIX (K1-only, K1-K2, ...) build/test independently against dummy downstream stages. The
2026-07-23 stub-replacement (user-directed, see VERIFICATION.md) wires all 7 real kernels into one
oi_p2_setup call, so there is no longer a separate "K1-only" build artifact to test in isolation --
this is a real, disclosed scope evolution (same class as the 7->8 stage bump p2d already went
through), not a silent gap. What P2-R1's OBSERVABLE contract asked for -- each intermediate
buffer (I2..I5) inspectable independent of the final result -- is still checked below via
pipeline_runner's taps_ok field, over REAL oracle-packed data (a strictly stronger check than the
original dummy-sink version, since these taps must hold real, meaningful values, not placeholders).

P2-R16 (no perf thresholds): this file contains no timing/rate value used as a pass/fail check;
verified by lint_no_perf.sh, which already scans p2*/tests/** and will catch any future violation
introduced here.

P2-R17 (frozen API surface): checked below by grepping pipeline_runner.cpp's own source for which
oi_p2_* calls it makes, asserting the set is exactly the documented public surface -- the honest,
buildable substitute for the LLD's original "p3/p4 stub links against oi_p2_host.h unchanged" test,
since neither p3 nor p4 exist yet to literally build stubs against.
"""
import json
import os
import re
import subprocess
import sys

FEATURE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # p2f-integration/
P2A_ROOT = os.path.abspath(os.path.join(FEATURE_ROOT, "..", "p2a-scaffold"))
PIPELINE_RUNNER = os.path.join(FEATURE_ROOT, "build", "pipeline_runner")
CONFIG_YAML = os.path.join(P2A_ROOT, "tests", "fixtures", "mvp_config.yaml")
ORACLE_PCAP_DIR = os.path.join(FEATURE_ROOT, "tests", "fixtures", "oracle_pcaps")
# 2026-07-25 real path fix: this used to point at p1-ran-baseline/captures/, a location
# p1-ran-baseline's own tooling (archive_pcap.sh) never actually writes to -- two sessions each
# picked a reasonable-sounding path independently and nobody reconciled them until this gate was
# actually run against real P1 output. archive_pcap.sh's real, manifest-carrying corpus root is
# artifacts/p1/pcaps/<run-id>/ (see p1-ran-baseline/VERIFICATION.md) -- pointing here instead of
# duplicating captures into the feature tree (which would bloat it with hundreds of MB of binary
# pcap data per run) matches how this project already treats generated artifacts elsewhere.
P1_PCAP_ROOT = os.path.abspath(os.path.join(FEATURE_ROOT, "..", "..", "artifacts", "p1", "pcaps"))

sys.path.insert(0, os.path.join(FEATURE_ROOT, "helpers"))
from pcap_packer import pack as pack_oracle  # noqa: E402

g_fail = 0


def check(cond, what):
    global g_fail
    if cond:
        print(f"PASS: {what}")
    else:
        print(f"FAIL: {what}", file=sys.stderr)
        g_fail += 1


def ensure_pipeline_runner_built():
    if not os.path.isfile(PIPELINE_RUNNER):
        subprocess.run(["make", "build/pipeline_runner"], cwd=FEATURE_ROOT, check=True)


def run_pipeline(pcap_path, mcs_index):
    """Invokes pipeline_runner with CWD = p2a-scaffold/tests/ (its own kernel .cl source paths are
    CWD-relative -- see oi_p2_host.cpp's own documented fragility, VERIFICATION.md). Returns the
    parsed JSON result dict, or None + prints stderr if the runner itself errored out.
    """
    result = subprocess.run(
        [PIPELINE_RUNNER, CONFIG_YAML, pcap_path, str(mcs_index)],
        cwd=os.path.join(P2A_ROOT, "tests"),
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(f"  pipeline_runner stderr: {result.stderr}", file=sys.stderr)
        return None
    return json.loads(result.stdout)


def class_b_oracle_packed():
    """P2-R15's actual pass/fail decode gate: real, self-verified oracle transmissions, CRC24A
    pass + TB bit-exact required for all three MVP MCS points."""
    for mcs_index in (4, 13, 21):
        pcap_path, _json_path, sidecar = pack_oracle(mcs_index, seed=mcs_index * 1000 + 7,
                                                      out_dir=ORACLE_PCAP_DIR)
        result = run_pipeline(pcap_path, mcs_index)
        label = f"class-b MCS{mcs_index}"
        check(result is not None, f"{label}: pipeline_runner completes without error")
        if result is None:
            continue
        check(result["crc24a_ok"] == 1, f"{label}: CRC24A pass")
        check(result["nof_cb"] == sidecar["nof_cb"], f"{label}: nof_cb matches oracle ({sidecar['nof_cb']})")
        check(result["base_graph"] == sidecar["base_graph"],
              f"{label}: base_graph matches oracle (BG{sidecar['base_graph']})")
        check(result["tb_data_hex"] == sidecar["tb_bytes_hex"], f"{label}: decoded TB bit-exact vs oracle TB")
        check(result["nof_frames_dropped"] == 0, f"{label}: all 14 real wire frames parsed, none dropped")
        check(result["taps_ok"], f"{label}: I2-I5 taps readable (P2-R1 observable contract)")


def class_a_p1_captured():
    """P2-R15's structural-only gate over P1-captured pcaps. Skipped (not failed) only if no real
    P1 corpus exists yet under artifacts/p1/pcaps/ -- p1-ran-baseline is now implemented and has
    produced a real corpus (2026-07-25, GCP confirmation run), so this normally runs for real.

    Uses the LATEST run-id subdirectory (lexical sort == chronological, since archive_pcap.sh's
    run-ids are UTC timestamps) rather than every historical run -- structural-only checks add no
    value re-run against stale corpora, and this avoids O(runs) growth in test time as more P1
    deploys accumulate archives over time.

    Picks up every rotated pcap fragment (archive_pcap.sh's size-based -C rotation produces
    `fronthaul.pcap`, `fronthaul.pcap1`, `fronthaul.pcap2`, ... for large captures -- see
    p1-ran-baseline/VERIFICATION.md), not just files literally ending in `.pcap`; each fragment is
    independently structurally valid and checked on its own, no reassembly needed for a
    completes-and-taps-readable check."""
    if not os.path.isdir(P1_PCAP_ROOT) or not os.listdir(P1_PCAP_ROOT):
        print(f"SKIP: class-a P1-captured pcaps ({P1_PCAP_ROOT} absent/empty -- "
              f"no P1 corpus archived yet, see p1-ran-baseline/VERIFICATION.md)")
        return
    run_ids = sorted(d for d in os.listdir(P1_PCAP_ROOT) if os.path.isdir(os.path.join(P1_PCAP_ROOT, d)))
    if not run_ids:
        print(f"SKIP: class-a P1-captured pcaps ({P1_PCAP_ROOT} has no run-id subdirectories)")
        return
    run_dir = os.path.join(P1_PCAP_ROOT, run_ids[-1])
    # Structural gate: pipeline must run to completion and its taps must be readable; no CRC/TB
    # assertion is made (DEV-044: ru_emulator's static IQ has no ground truth -- this pre-p3 rig's
    # corpus is protocol-real, data-synthetic by design, never a correctness oracle).
    for fname in sorted(os.listdir(run_dir)):
        if ".pcap" not in fname:
            continue
        pcap_path = os.path.join(run_dir, fname)
        result = run_pipeline(pcap_path, mcs_index=4)  # MCS choice is arbitrary for structural-only
        check(result is not None, f"class-a {run_ids[-1]}/{fname}: pipeline runs to completion")
        if result is not None:
            check(result["taps_ok"], f"class-a {run_ids[-1]}/{fname}: I2-I5 taps readable")


def check_p2_r17_api_surface():
    """Honest substitute for the LLD's literal "p3/p4 stub links against oi_p2_host.h unchanged"
    test (neither exists yet): asserts pipeline_runner.cpp -- itself standing in for a production
    ingest_backend -- calls only the documented, frozen oi_p2_host.h public surface, including the
    2026-07-23 oi_p2_write_arena addition (P2-R17 protects EXISTING callers from a signature
    change after they depend on it; it does not forbid additive new calls before anyone does --
    same reasoning already applied to the mcs_index addition, see VERIFICATION.md)."""
    runner_src = os.path.join(FEATURE_ROOT, "tools", "pipeline_runner.cpp")
    with open(runner_src) as f:
        src = f.read()
    calls = set(re.findall(r"\b(oi_p2_[a-z_]+)\s*\(", src))
    allowed = {"oi_p2_setup", "oi_p2_write_arena", "oi_p2_feed", "oi_p2_launch_slot", "oi_p2_drain",
              "oi_p2_tap", "oi_p2_teardown"}
    unexpected = calls - allowed
    check(not unexpected, f"P2-R17: pipeline_runner.cpp calls only the frozen oi_p2_host.h surface "
                          f"(found: {sorted(calls)})")
    check(len(calls) >= 5, "P2-R17: pipeline_runner.cpp exercises a real, non-trivial slice of the API")


def main():
    ensure_pipeline_runner_built()
    class_b_oracle_packed()
    class_a_p1_captured()
    check_p2_r17_api_surface()

    print()
    if g_fail == 0:
        print("pipeline_test: ALL PASS")
        return 0
    print(f"pipeline_test: {g_fail} FAILURE(S) ABOVE")
    return 1


if __name__ == "__main__":
    sys.exit(main())
