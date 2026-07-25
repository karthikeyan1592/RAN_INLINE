#!/usr/bin/env python3
"""oracle-verdict — IF-P0-VERDICT CLI (P0-R5, LLD "Public APIs").

Usage: oracle-verdict --kernel K --case ID --result FILE [--tolerance-profile P]
Exit 0 = PASS, 1 = FAIL, 2 = usage/IO error.
Stdout: one JSON line, schema per LLD "Public APIs" (oracle-verdict section).

P0 ships bit_exact comparisons only (LDPC). --tolerance-profile is accepted and recorded but has
no effect until p2-phy-kernels' float-stage kernels need it (LLD Open Question, not yet defined).
"""
import argparse
import json
import sys
from pathlib import Path

VECTOR_DIR_DEFAULT = "/oi/vectors"


def load_manifest(kernel: str, vector_dir: str) -> dict:
    manifest_path = Path(vector_dir) / kernel / "manifest.json"
    if not manifest_path.is_file():
        print(f"error: no manifest for kernel '{kernel}' at {manifest_path}", file=sys.stderr)
        sys.exit(2)
    with manifest_path.open() as f:
        return json.load(f)


def find_case(manifest: dict, case_id: str) -> dict:
    for case in manifest.get("cases", []):
        if case.get("id") == case_id:
            return case
    print(f"error: case '{case_id}' not found in manifest", file=sys.stderr)
    sys.exit(2)


def compare_bit_exact(expected_path: Path, result_path: Path) -> int:
    """Returns mismatch count (bytewise; P0's LDPC vectors are byte-packed bitstreams)."""
    if not expected_path.is_file():
        print(f"error: expected file missing: {expected_path}", file=sys.stderr)
        sys.exit(2)
    if not result_path.is_file():
        print(f"error: result file missing: {result_path}", file=sys.stderr)
        sys.exit(2)
    exp = expected_path.read_bytes()
    got = result_path.read_bytes()
    if len(exp) != len(got):
        # Length mismatch: count every bit as a mismatch (can't do a meaningful bitwise diff).
        return max(len(exp), len(got)) * 8
    mismatches = 0
    for a, b in zip(exp, got):
        x = a ^ b
        mismatches += bin(x).count("1")
    return mismatches


def main() -> int:
    ap = argparse.ArgumentParser(prog="oracle-verdict")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--case", required=True, dest="case_id")
    ap.add_argument("--result", required=True, dest="result_file")
    ap.add_argument("--tolerance-profile", default=None,
                     help="reserved for p2 float-stage kernels; no effect in P0 (bit_exact only)")
    ap.add_argument("--vector-dir", default=VECTOR_DIR_DEFAULT)
    args = ap.parse_args()

    manifest = load_manifest(args.kernel, args.vector_dir)
    case = find_case(manifest, args.case_id)
    compare = case.get("compare", "bit_exact")

    if compare != "bit_exact":
        print(f"error: compare mode '{compare}' unsupported in P0 (bit_exact only; "
              f"--tolerance-profile reserved for p2)", file=sys.stderr)
        return 2

    case_dir = Path(args.vector_dir) / args.kernel / args.case_id
    expected_path = case_dir / "expected.bin"
    mismatches = compare_bit_exact(expected_path, Path(args.result_file))

    verdict = "pass" if mismatches == 0 else "fail"
    out = {
        "kernel": args.kernel,
        "case": args.case_id,
        "verdict": verdict,
        "compare": "bit_exact",
        "mismatches": mismatches,
    }
    print(json.dumps(out))
    return 0 if verdict == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
