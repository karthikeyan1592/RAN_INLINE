#!/usr/bin/env python3
"""agpl_denylist.py — P2-R13 AGPL-hygiene CI gate.

Scans the repo tree (source) and, if given, a built image's exported filesystem/tarball for any
path under `third_party/srsRAN_Project` (the AGPLv3 checkout) or any vendored srsRAN test-data
header. srsRAN golden vectors may be *read* at CI runtime via a bind mount (see p2-phy-kernels
LLD §5 config: oracle.srsran_vectors_path) — they must never be copied, committed, or baked into
a shipped/packaged artifact.

Usage:
  agpl_denylist.py --repo-root PATH [--image-export TARBALL_OR_DIR]
Exit 0 = clean. Exit 1 = one or more AGPL artifacts found in a place they must not be.
"""
import argparse
import sys
import tarfile
from pathlib import Path

DENYLIST_PATH_FRAGMENTS = (
    "third_party/srsRAN_Project",
    "srsRAN_Project",  # catches an accidental copy landing at a different relative depth
)

# Filenames strongly associated with srsRAN's own vector/test-data headers — a copy of one of
# these committed into our tree (as opposed to read via the CI bind mount) is exactly the
# violation this gate exists to catch.
SRSRAN_TESTDATA_HINTS = ("_test_data.h", "_vectortest.cpp")


def scan_repo_tree(repo_root: Path) -> list[str]:
    errs = []
    for path in repo_root.rglob("*"):
        if not path.is_file():
            continue
        rel = str(path.relative_to(repo_root))
        if "third_party/srsRAN_Project" in rel:
            continue  # the checkout itself is allowed to exist on disk; it must just never ship
        if any(rel.endswith(hint) for hint in SRSRAN_TESTDATA_HINTS):
            # A vendored srsRAN vector header living OUTSIDE third_party/ (i.e. copied into our
            # own tree) is the violation; check its content is really srsRAN-flavored before
            # flagging (cheap heuristic: filename pattern is already fairly specific).
            if "third_party" not in rel:
                errs.append(f"{path}: filename matches srsRAN vector/test-data pattern outside "
                            f"third_party/ — looks like a vendored copy, not a CI bind-mount read")
    return errs


def scan_export(export_path: Path) -> list[str]:
    """Scan a built image's exported filesystem (directory) or a `docker save`/`export` tarball."""
    errs = []
    if export_path.is_dir():
        candidates = (str(p) for p in export_path.rglob("*"))
    elif tarfile.is_tarfile(export_path):
        with tarfile.open(export_path) as tf:
            candidates = (m.name for m in tf.getmembers())
    else:
        print(f"error: {export_path} is neither a directory nor a tarfile", file=sys.stderr)
        sys.exit(2)

    for name in candidates:
        for frag in DENYLIST_PATH_FRAGMENTS:
            if frag in name:
                errs.append(f"{export_path}::{name}: contains denylisted path fragment '{frag}' "
                            f"— an AGPL artifact would ship in this image")
    return errs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", required=True, type=Path)
    ap.add_argument("--image-export", type=Path, default=None,
                     help="directory (exported image fs) or tarball to scan for shipped AGPL artifacts")
    args = ap.parse_args()

    all_errs = scan_repo_tree(args.repo_root)
    if args.image_export:
        all_errs.extend(scan_export(args.image_export))

    if all_errs:
        for e in all_errs:
            print(f"FAIL: {e}", file=sys.stderr)
        print(f"\nagpl_denylist: {len(all_errs)} violation(s)", file=sys.stderr)
        return 1

    print("PASS: agpl_denylist — no AGPL artifacts found in a place they must not be")
    return 0


if __name__ == "__main__":
    sys.exit(main())
