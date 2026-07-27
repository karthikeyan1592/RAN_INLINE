#!/usr/bin/env bash
# compare_ledgers.sh -- P5-R9: diff two oi-p5-ledger/1 JSON files, ignoring host-identity and
# timestamp fields (host, started_utc, finished_utc, run_id). pins_digest and rigcfg_digest ARE
# compared for equality -- both hosts must run the same pins and the same merged rig config
# (LLD's Public APIs CLI contract note).
#
# exit 0 iff every gate's status and the overall verdict match (plus pins/rigcfg digests);
# exit 1 with a field-level diff printed otherwise.
set -uo pipefail

LEDGER_A="${1:?usage: compare_ledgers.sh LEDGER_A.json LEDGER_B.json}"
LEDGER_B="${2:?usage: compare_ledgers.sh LEDGER_A.json LEDGER_B.json}"

python3 - "$LEDGER_A" "$LEDGER_B" <<'PYEOF'
import json
import sys

path_a, path_b = sys.argv[1], sys.argv[2]
with open(path_a) as f:
    a = json.load(f)
with open(path_b) as f:
    b = json.load(f)

diffs = []

if a.get("overall") != b.get("overall"):
    diffs.append(f"overall: {a.get('overall')!r} != {b.get('overall')!r}")

if a.get("pins_digest") != b.get("pins_digest"):
    diffs.append(f"pins_digest: {a.get('pins_digest')!r} != {b.get('pins_digest')!r}")

if a.get("rigcfg_digest") != b.get("rigcfg_digest"):
    diffs.append(f"rigcfg_digest: {a.get('rigcfg_digest')!r} != {b.get('rigcfg_digest')!r}")

gates_a = {(p["phase"], g["id"]): g["status"]
           for p in a.get("phases", []) for g in p.get("gates", [])}
gates_b = {(p["phase"], g["id"]): g["status"]
           for p in b.get("phases", []) for g in p.get("gates", [])}

for key in sorted(set(gates_a) | set(gates_b)):
    sa = gates_a.get(key, "<absent>")
    sb = gates_b.get(key, "<absent>")
    if sa != sb:
        diffs.append(f"gate {key}: {sa!r} != {sb!r}")

phases_a = {p["phase"]: p.get("discovered") for p in a.get("phases", [])}
phases_b = {p["phase"]: p.get("discovered") for p in b.get("phases", [])}
for key in sorted(set(phases_a) | set(phases_b)):
    da = phases_a.get(key, "<absent>")
    db = phases_b.get(key, "<absent>")
    if da != db:
        diffs.append(f"phase {key} discovered: {da!r} != {db!r}")

if diffs:
    print("FAIL: ledgers differ:")
    for d in diffs:
        print(f"  {d}")
    sys.exit(1)

print("PASS: compare_ledgers — ledgers agree on overall, pins/rigcfg digests, and every gate status")
sys.exit(0)
PYEOF
