#!/usr/bin/env python3
"""ledger_build.py -- builds oi-p5-ledger/1 from discovered manifests + captured gate results.

Importable as a module (simtest_runner.py uses classify_gate/compute_overall/build_ledger
directly) and runnable standalone per the LLD's documented CLI:

  ledger_build.py --run-id ID --artifacts-dir DIR --manifests FILE.json --results FILE.json
    -> writes <artifacts-dir>/ledger.json
"""
import argparse
import json
import os
import sys


def last_json_line(stdout_path):
    """Returns (verdict_dict_or_None, is_valid). Only the LAST non-empty line counts (P5-R4)."""
    if not os.path.isfile(stdout_path):
        return None, False
    with open(stdout_path, "r", errors="replace") as f:
        lines = [l.strip() for l in f if l.strip()]
    if not lines:
        return None, False
    try:
        return json.loads(lines[-1]), True
    except (json.JSONDecodeError, ValueError):
        return None, False


def classify_gate(exit_code, timed_out, verdict_valid):
    """PASS/FAIL/ERROR/BLOCKED/TIMEOUT per LLD's error-handling table (unconditional rules)."""
    if timed_out:
        return "TIMEOUT"
    if not verdict_valid:
        # "Gate's last stdout line is not valid JSON -> status forced to ERROR regardless of
        # exit code" (LLD Error handling table) -- this includes the missing/non-executable
        # script case, whose exit_code is already 2.
        return "ERROR"
    if exit_code == 0:
        return "PASS"
    if exit_code == 1:
        return "FAIL"
    if exit_code == 3:
        return "BLOCKED"
    # exit_code == 2, or any other/unexpected code with a (coincidentally) valid JSON last line.
    return "ERROR"


def compute_overall(phases):
    """BLOCKED > FAIL/ERROR > INCOMPLETE > PASS -- unconditional precedence (LLD Data structures)."""
    all_statuses = [g["status"] for p in phases for g in p["gates"]]
    if "BLOCKED" in all_statuses:
        return "BLOCKED"
    if "FAIL" in all_statuses or "ERROR" in all_statuses:
        return "FAIL"
    if any(not p["discovered"] for p in phases):
        return "INCOMPLETE"
    return "PASS"


def build_ledger(run_id, started_utc, finished_utc, tier, host, pins_digest, rigcfg_digest,
                  phases):
    return {
        "schema": "oi-p5-ledger/1",
        "run_id": run_id,
        "started_utc": started_utc,
        "finished_utc": finished_utc,
        "tier": tier,
        "host": host,
        "pins_digest": pins_digest,
        "rigcfg_digest": rigcfg_digest,
        "phases": phases,
        "overall": compute_overall(phases),
        "performance_claims": [],
        "honesty_notes": [
            "SIM proves function and integration only -- no number above is a performance claim.",
            "Per-phase honesty-ledger caveats live in each feature's own SPEC.md; this ledger "
            "references, not duplicates, them.",
        ],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--artifacts-dir", required=True)
    ap.add_argument("--manifests", required=True, help="JSON file: discover_suites.py output")
    ap.add_argument("--results", required=True, help="JSON file: per-gate captured results")
    ap.add_argument("--tier", default="sim")
    ap.add_argument("--host-kind", default="wsl2", choices=["wsl2", "gcp"])
    ap.add_argument("--pins-digest", default="sha256:unknown")
    ap.add_argument("--rigcfg-digest", default="sha256:unknown")
    ap.add_argument("--started-utc", required=True)
    ap.add_argument("--finished-utc", required=True)
    args = ap.parse_args()

    with open(args.manifests) as f:
        manifests = json.load(f)
    with open(args.results) as f:
        results = json.load(f)

    results_by_key = {(r["phase"], r["gate_id"]): r for r in results}

    phases = []
    for m in manifests:
        if not m["discovered"]:
            phase_entry = {
                "phase": m["phase"], "feature": m["feature"], "discovered": False, "gates": [],
            }
            if "validation_error" in m:
                phase_entry["validation_error"] = m["validation_error"]
            phases.append(phase_entry)
            continue

        gates = []
        for g in m["gates"]:
            r = results_by_key.get((m["phase"], g["id"]))
            if r is None:
                # Declared but never invoked (e.g. runner aborted early) -- ERROR, not silently
                # dropped (P5-R14's "never silently omit" applies at gate level too).
                gates.append({
                    "id": g["id"], "type": g["type"], "status": "ERROR",
                    "exit_code": None, "duration_s": 0.0, "verdict": None,
                    "artifacts": "",
                })
                continue
            verdict, valid = last_json_line(os.path.join(r["artifacts_dir"], "stdout.log"))
            status = classify_gate(r["exit_code"], r.get("timed_out", False), valid)
            gates.append({
                "id": g["id"], "type": g["type"], "status": status,
                "exit_code": r["exit_code"], "duration_s": r["duration_s"],
                "verdict": verdict, "artifacts": r["artifacts_dir"],
            })
        phases.append({
            "phase": m["phase"], "feature": m["feature"], "discovered": True, "gates": gates,
        })

    host = {"kind": args.host_kind}
    ledger = build_ledger(args.run_id, args.started_utc, args.finished_utc, args.tier, host,
                          args.pins_digest, args.rigcfg_digest, phases)

    out_path = os.path.join(args.artifacts_dir, "ledger.json")
    os.makedirs(args.artifacts_dir, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(ledger, f, indent=2)
    print(out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
