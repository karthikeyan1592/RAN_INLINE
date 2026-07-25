#!/usr/bin/env python3
"""Auto-generate paper/notes.md from measurement CSVs."""

import os
from datetime import datetime

import pandas as pd

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RESULTS = os.path.join(ROOT, "paper", "results")
NOTES = os.path.join(ROOT, "paper", "notes.md")


def read_latency(path):
    if not os.path.exists(path):
        return None
    df = pd.read_csv(path)
    return {
        "mean": df["latency_us"].mean(),
        "miss": 100.0 * df["deadline_miss"].mean(),
    }


def main():
    baseline = read_latency(os.path.join(RESULTS, "baseline_latency.csv"))
    offload = read_latency(os.path.join(RESULTS, "offload_latency.csv"))

    cal = ""
    cal_path = os.path.join(RESULTS, "calibration_check.txt")
    if os.path.exists(cal_path):
        cal = open(cal_path).read()

    emulation = "unknown"
    emu_path = os.path.join(RESULTS, "emulation_mode.txt")
    if os.path.exists(emu_path):
        emulation = open(emu_path).read().strip()

    with open(NOTES, "w") as f:
        f.write(f"## Auto-generated Measurement Summary\n\n")
        f.write(f"Generated: {datetime.now().isoformat()}\n\n")
        f.write(f"Emulation: {emulation}\n\n")
        if cal:
            f.write("### Calibration Check\n\n")
            f.write(cal)
            f.write("\n")
        if baseline:
            f.write(f"### CPU-only baseline\n")
            f.write(f"- Mean per-slot latency: {baseline['mean']:.1f} µs\n")
            f.write(f"- Slot deadline miss rate: {baseline['miss']:.1f}%\n\n")
        if offload:
            f.write(f"### eBPF + CXL offload\n")
            f.write(f"- Mean per-slot latency: {offload['mean']:.1f} µs\n")
            f.write(f"- Slot deadline miss rate: {offload['miss']:.1f}%\n\n")
        f.write("### Novelty evidence\n")
        f.write("- L1 application lines changed for offload hook: 0 (weak symbol)\n")
        f.write("- eBPF intercept: ldpc_decode() / fft_process() boundaries\n")
        f.write("- Accelerator: CPU-based daemon (architecture proof)\n")

    print(f"Wrote {NOTES}")


if __name__ == "__main__":
    main()
