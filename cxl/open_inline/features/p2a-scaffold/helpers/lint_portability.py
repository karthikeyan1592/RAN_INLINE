#!/usr/bin/env python3
"""lint_portability.py — P2-R2 static portability lint.

Scans every `.cl`/SYCL source across the p2 sub-feature family for warp/wavefront-width
assumptions, inline asm, and vendor-specific preprocessor branches — all of which are banned
outside `oi_kernel_compat.h` (the single permitted location, SIM §3). Runs against whatever
kernel sources exist at the time it's invoked, so it's live from commit 1 (before any real kernel
lands) and stays live as p2b..p2f each add their kernels.

Usage: lint_portability.py [FEATURES_ROOT]
Exit 0 = clean. Exit 1 = one or more banned patterns found (file:line printed for each).
"""
import re
import sys
from pathlib import Path

# Each pattern: (regex, human description). Matched against source text outside compat headers.
BANNED_PATTERNS = [
    (re.compile(r"\bwarpSize\b"), "warp-size assumption (warpSize)"),
    (re.compile(r"\b__shfl(_\w+)?\s*\("), "warp-shuffle intrinsic (__shfl*)"),
    (re.compile(r"\basm\s*(volatile)?\s*\("), "inline assembly"),
    (re.compile(r"#\s*ifdef\s+__NVPTX__"), "vendor-conditional #ifdef __NVPTX__"),
    (re.compile(r"#\s*ifdef\s+__AMDGCN__"), "vendor-conditional #ifdef __AMDGCN__"),
    (re.compile(r"#\s*ifdef\s+__OPENCL_CPP_VERSION__"), "vendor-conditional preprocessor branch"),
    (re.compile(r"\bget_sub_group_size\s*\("), "sub-group-size query (implies width assumption)"),
    (re.compile(r"\bsub_group_shuffle\b"), "SYCL sub-group shuffle (width assumption)"),
]

# Kernel-source-only concern: literal work-group-size constants baked into the .cl file itself
# (as opposed to a runtime clGetKernelWorkGroupInfo query). Heuristic: a __kernel/__attribute__
# reqd_work_group_size is exactly the kind of literal-WG-size dependency P2-R2 bans.
REQD_WG_SIZE = re.compile(r"reqd_work_group_size")

ALLOWED_FILE_SUFFIXES = (".cl", ".hpp.sycl", ".cpp.sycl")  # SYCL variant sources, if/when added
COMPAT_HEADER_NAME = "oi_kernel_compat.h"


def scan_file(path: Path) -> list[str]:
    errs = []
    text = path.read_text(errors="replace")
    for lineno, line in enumerate(text.splitlines(), start=1):
        for pattern, desc in BANNED_PATTERNS:
            if pattern.search(line):
                errs.append(f"{path}:{lineno}: {desc}: {line.strip()}")
        if REQD_WG_SIZE.search(line):
            errs.append(f"{path}:{lineno}: literal work-group-size constant "
                        f"(reqd_work_group_size) — query via clGetKernelWorkGroupInfo instead: "
                        f"{line.strip()}")
    return errs


def main() -> int:
    features_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    all_errs: list[str] = []
    checked = 0

    for kernels_dir in sorted(features_root.glob("p2*/src/kernels")):
        for f in sorted(kernels_dir.glob("*.cl")):
            if f.name == COMPAT_HEADER_NAME:
                continue
            checked += 1
            all_errs.extend(scan_file(f))
        for f in sorted(kernels_dir.glob("*.h")):
            if f.name == COMPAT_HEADER_NAME:
                continue  # the one permitted location for vendor branches
            checked += 1
            all_errs.extend(scan_file(f))

    if all_errs:
        for e in all_errs:
            print(f"FAIL: {e}", file=sys.stderr)
        print(f"\nlint_portability: {len(all_errs)} violation(s) across {checked} file(s)",
              file=sys.stderr)
        return 1

    print(f"PASS: lint_portability — {checked} file(s) scanned, no banned patterns "
          f"(outside {COMPAT_HEADER_NAME})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
