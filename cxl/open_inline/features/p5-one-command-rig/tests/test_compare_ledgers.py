#!/usr/bin/env python3
"""test_compare_ledgers.py -- P5-R9 unit test: two ledgers differing only in host/timestamps/
run_id compare equal; two ledgers differing in any gate's status, overall, or a digest compare
unequal with a field-level diff."""
import json
import os
import shutil
import subprocess
import sys
import tempfile

FEATURE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPARE_SH = os.path.join(FEATURE_ROOT, "helpers", "compare_ledgers.sh")

g_fail = 0


def check(cond, what):
    global g_fail
    if cond:
        print(f"PASS: {what}")
    else:
        print(f"FAIL: {what}", file=sys.stderr)
        g_fail += 1


def base_ledger():
    return {
        "schema": "oi-p5-ledger/1", "run_id": "X", "started_utc": "t0", "finished_utc": "t1",
        "tier": "sim", "host": {"kind": "wsl2"}, "pins_digest": "sha256:aaa",
        "rigcfg_digest": "sha256:bbb",
        "phases": [
            {"phase": "p1", "feature": "p1-ran-baseline", "discovered": True,
             "gates": [{"id": "p1-g1", "type": "unit", "status": "PASS", "exit_code": 0,
                        "duration_s": 1.0, "verdict": {}, "artifacts": ""}]}
        ],
        "overall": "PASS", "performance_claims": [], "honesty_notes": [],
    }


def run_compare(a, b):
    return subprocess.run([COMPARE_SH, a, b], capture_output=True, text=True)


def main():
    tmp = tempfile.mkdtemp(prefix="p5_compare_test_")
    try:
        a = base_ledger()
        b = base_ledger()
        b["run_id"] = "Y"
        b["started_utc"] = "t0-different"
        b["finished_utc"] = "t1-different"
        b["host"] = {"kind": "gcp", "n2_standard_16": True}
        path_a = os.path.join(tmp, "a.json")
        path_b = os.path.join(tmp, "b.json")
        with open(path_a, "w") as f:
            json.dump(a, f)
        with open(path_b, "w") as f:
            json.dump(b, f)
        r = run_compare(path_a, path_b)
        check(r.returncode == 0, "identical except host/timestamps/run_id -> exit 0")

        c = base_ledger()
        c["phases"][0]["gates"][0]["status"] = "FAIL"
        c["overall"] = "FAIL"
        path_c = os.path.join(tmp, "c.json")
        with open(path_c, "w") as f:
            json.dump(c, f)
        r2 = run_compare(path_a, path_c)
        check(r2.returncode == 1, "differing gate status -> exit 1")
        check("p1-g1" in r2.stdout, "diff output names the differing gate")

        d = base_ledger()
        d["pins_digest"] = "sha256:different"
        path_d = os.path.join(tmp, "d.json")
        with open(path_d, "w") as f:
            json.dump(d, f)
        r3 = run_compare(path_a, path_d)
        check(r3.returncode == 1, "differing pins_digest -> exit 1 (NOT exempted)")
        check("pins_digest" in r3.stdout, "diff output names pins_digest")

        e = base_ledger()
        e["overall"] = "BLOCKED"
        path_e = os.path.join(tmp, "e.json")
        with open(path_e, "w") as f:
            json.dump(e, f)
        r4 = run_compare(path_a, path_e)
        check(r4.returncode == 1, "differing overall -> exit 1")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    total = g_fail
    print(f"\n{'PASS' if total == 0 else 'FAIL'}: test_compare_ledgers — {total} failure(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
