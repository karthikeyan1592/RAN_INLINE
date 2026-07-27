#!/usr/bin/env python3
"""test_discover_suites.py -- P5-R2/R3 unit tests: schema/path validation against synthetic
`features/*/gates/suite.yml` layouts, built in a tempdir so no real p1-p4 suites are needed."""
import os
import shutil
import stat
import sys
import tempfile

FEATURE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(FEATURE_ROOT, "helpers"))
import discover_suites  # noqa: E402

g_fail = 0


def check(cond, what):
    global g_fail
    if cond:
        print(f"PASS: {what}")
    else:
        print(f"FAIL: {what}", file=sys.stderr)
        g_fail += 1


def make_executable(path, body="#!/usr/bin/env bash\necho '{}'\n"):
    with open(path, "w") as f:
        f.write(body)
    os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def write_suite(root, feature_dir, phase, feature, gates, overlays=None, extra_yaml=""):
    fdir = os.path.join(root, "features", feature_dir)
    os.makedirs(os.path.join(fdir, "gates"), exist_ok=True)
    os.makedirs(os.path.join(fdir, "docker"), exist_ok=True)
    os.makedirs(os.path.join(fdir, "scripts"), exist_ok=True)

    overlays = overlays if overlays is not None else ["docker/compose.mock.yml"]
    for ov in overlays:
        p = os.path.join(fdir, ov)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write("services: {}\n")

    gate_lines = []
    for g in gates:
        script_path = os.path.join(fdir, g["script"])
        os.makedirs(os.path.dirname(script_path), exist_ok=True)
        make_executable(script_path)
        gate_lines.append(
            f"  - id: {g['id']}\n    type: {g['type']}\n    script: {g['script']}\n"
            f"    args: []\n    timeout_s: {g.get('timeout_s', 10)}\n"
        )

    yaml_text = (
        f"schema: oi-p5-suite/2\nphase: {phase}\nfeature: {feature}\n"
        f"compose_overlays:\n" + "".join(f"  - {ov}\n" for ov in overlays) +
        f"gates:\n" + "".join(gate_lines) + extra_yaml
    )
    with open(os.path.join(fdir, "gates", "suite.yml"), "w") as f:
        f.write(yaml_text)


def main():
    tmp = tempfile.mkdtemp(prefix="p5_discover_test_")
    try:
        write_suite(tmp, "pA-mock", "pA", "pA-mock-feature",
                    [{"id": "pa-g1", "type": "unit", "script": "scripts/gate1.sh"}])
        write_suite(tmp, "pB-mock", "pB", "pB-mock-feature",
                    [{"id": "pb-g1", "type": "integration", "script": "scripts/gate1.sh"},
                     {"id": "pb-g2", "type": "unit", "script": "scripts/gate2.sh"}])

        entries = discover_suites.discover(tmp, "sim")
        check(len(entries) == 2, "discovers exactly the 2 valid synthetic manifests")
        check(entries[0]["phase"] == "pA" and entries[1]["phase"] == "pB",
              "entries sorted by phase (pA before pB)")
        check(all(e["discovered"] for e in entries), "both valid manifests marked discovered=true")
        check(len(entries[1]["gates"]) == 2, "pB manifest's 2 gates both resolved")
        check(entries[0]["gates"][0]["script"].endswith("scripts/gate1.sh"),
              "gate script path resolved relative to feature root")

        # Missing schema key -- should be rejected, not crash.
        bad_dir = os.path.join(tmp, "features", "pC-bad-schema", "gates")
        os.makedirs(bad_dir, exist_ok=True)
        with open(os.path.join(bad_dir, "suite.yml"), "w") as f:
            f.write("schema: oi-p5-suite/2\nphase: pC\n")  # missing feature/compose_overlays/gates
        entries2 = discover_suites.discover(tmp, "sim")
        pc = [e for e in entries2 if e["phase"] == "pC"][0]
        check(pc["discovered"] is False, "manifest missing required keys marked discovered=false")
        check("validation_error" in pc and pc["validation_error"], "validation_error attached for bad schema")

        # Nonexistent script path -- should be rejected, not crash.
        write_suite(tmp, "pD-badscript", "pD", "pD-mock-feature",
                    [{"id": "pd-g1", "type": "unit", "script": "scripts/does_not_exist.sh"}])
        # write_suite always creates+chmods the script; simulate a broken manifest by removing it after.
        os.remove(os.path.join(tmp, "features", "pD-badscript", "scripts", "does_not_exist.sh"))
        entries3 = discover_suites.discover(tmp, "sim")
        pd = [e for e in entries3 if e["phase"] == "pD"][0]
        check(pd["discovered"] is False, "manifest with nonexistent gate script marked discovered=false")
        check("does not exist" in pd["validation_error"], "validation_error names the missing script")

        # Nonexistent compose_overlays path -- should be rejected, not crash.
        write_suite(tmp, "pE-badoverlay", "pE", "pE-mock-feature",
                    [{"id": "pe-g1", "type": "unit", "script": "scripts/gate1.sh"}],
                    overlays=["docker/does_not_exist.yml"])
        os.remove(os.path.join(tmp, "features", "pE-badoverlay", "docker", "does_not_exist.yml"))
        entries4 = discover_suites.discover(tmp, "sim")
        pe = [e for e in entries4 if e["phase"] == "pE"][0]
        check(pe["discovered"] is False, "manifest with nonexistent compose_overlays marked discovered=false")

        # Duplicate gate ids -- rejected.
        write_suite(tmp, "pF-dupid", "pF", "pF-mock-feature",
                    [{"id": "same", "type": "unit", "script": "scripts/gate1.sh"},
                     {"id": "same", "type": "unit", "script": "scripts/gate2.sh"}])
        entries5 = discover_suites.discover(tmp, "sim")
        pf = [e for e in entries5 if e["phase"] == "pF"][0]
        check(pf["discovered"] is False, "duplicate gate ids within one manifest rejected")

        # bad type value
        bad_type_dir = os.path.join(tmp, "features", "pG-badtype", "gates")
        os.makedirs(bad_type_dir, exist_ok=True)
        scripts_dir = os.path.join(tmp, "features", "pG-badtype", "scripts")
        os.makedirs(scripts_dir, exist_ok=True)
        make_executable(os.path.join(scripts_dir, "gate1.sh"))
        docker_dir = os.path.join(tmp, "features", "pG-badtype", "docker")
        os.makedirs(docker_dir, exist_ok=True)
        with open(os.path.join(docker_dir, "compose.mock.yml"), "w") as f:
            f.write("services: {}\n")
        with open(os.path.join(bad_type_dir, "suite.yml"), "w") as f:
            f.write("schema: oi-p5-suite/2\nphase: pG\nfeature: pG-mock\n"
                    "compose_overlays:\n  - docker/compose.mock.yml\n"
                    "gates:\n  - id: g1\n    type: NOT_A_TYPE\n    script: scripts/gate1.sh\n"
                    "    timeout_s: 10\n")
        entries6 = discover_suites.discover(tmp, "sim")
        pg = [e for e in entries6 if e["phase"] == "pG"][0]
        check(pg["discovered"] is False, "invalid gate type value rejected by schema")

        # No manifests at all (physical tier, nothing shipped) -- empty list, no crash.
        entries7 = discover_suites.discover(tmp, "physical")
        check(entries7 == [], "physical tier with no manifests returns empty list, not a crash")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    total = g_fail
    print(f"\n{'PASS' if total == 0 else 'FAIL'}: test_discover_suites — {total} failure(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
