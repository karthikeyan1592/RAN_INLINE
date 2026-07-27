#!/usr/bin/env python3
"""simtest_runner.py -- the p5 orchestrator: discover -> merge overlays -> up -> invoke gates ->
capture -> aggregate -> down (LLD Data flow).

Deliberately contains NO feature-specific assertion logic (P5-R15) -- every decision here is
generic: which manifests exist, which overlays to merge, which gate to run next, how long to
wait, where to write output, how to roll the statuses up. All actual test logic lives inside the
invoked gate scripts, which this runner treats as opaque (SPEC "Out of scope").
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discover_suites  # noqa: E402
import ledger_build  # noqa: E402

HELPERS_DIR = os.path.dirname(os.path.abspath(__file__))
FEATURE_ROOT = os.path.dirname(HELPERS_DIR)
DEFAULT_REPO_ROOT = os.path.dirname(os.path.dirname(FEATURE_ROOT))


def p0_base_overlays(root):
    # Real bug found running `make simtest` for real for the first time (2026-07-27, WSL2): this
    # used to return only compose.sim.yml, omitting the upstream docker-compose.yml (via the
    # p0-rig-scaffold/docker/upstream symlink into third_party/ocudu) that actually defines the
    # `gnb`/`5gc` services' image/build blocks -- every feature's own bring-up helper
    # (bring_up.sh, DEFERRED_LIVE_GATES.md's runbook) always layers both, in this exact order;
    # this runner's own notion of "the p0 base" was silently missing the first of the two.
    p0_docker = os.path.join(root, "features", "p0-rig-scaffold", "docker")
    return [
        os.path.join(p0_docker, "upstream", "docker", "docker-compose.yml"),
        os.path.join(p0_docker, "compose.sim.yml"),
    ]


def pins_json_path(root):
    return os.path.join(root, "features", "p0-rig-scaffold", "pins.json")

TYPE_DEFAULT_TIMEOUT = {"unit": 120, "integration": 900}


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path):
    h = hashlib.sha256()
    if os.path.isfile(path):
        with open(path, "rb") as f:
            h.update(f.read())
    else:
        h.update(b"missing:" + path.encode())
    return "sha256:" + h.hexdigest()


def sha256_files(paths):
    h = hashlib.sha256()
    for p in sorted(paths):
        h.update(p.encode())
        if os.path.isfile(p):
            with open(p, "rb") as f:
                h.update(f.read())
    return "sha256:" + h.hexdigest()


def detect_host_kind():
    try:
        with open("/proc/version") as f:
            if "microsoft" in f.read().lower():
                return "wsl2"
    except OSError:
        pass
    try:
        with open("/sys/class/dmi/id/product_name") as f:
            if "Google" in f.read():
                return "gcp"
    except OSError:
        pass
    return None


def gen_run_id():
    short = uuid.uuid4().hex[:7]
    return f"{datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')}-{short}"


def compose_cmd(compose_files, project_name):
    cmd = ["docker", "compose", "-p", project_name]
    for f in compose_files:
        cmd += ["-f", f]
    return cmd


def bring_up(compose_files, project_name, compose_env=None):
    if not compose_files:
        return True, ""
    cmd = compose_cmd(compose_files, project_name) + ["up", "-d"]
    env = {**os.environ, **(compose_env or {})}
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return r.returncode == 0, (r.stdout + r.stderr)


def tear_down(compose_files, project_name, compose_env=None):
    if not compose_files:
        return
    cmd = compose_cmd(compose_files, project_name) + ["down", "-v"]
    env = {**os.environ, **(compose_env or {})}
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    # Real bug found running this for real (2026-07-27, WSL2): this used to silently swallow a
    # failed teardown (no env vars passed, same class of gap bring_up() had -- see its own
    # comment), leaving every container running after a `make simtest` invocation that itself
    # reported [p5] overall: PASS. A failed teardown must be visible, not silent -- this is the
    # runner's own cleanup responsibility, same as bring_up() is.
    if r.returncode != 0:
        print(f"[p5] compose down FAILED (exit={r.returncode}): {r.stdout}{r.stderr}", file=sys.stderr)


def run_one_gate(phase, gate, artifacts_dir, timeout_scale):
    gate_dir = os.path.join(artifacts_dir, phase, gate["id"])
    os.makedirs(gate_dir, exist_ok=True)
    timeout_s = gate["timeout_s"] * timeout_scale
    run_gate_sh = os.path.join(HELPERS_DIR, "run_gate.sh")
    cmd = [run_gate_sh, "--id", gate["id"], "--type", gate["type"],
           "--timeout-s", str(timeout_s), "--artifacts-dir", gate_dir, "--",
           gate["script"]] + gate.get("args", [])
    start = time.time()
    r = subprocess.run(cmd)
    duration = time.time() - start
    timed_out = (r.returncode == 124)
    # run_gate.sh writes its own duration_s (wall clock of the invocation, more precise); read it
    # back if present, else fall back to our own measurement.
    dur_path = os.path.join(gate_dir, "duration_s")
    if os.path.isfile(dur_path):
        with open(dur_path) as f:
            try:
                duration = float(f.read().strip())
            except ValueError:
                pass
    return {
        "phase": phase, "gate_id": gate["id"], "type": gate["type"],
        "exit_code": r.returncode, "duration_s": duration, "timed_out": timed_out,
        "artifacts_dir": gate_dir,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["sim", "physical"], default="sim")
    ap.add_argument("--keep-up", action="store_true")
    ap.add_argument("--only-phase", default="")
    ap.add_argument("--timeout-scale", type=float, default=1.0)
    ap.add_argument("--run-id", default="")
    ap.add_argument("--artifacts-dir", default=os.path.join(DEFAULT_REPO_ROOT, "artifacts", "p5"))
    ap.add_argument("--root", default=DEFAULT_REPO_ROOT)
    ap.add_argument("--host-kind", default="")
    ap.add_argument("--no-compose", action="store_true",
                     help="skip docker compose up/down entirely (unit-test mode)")
    args = ap.parse_args()

    run_id = args.run_id or gen_run_id()
    artifacts_dir = os.path.join(args.artifacts_dir, run_id)
    os.makedirs(artifacts_dir, exist_ok=True)

    started_utc = utc_now()
    print(f"[p5] run_id={run_id} tier={args.tier} root={args.root}")

    manifests = discover_suites.discover(os.path.abspath(args.root), args.tier)
    with open(os.path.join(artifacts_dir, "manifests.json"), "w") as f:
        json.dump(manifests, f, indent=2)

    only_phases = set(p.strip() for p in args.only_phase.split(",") if p.strip())

    valid = [m for m in manifests if m["discovered"]]
    print(f"[p5] discovered {len(manifests)} manifest(s), {len(valid)} valid")

    overlay_set = []
    seen = set()
    for m in valid:
        for ov in m["compose_overlays"]:
            if ov not in seen:
                seen.add(ov)
                overlay_set.append(ov)

    # compose_env (oi-p5-suite/2): union of every valid manifest's own declared env vars, already
    # resolved (no {root}/{feature_root} tokens left) by discover_suites.py. Generic merge only --
    # this runner never inspects a var name, matching P5-R15 the same way overlay merging does.
    compose_env = {}
    for m in valid:
        compose_env.update(m.get("compose_env", {}))

    root_abs = os.path.abspath(args.root)
    p0_base = [p for p in p0_base_overlays(root_abs) if os.path.isfile(p)]
    compose_files = (p0_base if not args.no_compose else []) + overlay_set
    if args.no_compose:
        compose_files = []

    project_name = f"oi-p5-{run_id}".lower().replace(":", "-")

    up_ok, up_log = (True, "") if not compose_files else bring_up(compose_files, project_name, compose_env)
    with open(os.path.join(artifacts_dir, "compose_up.log"), "w") as f:
        f.write(up_log)

    results = []
    if up_ok:
        for m in valid:
            skip = bool(only_phases) and m["phase"] not in only_phases
            if skip:
                print(f"[p5] phase {m['phase']}: skipped by --only-phase")
                continue
            for gate in m["gates"]:
                print(f"[p5] {m['phase']}/{gate['id']}: running...")
                res = run_one_gate(m["phase"], gate, artifacts_dir, args.timeout_scale)
                print(f"[p5] {m['phase']}/{gate['id']}: exit={res['exit_code']} "
                      f"duration={res['duration_s']:.2f}s")
                results.append(res)
    else:
        print("[p5] compose up FAILED -- runner ERROR, skipping gate invocation", file=sys.stderr)

    if not args.keep_up:
        tear_down(compose_files, project_name, compose_env)
    else:
        print(f"[p5] --keep-up: leaving stack up (project={project_name})")

    with open(os.path.join(artifacts_dir, "results.json"), "w") as f:
        json.dump(results, f, indent=2)

    finished_utc = utc_now()
    host_kind = args.host_kind or detect_host_kind() or "wsl2"
    pins_digest = sha256_file(pins_json_path(root_abs))
    rigcfg_digest = sha256_files(compose_files)

    # Build phase entries directly (avoids a second subprocess hop through ledger_build.py's CLI).
    results_by_key = {(r["phase"], r["gate_id"]): r for r in results}
    phases = []
    for m in manifests:
        if not m["discovered"]:
            entry = {"phase": m["phase"], "feature": m["feature"], "discovered": False, "gates": []}
            if "validation_error" in m:
                entry["validation_error"] = m["validation_error"]
            phases.append(entry)
            continue
        skip = bool(only_phases) and m["phase"] not in only_phases
        gates_out = []
        if not skip or not up_ok:
            for g in m["gates"]:
                r = results_by_key.get((m["phase"], g["id"]))
                if r is None:
                    if not up_ok:
                        gates_out.append({"id": g["id"], "type": g["type"], "status": "ERROR",
                                          "exit_code": None, "duration_s": 0.0, "verdict": None,
                                          "artifacts": ""})
                    continue
                verdict, valid_json = ledger_build.last_json_line(
                    os.path.join(r["artifacts_dir"], "stdout.log"))
                status = ledger_build.classify_gate(r["exit_code"], r["timed_out"], valid_json)
                gates_out.append({"id": g["id"], "type": g["type"], "status": status,
                                  "exit_code": r["exit_code"], "duration_s": r["duration_s"],
                                  "verdict": verdict, "artifacts": r["artifacts_dir"]})
        phase_entry = {"phase": m["phase"], "feature": m["feature"], "discovered": True,
                      "gates": gates_out}
        if skip:
            phase_entry["skipped"] = True
        phases.append(phase_entry)

    host = {"kind": host_kind}
    ledger = ledger_build.build_ledger(run_id, started_utc, finished_utc, args.tier, host,
                                       pins_digest, rigcfg_digest, phases)

    ledger_path = os.path.join(artifacts_dir, "ledger.json")
    with open(ledger_path, "w") as f:
        json.dump(ledger, f, indent=2)

    md_path = os.path.join(artifacts_dir, "ledger.md")
    render_mod_path = os.path.join(HELPERS_DIR, "ledger_render_md.py")
    subprocess.run([sys.executable, render_mod_path, ledger_path, "-o", md_path])

    print(ledger_path)
    print(f"[p5] overall: {ledger['overall']}")

    if not up_ok:
        # Runner-level infrastructure failure (compose up itself failed), distinct from any
        # gate-level FAIL/ERROR rolled into ledger.overall -- LLD Public APIs exit-code table.
        return 2
    if ledger["overall"] == "PASS":
        return 0
    if ledger["overall"] == "BLOCKED":
        return 3
    return 1


if __name__ == "__main__":
    sys.exit(main())
