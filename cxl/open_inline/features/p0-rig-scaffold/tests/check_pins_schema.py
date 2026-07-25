#!/usr/bin/env python3
"""check_pins_schema.py — P0-R7 test: pins.json validates against oi-pins/1; both images' labels
match. Usage: check_pins_schema.py pins.json"""
import json
import subprocess
import sys

REQUIRED_TOP = ["schema", "built_utc", "ocudu", "base_image", "pocl", "adaptivecpp", "ldpc_suite", "images"]
REQUIRED_OCUDU = ["repo", "tag", "sha"]


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_pins_schema.py pins.json", file=sys.stderr)
        return 2

    with open(sys.argv[1]) as f:
        pins = json.load(f)

    if pins.get("schema") != "oi-pins/1":
        fail(f"schema is '{pins.get('schema')}', expected 'oi-pins/1'")
    for key in REQUIRED_TOP:
        if key not in pins:
            fail(f"missing top-level key: {key}")
    for key in REQUIRED_OCUDU:
        if key not in pins["ocudu"]:
            fail(f"missing ocudu.{key}")
    if pins["ocudu"]["tag"] != "release_26_04":
        fail(f"ocudu.tag is '{pins['ocudu']['tag']}', expected 'release_26_04'")
    if len(pins["ocudu"]["sha"]) != 40:
        fail(f"ocudu.sha is not a 40-hex commit SHA: {pins['ocudu']['sha']}")

    image_names = {img["name"] for img in pins["images"]}
    if image_names != {"oi/gpu-phy", "oi/oracle"}:
        fail(f"images list is {image_names}, expected exactly oi/gpu-phy + oi/oracle")

    print("PASS: pins.json schema valid, ocudu pin correct, both images present")

    # Label parity: both images' org.openinline.pins labels must be identical (P0-R7).
    labels = {}
    for name in ("oi/gpu-phy:dev", "oi/oracle:dev"):
        out = subprocess.run(
            ["docker", "image", "inspect", name, "--format",
             '{{index .Config.Labels "org.openinline.pins"}}'],
            capture_output=True, text=True, check=True,
        )
        labels[name] = out.stdout.strip()

    if labels["oi/gpu-phy:dev"] != labels["oi/oracle:dev"]:
        fail("org.openinline.pins label differs between oi/gpu-phy:dev and oi/oracle:dev")

    try:
        json.loads(labels["oi/gpu-phy:dev"])
    except json.JSONDecodeError as e:
        fail(f"org.openinline.pins label is not valid JSON: {e}")

    print("PASS: org.openinline.pins label identical on both images and parses as JSON")
    return 0


if __name__ == "__main__":
    sys.exit(main())
