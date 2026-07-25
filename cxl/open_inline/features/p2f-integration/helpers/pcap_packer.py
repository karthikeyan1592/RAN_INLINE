#!/usr/bin/env python3
"""pcap_packer.py -- P2-R15b oracle-vector packer.

Thin Python wrapper around `tools/oracle_tx_gen.cpp` (a compiled C++ binary). Deliberately does
NOT reimplement any of the TX encoding (CRC/segmentation/LDPC-encode/rate-match/scramble/modulate)
or the eCPRI/O-RAN wire framing in Python -- that logic already exists, correctly, as real linked
OCUDU calls in oracle_tx_gen.cpp (self-verified there against OCUDU's own real decoder before ever
being trusted here; see that file's header and VERIFICATION.md). Reimplementing wire-format logic
a second time in a different language, un-vetted against the same oracle, is exactly the kind of
avoidable correctness/provenance risk this project has repeatedly chosen not to take (same
reasoning as bg_tables.h's provenance fix). This script's own job is purely mechanical: build the
binary if needed, invoke it with the right arguments, and hand back the two paths it wrote plus the
parsed ground-truth sidecar -- no OCUDU-adjacent logic lives here.

Usage (library):
    from pcap_packer import pack
    pcap_path, json_path, sidecar = pack(mcs_index=4, seed=1, out_dir="tests/fixtures/oracle_pcaps")

Usage (CLI):
    python3 pcap_packer.py 4 1 tests/fixtures/oracle_pcaps
"""
import argparse
import json
import os
import subprocess

FEATURE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # .../p2f-integration
ORACLE_BIN = os.path.join(FEATURE_ROOT, "build", "oracle_tx_gen")
VALID_MCS = (4, 13, 21)


def ensure_built():
    """Builds oracle_tx_gen via the feature's own Makefile if the binary isn't already present.
    Does not attempt to bootstrap OCUDU itself (`make bootstrap-ocudu`) -- that is a one-time,
    slow (full OCUDU channel_coding/modulation/ofh build), explicit setup step every other feature
    in this project already requires the caller to have run once; this script assumes it has been.
    """
    if not os.path.isfile(ORACLE_BIN):
        subprocess.run(["make", "build/oracle_tx_gen"], cwd=FEATURE_ROOT, check=True)


def pack(mcs_index, seed, out_dir):
    """Generates one real, self-verified, wire-valid oracle transmission for `mcs_index` (must be
    one of the MVP's fixed set 4/13/21) using `seed` for the random transport block, and packs it
    into `out_dir` as a .pcap (14 real eCPRI+O-RAN U-plane frames) + a JSON ground-truth sidecar
    (schema oi-p2-oracle/1: mcs_index, qm, tbs_bits, tb_size_bytes, nof_cb, base_graph, rnti, n_id,
    nslot, seed, tb_bytes_hex). Returns (pcap_path, json_path, sidecar_dict). Raises RuntimeError
    if oracle_tx_gen's own self-check (real-decoder round-trip on all 14 frames) fails -- an
    unverified oracle is refused, not silently packed anyway.
    """
    if mcs_index not in VALID_MCS:
        raise ValueError(f"mcs_index must be one of {VALID_MCS}, got {mcs_index}")
    ensure_built()
    os.makedirs(out_dir, exist_ok=True)
    pcap_path = os.path.abspath(os.path.join(out_dir, f"oracle_mcs{mcs_index}_seed{seed}.pcap"))
    json_path = os.path.abspath(os.path.join(out_dir, f"oracle_mcs{mcs_index}_seed{seed}.json"))

    result = subprocess.run(
        [ORACLE_BIN, str(mcs_index), str(seed), pcap_path, json_path],
        cwd=os.path.join(FEATURE_ROOT, "tests"),  # no CWD dependency in oracle_tx_gen itself
        # (unlike pipeline_runner's kernel-path fragility) -- kept consistent with this project's
        # other tools purely for convention, not because it's load-bearing here.
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"oracle_tx_gen failed (mcs_index={mcs_index}, seed={seed}), exit {result.returncode}:\n"
            f"{result.stderr}"
        )

    with open(json_path) as f:
        sidecar = json.load(f)
    return pcap_path, json_path, sidecar


def main():
    ap = argparse.ArgumentParser(
        description="Pack a real, wire-valid oracle PUSCH-like transmission into a .pcap + JSON "
                    "ground-truth sidecar (P2-R15b)."
    )
    ap.add_argument("mcs_index", type=int, choices=list(VALID_MCS))
    ap.add_argument("seed", type=int)
    ap.add_argument("out_dir")
    args = ap.parse_args()

    pcap_path, json_path, sidecar = pack(args.mcs_index, args.seed, args.out_dir)
    print(f"wrote {pcap_path}")
    print(f"wrote {json_path}")
    print(json.dumps(sidecar, indent=2))


if __name__ == "__main__":
    main()
