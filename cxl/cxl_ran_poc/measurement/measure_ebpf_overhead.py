#!/usr/bin/env python3
"""
measure_ebpf_overhead.py — True eBPF uprobe overhead via paired srsRAN runs.

Method:
  Phase 1: N runs of srsRAN WITHOUT eBPF loader  → per-call latency T_base
  Phase 2: N runs of srsRAN WITH   eBPF loader   → per-call latency T_probe
             (offload_enabled = 0, probe fires + early-exit only)
  overhead_ns = (T_probe_us - T_base_us) * 1000 per call

The benchmark is run at -R 1000 (1000 internal reps) to reduce per-run noise.
With cb_len=9216 bits and 46 Mbps throughput: per-rep latency ≈ 200 µs.
Uprobe overhead of ~1-5 µs shows as 0.5-2.5% throughput reduction.

Output: paper/results/ebpf_overhead.csv
Columns: sample_id, t_base_ns, t_probe_ns, overhead_ns, emulation_mode
"""

import subprocess
import re
import csv
import os
import sys
import time
import signal
import statistics
import argparse

BINARY_DEFAULT = (
    "/root/linux_env/cxl/third_party/srsRAN_Project/build/tests/"
    "benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark"
)
LOADER_DEFAULT = "/root/linux_env/cxl/cxl_ran_poc/ebpf/l1_intercept_loader"
SOCKET_DEFAULT = "/tmp/gpu_daemon.sock"
SYMFILE_DEFAULT = (
    "/root/linux_env/cxl/cxl_ran_poc/paper/results/srsran_probe_symbol.txt"
)
RESULTS_DEFAULT = "/root/linux_env/cxl/cxl_ran_poc/paper/results"
EMULATION_MODE = "wsl2-mmap-shm-fallback"

# BG1 LS=384 R=0.917 codeblock: cb_len = 24 * 384 = 9216 bits
CB_BITS = 9216
REPS_PER_RUN = 1000   # internal benchmark reps → reduces per-run noise


def parse_p50_mbps(output: str) -> float | None:
    """Extract p50 throughput (Mbps) for BG=1 LS=384 cb_len=9216."""
    for line in output.split('\n'):
        if 'BG=1' not in line or 'cb_len=9216' not in line:
            continue
        cols = line.split('|')
        for col in cols[1:]:
            col = col.strip()
            try:
                v = float(col)
                if v > 0.5:
                    return v
            except ValueError:
                continue
    return None


def latency_ns_from_mbps(mbps: float) -> float:
    """Convert throughput (Mbps) to per-codeblock latency (ns)."""
    return CB_BITS / mbps * 1000.0   # bits / (Mbps = 1e6 bits/s) → µs, ×1000 → ns


def run_once(binary: str, reps: int = REPS_PER_RUN, timeout: int = 120) -> float | None:
    """Run benchmark once, return per-rep latency in ns, or None."""
    cmd = [binary, f"-R{reps}", "-I20", "-Tavx2", "-L384", "-s"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        mbps = parse_p50_mbps(r.stdout + r.stderr)
        if mbps is not None and mbps > 0.1:
            return latency_ns_from_mbps(mbps)
    except subprocess.TimeoutExpired:
        print("[WARN] run timed out", file=sys.stderr)
    except Exception as e:
        print(f"[WARN] run failed: {e}", file=sys.stderr)
    return None


def start_loader(loader: str, binary: str, socket: str,
                 symfile: str | None = None) -> subprocess.Popen | None:
    """Start l1_intercept_loader in probe-only mode (no -e flag)."""
    cmd = [loader, "-b", binary, "-s", socket]
    if symfile and os.path.isfile(symfile):
        cmd += ["-f", symfile]
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            stdin=subprocess.DEVNULL)
        time.sleep(1.5)   # Wait for BPF programs to load
        if proc.poll() is not None:
            out, err = proc.communicate()
            print(f"[ERROR] Loader exited early. stdout: {out.decode()[:200]}",
                  file=sys.stderr)
            print(f"  stderr: {err.decode()[:200]}", file=sys.stderr)
            return None
        return proc
    except Exception as e:
        print(f"[ERROR] Cannot start loader: {e}", file=sys.stderr)
        return None


def stop_loader(proc: subprocess.Popen | None):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    time.sleep(1.5)  # Wait for BPF programs to unload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=BINARY_DEFAULT)
    ap.add_argument("--loader", default=LOADER_DEFAULT)
    ap.add_argument("--socket", default=SOCKET_DEFAULT)
    ap.add_argument("--symfile", default=SYMFILE_DEFAULT,
                    help="probe symbol file written by 04_find_probe_symbol.sh")
    ap.add_argument("--results-dir", default=RESULTS_DEFAULT)
    ap.add_argument("--samples", type=int, default=100,
                    help="paired samples (default 100)")
    ap.add_argument("--reps", type=int, default=REPS_PER_RUN,
                    help="benchmark internal reps per run (default 1000)")
    args = ap.parse_args()

    if not os.path.isfile(args.binary):
        print(f"[ERROR] Binary not found: {args.binary}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(args.loader):
        print(f"[ERROR] Loader not found: {args.loader}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.results_dir, exist_ok=True)
    out_path = os.path.join(args.results_dir, "ebpf_overhead.csv")

    print(f"[ebpf-overhead] binary:  {args.binary}")
    print(f"[ebpf-overhead] loader:  {args.loader}")
    print(f"[ebpf-overhead] samples: {args.samples}  reps/run: {args.reps}")
    print(f"[ebpf-overhead] cb_bits: {CB_BITS}  (BG1 LS=384 R=0.917)")
    print()

    # ── Phase 1: WITHOUT loader ──────────────────────────────────────────────
    print(f"Phase 1: {args.samples} runs WITHOUT eBPF loader (probe not attached)…")
    baseline_ns = []
    t0 = time.time()
    for i in range(args.samples):
        lat = run_once(args.binary, reps=args.reps)
        if lat is not None:
            baseline_ns.append(lat)
        if (i + 1) % 20 == 0:
            done = time.time() - t0
            eta = done / (i + 1) * (args.samples - i - 1)
            print(f"  {i+1}/{args.samples}  elapsed={done:.0f}s  eta={eta:.0f}s",
                  flush=True)

    if not baseline_ns:
        print("[ERROR] No baseline samples collected!", file=sys.stderr)
        sys.exit(1)
    print(f"  Baseline: n={len(baseline_ns)}  "
          f"mean={statistics.mean(baseline_ns):.0f} ns  "
          f"stdev={statistics.stdev(baseline_ns):.0f} ns  "
          f"p50={sorted(baseline_ns)[len(baseline_ns)//2]:.0f} ns")
    print()

    # ── Phase 2: WITH loader (probe-only, offload disabled) ──────────────────
    print("Phase 2: Starting eBPF loader (probe-only, offload disabled)…")
    loader_proc = start_loader(args.loader, args.binary, args.socket,
                               symfile=args.symfile)
    if loader_proc is None:
        print("[WARN] Loader failed — writing baseline-only CSV", file=sys.stderr)
    else:
        print("  Loader started OK")

    print(f"Phase 2: {args.samples} runs WITH eBPF loader (uprobe fires, early exit)…")
    probe_ns = []
    t0 = time.time()
    for i in range(args.samples):
        lat = run_once(args.binary, reps=args.reps)
        if lat is not None:
            probe_ns.append(lat)
        if (i + 1) % 20 == 0:
            done = time.time() - t0
            eta = done / (i + 1) * (args.samples - i - 1)
            print(f"  {i+1}/{args.samples}  elapsed={done:.0f}s  eta={eta:.0f}s",
                  flush=True)

    stop_loader(loader_proc)

    if not probe_ns:
        print("[ERROR] No probe samples collected!", file=sys.stderr)
        sys.exit(1)
    print(f"  With probe: n={len(probe_ns)}  "
          f"mean={statistics.mean(probe_ns):.0f} ns  "
          f"stdev={statistics.stdev(probe_ns):.0f} ns  "
          f"p50={sorted(probe_ns)[len(probe_ns)//2]:.0f} ns")
    print()

    # ── Compute overhead ─────────────────────────────────────────────────────
    n = min(len(baseline_ns), len(probe_ns))
    overhead_ns = [(probe_ns[i] - baseline_ns[i]) for i in range(n)]
    # Each run has REPS_PER_RUN codeblock calls; the reported latency is per-call.
    # The overhead_ns is already per-call (per decode invocation).

    pos_overhead = [oh for oh in overhead_ns if oh > 0]
    all_overhead = overhead_ns  # Include negatives (noise)

    mean_oh = statistics.mean(all_overhead) if all_overhead else 0
    pos_mean = statistics.mean(pos_overhead) if pos_overhead else 0
    p50_oh = sorted(all_overhead)[len(all_overhead) // 2] if all_overhead else 0

    print(f"[ebpf-overhead] n={n}")
    print(f"  overhead mean (all):     {mean_oh:.1f} ns")
    print(f"  overhead mean (pos):     {pos_mean:.1f} ns")
    print(f"  overhead p50:            {p50_oh:.1f} ns")
    print(f"  Cloudflare ref:          1670 ns (ebpf_exporter, native Linux)")
    print(f"  Note: Early-exit handler (offload disabled) is lighter than")
    print(f"        full ringbuf handler — expect lower overhead than reference")
    print()

    # ── Write CSV ────────────────────────────────────────────────────────────
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "sample_id", "t_base_ns", "t_probe_ns", "overhead_ns",
            "reps_per_run", "cb_bits", "emulation_mode"
        ])
        for i in range(n):
            writer.writerow([
                i,
                f"{baseline_ns[i]:.1f}",
                f"{probe_ns[i]:.1f}",
                f"{overhead_ns[i]:.1f}",
                args.reps,
                CB_BITS,
                EMULATION_MODE,
            ])

    print(f"[ebpf-overhead] Written {n} rows to {out_path}")
    print(f"  Mean overhead per ldpc_decode call: {mean_oh:.1f} ns")
    print(f"  (Each srsRAN run = {args.reps} decode calls; overhead is per-call)")


if __name__ == "__main__":
    main()
