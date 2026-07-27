#!/usr/bin/env python3
"""discover_suites.py -- IF-P5-SUITE discovery (P5-R2/R3/R12).

Globs `<root>/features/*/gates/suite.yml` (sim tier) or
`<root>/features/*/gates/suite.physical.yml` (physical tier, p6 stub), schema-validates each
against oi-p5-suite/2, and applies the LLD's extra validation rules (compose_overlays exist,
gate ids unique, script exists+executable). Never crashes on one bad manifest -- an invalid
manifest is reported as `discovered: false` with `validation_error` attached (LLD Data structures)
so the caller can still emit a full ledger (P5-R14).

p5 does not hardcode any feature's script names -- only this schema (P5-R2/R15). Same reasoning
applies to `compose_env` (oi-p5-suite/2, added 2026-07-27, real bug found running `make simtest`
for real for the first time on WSL2: `docker compose up` for the merged multi-feature stack failed
outright because several overlays (p1/p3/p4) have their own hard-required hard-required
`${VAR:?...}` env vars for config-file bind mounts, and nothing in this runner ever set them --
each feature's own bring-up helper (bring_up.sh, the DEFERRED_LIVE_GATES.md runbook) sets them
itself for its OWN standalone compose invocation, but p5's merged-stack orchestration never did).
`compose_env` lets each manifest declare exactly the env vars ITS OWN compose_overlays need,
resolved by the runner via generic `{root}`/`{feature_root}` template tokens -- this file and
simtest_runner.py never need to know GNB_CONFIG_PATH/P3_OSG_DIR/etc. by name, same as they don't
know gate script names.
"""
import argparse
import glob
import json
import os
import sys

import yaml
from jsonschema import Draft7Validator

SCHEMA_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "schemas", "suite.schema.json")


def _glob_pattern(root, tier):
    fname = "suite.yml" if tier == "sim" else "suite.physical.yml"
    return os.path.join(root, "features", "*", "gates", fname)


def _phase_from_dir(manifest_path):
    # <root>/features/<feature-dir>/gates/suite.yml -> <feature-dir>
    return os.path.basename(os.path.dirname(os.path.dirname(manifest_path)))


def discover(root, tier):
    with open(SCHEMA_PATH) as f:
        schema = json.load(f)
    validator = Draft7Validator(schema)

    entries = []
    for manifest_path in sorted(glob.glob(_glob_pattern(root, tier))):
        feature_root = os.path.dirname(os.path.dirname(manifest_path))
        entry = {
            "manifest_path": manifest_path,
            "feature_root": feature_root,
        }
        try:
            with open(manifest_path) as f:
                raw = yaml.safe_load(f)
        except yaml.YAMLError as e:
            entry.update({"phase": _phase_from_dir(manifest_path), "feature": "?",
                          "discovered": False, "validation_error": f"YAML parse error: {e}",
                          "gates": []})
            entries.append(entry)
            continue

        errors = sorted(validator.iter_errors(raw), key=lambda e: list(e.path))
        if errors:
            entry.update({
                "phase": raw.get("phase", _phase_from_dir(manifest_path)) if isinstance(raw, dict) else _phase_from_dir(manifest_path),
                "feature": raw.get("feature", "?") if isinstance(raw, dict) else "?",
                "discovered": False,
                "validation_error": "; ".join(f"{list(e.path)}: {e.message}" for e in errors),
                "gates": [],
            })
            entries.append(entry)
            continue

        # LLD extra validation rules beyond JSON-schema shape.
        val_errors = []

        seen_ids = set()
        for g in raw["gates"]:
            if g["id"] in seen_ids:
                val_errors.append(f"duplicate gate id: {g['id']}")
            seen_ids.add(g["id"])

        resolved_overlays = []
        for ov in raw["compose_overlays"]:
            p = os.path.join(feature_root, ov)
            if not os.path.isfile(p):
                val_errors.append(f"compose_overlays path does not exist: {ov}")
            resolved_overlays.append(p)

        resolved_gates = []
        for g in raw["gates"]:
            script_path = os.path.join(feature_root, g["script"])
            if not os.path.isfile(script_path):
                val_errors.append(f"gate {g['id']}: script does not exist: {g['script']}")
            elif not os.access(script_path, os.X_OK):
                val_errors.append(f"gate {g['id']}: script not executable: {g['script']}")
            resolved_gates.append({
                "id": g["id"], "type": g["type"], "script": script_path,
                "args": g.get("args", []), "timeout_s": g["timeout_s"],
            })

        if val_errors:
            entry.update({
                "phase": raw["phase"], "feature": raw["feature"], "discovered": False,
                "validation_error": "; ".join(val_errors), "gates": [],
            })
            entries.append(entry)
            continue

        # compose_env (oi-p5-suite/2): each value may reference {root} (repo root, absolute) or
        # {feature_root} (this manifest's own feature dir, absolute) -- resolved here, once, so
        # every downstream consumer (simtest_runner.py) just gets plain, ready-to-use strings.
        resolved_compose_env = {
            k: v.format(root=root, feature_root=feature_root)
            for k, v in raw.get("compose_env", {}).items()
        }

        entry.update({
            "phase": raw["phase"], "feature": raw["feature"], "discovered": True,
            "compose_overlays": resolved_overlays,
            "compose_env": resolved_compose_env,
            "setup": [os.path.join(feature_root, s) for s in raw.get("setup", [])],
            "teardown": [os.path.join(feature_root, s) for s in raw.get("teardown", [])],
            "gates": resolved_gates,
        })
        entries.append(entry)

    entries.sort(key=lambda e: e["phase"])
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["sim", "physical"], default="sim")
    _helpers_dir = os.path.dirname(os.path.abspath(__file__))
    _repo_root_default = os.path.dirname(os.path.dirname(os.path.dirname(_helpers_dir)))
    ap.add_argument("--root", default=_repo_root_default)
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    entries = discover(root, args.tier)
    print(json.dumps(entries, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
