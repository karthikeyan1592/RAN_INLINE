#!/usr/bin/env python3
"""test_ledger_build.py -- P5-R4/R7/R14 unit tests: gate classification and the unconditional
overall-derivation precedence (BLOCKED > FAIL/ERROR > INCOMPLETE > PASS), plus ledger.schema.json
validation of a constructed ledger."""
import json
import os
import sys

import jsonschema

FEATURE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(FEATURE_ROOT, "helpers"))
import ledger_build  # noqa: E402

g_fail = 0


def check(cond, what):
    global g_fail
    if cond:
        print(f"PASS: {what}")
    else:
        print(f"FAIL: {what}", file=sys.stderr)
        g_fail += 1


def gate(status):
    return {"id": "g", "type": "unit", "status": status, "exit_code": 0, "duration_s": 0.1,
            "verdict": {}, "artifacts": ""}


def phase(discovered, statuses):
    return {"phase": "p", "feature": "f", "discovered": discovered,
            "gates": [gate(s) for s in statuses]}


def main():
    # classify_gate
    check(ledger_build.classify_gate(0, False, True) == "PASS", "exit 0 + valid JSON -> PASS")
    check(ledger_build.classify_gate(1, False, True) == "FAIL", "exit 1 + valid JSON -> FAIL")
    check(ledger_build.classify_gate(2, False, True) == "ERROR", "exit 2 + valid JSON -> ERROR")
    check(ledger_build.classify_gate(3, False, True) == "BLOCKED", "exit 3 + valid JSON -> BLOCKED")
    check(ledger_build.classify_gate(0, True, True) == "TIMEOUT", "timed_out overrides exit 0 -> TIMEOUT")
    check(ledger_build.classify_gate(0, False, False) == "ERROR",
          "exit 0 but invalid/missing JSON last line -> forced ERROR (LLD error table)")
    check(ledger_build.classify_gate(3, False, False) == "ERROR",
          "exit 3 but invalid JSON last line -> forced ERROR, not BLOCKED")
    check(ledger_build.classify_gate(99, False, True) == "ERROR", "unexpected exit code -> ERROR")

    # compute_overall precedence: BLOCKED > FAIL/ERROR > INCOMPLETE > PASS, unconditional
    check(ledger_build.compute_overall([phase(True, ["PASS", "PASS"])]) == "PASS",
          "all PASS, all discovered -> overall PASS")
    check(ledger_build.compute_overall([phase(True, ["PASS", "FAIL"])]) == "FAIL",
          "any FAIL -> overall FAIL")
    check(ledger_build.compute_overall([phase(True, ["PASS", "ERROR"])]) == "FAIL",
          "any ERROR (no FAIL) -> overall FAIL (ERROR folds into FAIL bucket)")
    check(ledger_build.compute_overall([phase(True, ["PASS"]), phase(False, [])]) == "INCOMPLETE",
          "an undiscovered phase with no BLOCKED/FAIL/ERROR anywhere -> INCOMPLETE")
    check(ledger_build.compute_overall([phase(True, ["BLOCKED"]), phase(False, [])]) == "BLOCKED",
          "BLOCKED wins over an undiscovered phase too (rule 1, no exceptions)")
    check(ledger_build.compute_overall(
        [phase(True, ["BLOCKED"]), phase(True, ["ERROR", "ERROR"]), phase(True, ["FAIL"])]
    ) == "BLOCKED", "BLOCKED wins even with FAIL and ERROR also present elsewhere")
    check(ledger_build.compute_overall(
        [phase(True, ["FAIL"]), phase(False, [])]
    ) == "FAIL", "FAIL wins over INCOMPLETE (rule 2 before rule 3)")
    check(ledger_build.compute_overall([]) == "PASS",
          "zero phases (degenerate) -> PASS (vacuously, no gate/phase violates anything)")

    # ledger.schema.json validation of a constructed, realistic ledger
    ledger = ledger_build.build_ledger(
        "20260725T120000Z-abc1234", "2026-07-25T12:00:00Z", "2026-07-25T12:05:00Z", "sim",
        {"kind": "wsl2"}, "sha256:deadbeef", "sha256:cafef00d",
        [phase(True, ["PASS", "PASS"]), phase(False, [])],
    )
    with open(os.path.join(FEATURE_ROOT, "schemas", "ledger.schema.json")) as f:
        schema = json.load(f)
    errors = list(jsonschema.Draft7Validator(schema).iter_errors(ledger))
    check(errors == [], f"constructed ledger validates against ledger.schema.json (errors: {errors})")
    check(ledger["overall"] == "INCOMPLETE", "constructed ledger's own overall is INCOMPLETE as expected")
    check(ledger["performance_claims"] == [], "performance_claims always empty at SIM tier (P5-R8)")

    total = g_fail
    print(f"\n{'PASS' if total == 0 else 'FAIL'}: test_ledger_build — {total} failure(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
