#!/usr/bin/env python3
"""provenance_check.py — P2-R12 CI gate.

Every `k*.cl` kernel source across the p2 sub-feature family must have a matching entry in the
`provenance.json` that lives alongside it (same `src/kernels/` directory) — schema
"oi-p2-provenance/1" (see ../src/host/oi_p2_provenance.h). Fails CI, naming the offending file, if
any kernel source lacks an entry or an entry is missing its required fields.

Usage: provenance_check.py [FEATURES_ROOT]   # default: parent of this feature's parent (features/)
"""
import json
import sys
from pathlib import Path

EXPECTED_OCUDU_TAG = "release_26_04"
SHA_LEN = 40


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def check_entry(entry: dict, kernel_file: Path) -> list[str]:
    errs = []
    for key in ("kernel", "type", "ocudu"):
        if key not in entry:
            errs.append(f"{kernel_file}: entry missing required key '{key}'")
    if "ocudu" in entry:
        for key in ("repo", "tag", "sha"):
            if key not in entry["ocudu"]:
                errs.append(f"{kernel_file}: ocudu entry missing '{key}'")
        if entry["ocudu"].get("tag") != EXPECTED_OCUDU_TAG:
            errs.append(
                f"{kernel_file}: ocudu.tag is '{entry['ocudu'].get('tag')}', "
                f"expected '{EXPECTED_OCUDU_TAG}'"
            )
        sha = entry["ocudu"].get("sha", "")
        if len(sha) != SHA_LEN:
            errs.append(f"{kernel_file}: ocudu.sha is not a {SHA_LEN}-hex commit SHA: '{sha}'")
    kind = entry.get("type")
    if kind == "port" and not entry.get("port_sources"):
        errs.append(f"{kernel_file}: type=='port' but 'port_sources' is missing/empty")
    elif kind == "fresh" and not entry.get("references"):
        errs.append(f"{kernel_file}: type=='fresh' but 'references' is missing/empty")
    elif kind not in ("port", "fresh"):
        errs.append(f"{kernel_file}: type is '{kind}', expected 'port' or 'fresh'")
    return errs


def main() -> int:
    features_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    all_errs: list[str] = []
    checked = 0

    for kernels_dir in sorted(features_root.glob("p2*/src/kernels")):
        cl_files = sorted(kernels_dir.glob("k*.cl"))
        if not cl_files:
            continue
        prov_path = kernels_dir / "provenance.json"
        if not prov_path.is_file():
            for f in cl_files:
                all_errs.append(f"{f}: no provenance.json found in {kernels_dir}")
            continue
        with prov_path.open() as fh:
            entries = {e.get("kernel"): e for e in json.load(fh)}
        for f in cl_files:
            checked += 1
            stem = f.stem
            if stem not in entries:
                all_errs.append(f"{f}: no provenance entry for kernel '{stem}' in {prov_path}")
                continue
            all_errs.extend(check_entry(entries[stem], f))

    if all_errs:
        for e in all_errs:
            fail(e)
        print(f"\nprovenance_check: {len(all_errs)} problem(s) across {checked} kernel file(s)",
              file=sys.stderr)
        return 1

    print(f"PASS: provenance_check — {checked} kernel file(s), all have valid provenance entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
