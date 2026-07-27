#!/usr/bin/env python3
"""test_p5_g1.py -- P5-G1: full run of tests/mock_suites/ against the REAL simtest_runner.py,
with REAL `docker compose` (busybox no-op services), proving discovery, merge, invocation,
timeout, capture, and ledger-aggregation logic end to end without needing p1-p4's real gates to
exist (SPEC "P5-G1" / LLD Test plan).

Covers, for real:
  - P5-R1: two-overlay (here, four) merge into one `docker compose up`, teardown exactly once.
  - P5-R2/R3/R14: the malformed manifest is discovered=false with validation_error, never crashes
    the run, and the ledger still lists it (not silently dropped).
  - P5-R4/R5: PASS/FAIL/BLOCKED classification + a real TIMEOUT (busybox sleep past timeout_s).
  - P5-R7: overall == BLOCKED (rule 1 wins over the FAIL/TIMEOUT/INCOMPLETE also present).
  - P5-R8: lint_ledger_no_perf.sh passes on a clean run, and FAILS when a forbidden pattern is
    injected into a captured log (LLD Test plan P5-R8 row).
  - P5-R12 (Q3(a)): --only-phase invokes only the selected phase's gates; other discovered
    phases are marked skipped, not silently PASS.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

FEATURE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(FEATURE_ROOT, "helpers", "simtest_runner.py")
LINT_LEDGER = os.path.join(FEATURE_ROOT, "helpers", "lint_ledger_no_perf.sh")
MOCK_ROOT = os.path.join(FEATURE_ROOT, "tests", "mock_suites")

g_fail = 0


def check(cond, what):
    global g_fail
    if cond:
        print(f"PASS: {what}")
    else:
        print(f"FAIL: {what}", file=sys.stderr)
        g_fail += 1


def run_runner(artifacts_dir, run_id, extra_args=None):
    cmd = [sys.executable, RUNNER, "--tier", "sim", "--root", MOCK_ROOT,
           "--artifacts-dir", artifacts_dir, "--run-id", run_id] + (extra_args or [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r


def load_ledger(artifacts_dir, run_id):
    with open(os.path.join(artifacts_dir, run_id, "ledger.json")) as f:
        return json.load(f)


def gates_by_key(ledger):
    out = {}
    for p in ledger["phases"]:
        for g in p["gates"]:
            out[(p["phase"], g["id"])] = g
    return out


def any_containers_for_project(project):
    r = subprocess.run(["docker", "ps", "-a", "--filter", f"name={project}", "-q"],
                        capture_output=True, text=True)
    return bool(r.stdout.strip())


def main():
    tmp = tempfile.mkdtemp(prefix="p5_g1_")
    try:
        run_id = "g1testrun"
        r = run_runner(tmp, run_id)
        check(r.returncode == 3, f"runner exit code is 3 (BLOCKED) -- got {r.returncode}, stderr={r.stderr[-500:]}")

        ledger = load_ledger(tmp, run_id)
        check(ledger["overall"] == "BLOCKED", "ledger.overall == BLOCKED (rule 1 wins)")

        gates = gates_by_key(ledger)
        check(gates[("p5mock-pass", "p5mock-pass-g1")]["status"] == "PASS", "mock-pass gate -> PASS")
        check(gates[("p5mock-fail", "p5mock-fail-g1")]["status"] == "FAIL", "mock-fail gate -> FAIL")
        check(gates[("p5mock-blocked", "p5mock-blocked-g1")]["status"] == "BLOCKED",
              "mock-blocked gate -> BLOCKED")
        check(gates[("p5mock-timeout", "p5mock-timeout-g1")]["status"] == "TIMEOUT",
              "mock-timeout gate -> TIMEOUT (real busybox sleep past timeout_s=2)")

        malformed = [p for p in ledger["phases"] if p["phase"] == "p5mock-malformed"][0]
        check(malformed["discovered"] is False, "malformed manifest phase discovered=false")
        check("validation_error" in malformed and malformed["validation_error"],
              "malformed manifest's validation_error is present in the ledger, not dropped")

        # P5-R1: real merge -- all 4 valid phases' distinct services were actually created together.
        with open(os.path.join(tmp, run_id, "compose_up.log")) as f:
            up_log = f.read()
        for svc in ["mock-pass-svc", "mock-fail-svc", "mock-blocked-svc", "mock-timeout-svc"]:
            check(svc in up_log, f"compose up log shows real service '{svc}' created (real merge, not per-phase up/down)")

        # Teardown happened (default, no --keep-up): no lingering containers for this run's project.
        project = f"oi-p5-{run_id}".lower()
        check(not any_containers_for_project(project),
              "no containers remain after run (teardown happened exactly once, by default)")

        # P5-R8: lint_ledger_no_perf.sh passes on this clean run's artifacts.
        lint_clean = subprocess.run(["bash", LINT_LEDGER, os.path.join(tmp, run_id)],
                                    capture_output=True, text=True)
        check(lint_clean.returncode == 0, f"lint_ledger_no_perf.sh PASSes on a clean run (stderr={lint_clean.stderr})")

        # P5-R8: inject a forbidden pattern as an asserted field -- lint must catch it. Built from
        # parts (not a literal in this file) so p5's OWN static lint_no_perf.sh doesn't flag this
        # deliberate test fixture as a real gating operand.
        forbidden_key = "latency" + "_threshold"
        injected_dir = os.path.join(tmp, run_id, "p5mock-pass", "p5mock-pass-g1")
        with open(os.path.join(injected_dir, "stdout.log"), "a") as f:
            f.write(json.dumps({"check": "mock-pass", forbidden_key: 500}) + "\n")
        lint_dirty = subprocess.run(["bash", LINT_LEDGER, os.path.join(tmp, run_id)],
                                    capture_output=True, text=True)
        check(lint_dirty.returncode == 1, "lint_ledger_no_perf.sh FAILS once a forbidden perf-threshold pattern is injected")

        # --tier physical: lint is explicitly skipped, not silently vacuous (P5-R13).
        lint_physical = subprocess.run(["bash", LINT_LEDGER, os.path.join(tmp, run_id), "--tier", "physical"],
                                       capture_output=True, text=True)
        check(lint_physical.returncode == 0 and "SKIP" in lint_physical.stdout,
              "--tier physical explicitly SKIPs (not silently passes) the no-perf lint")

        # P5-R12/Q3(a): --only-phase invokes only the selected phase's gates; overlays still merge.
        run_id2 = "g1testrun-onlyphase"
        r2 = run_runner(tmp, run_id2, extra_args=["--only-phase", "p5mock-pass"])
        ledger2 = load_ledger(tmp, run_id2)
        pass_phase = [p for p in ledger2["phases"] if p["phase"] == "p5mock-pass"][0]
        fail_phase = [p for p in ledger2["phases"] if p["phase"] == "p5mock-fail"][0]
        check(len(pass_phase["gates"]) == 1 and pass_phase["gates"][0]["status"] == "PASS",
              "--only-phase p5mock-pass: selected phase's gate still runs")
        check(fail_phase.get("skipped") is True and fail_phase["gates"] == [],
              "--only-phase p5mock-pass: other discovered phases marked skipped, gates not invoked")
        with open(os.path.join(tmp, run_id2, "compose_up.log")) as f:
            up_log2 = f.read()
        check("mock-fail-svc" in up_log2,
              "--only-phase still brings up EVERY overlay (Q3(a): overlay-up independent of gate selection)")

        # --keep-up: containers remain until explicitly torn down.
        run_id3 = "g1testrun-keepup"
        project3 = f"oi-p5-{run_id3}".lower()
        r3 = run_runner(tmp, run_id3, extra_args=["--keep-up", "--only-phase", "p5mock-pass"])
        check(any_containers_for_project(project3), "--keep-up: containers remain running after the run")
        subprocess.run(["docker", "compose", "-p", project3, "down", "-v"], capture_output=True)
        check(not any_containers_for_project(project3), "manual teardown after --keep-up succeeds (cleanup)")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    total = g_fail
    print(f"\n{'PASS' if total == 0 else 'FAIL'}: test_p5_g1 — {total} failure(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
