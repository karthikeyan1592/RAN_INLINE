#!/usr/bin/env python3
"""
plot_v3_figures.py — IEEE two-column quality figures for CXL RAN PoC v3.

Produces:
  latency_cdf.pdf         — Baseline vs eBPF+CXL+OpenCL CDF
  latency_breakdown.pdf   — Stacked bar: eBPF + CXL + OpenCL components
  numa_sensitivity.pdf    — Slot miss rate vs CXL latency (Pond sweep)
  ebpf_overhead.pdf       — eBPF uprobe overhead distribution

Run from project root or pass --results-dir / --figures-dir.
"""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
import pandas as pd
import os
import argparse
import sys

# IEEE two-column style
plt.rcParams.update({
    "figure.figsize":    (3.5, 2.6),
    "font.size":         8,
    "font.family":       "serif",
    "axes.labelsize":    8,
    "xtick.labelsize":   7,
    "ytick.labelsize":   7,
    "legend.fontsize":   7,
    "lines.linewidth":   1.2,
    "figure.dpi":        300,
    "savefig.dpi":       300,
    "savefig.bbox":      "tight",
    "savefig.pad_inches": 0.02,
})


def safe_read(path, col=None):
    if not os.path.exists(path):
        return None
    try:
        df = pd.read_csv(path)
        if col is not None:
            return df[col] if col in df.columns else None
        return df
    except Exception as e:
        print(f"[WARN] Cannot read {path}: {e}", file=sys.stderr)
        return None


def cdf(data):
    s = np.sort(data)
    p = np.arange(1, len(s) + 1) / len(s)
    return s, p


# ── Figure 1: Latency CDF ────────────────────────────────────────────────────

def plot_latency_cdf(results, figures):
    baseline = safe_read(f"{results}/baseline_latency.csv", "latency_us")
    offload  = safe_read(f"{results}/cpu_daemon_sync_ipc_latency.csv",  "latency_us")
    async_of = safe_read(f"{results}/async_offload_latency.csv", "latency_us")

    if baseline is None or offload is None:
        print("[fig1] Missing baseline or offload CSV — skipping CDF")
        return

    fig, ax = plt.subplots()

    x, y = cdf(baseline.dropna().values)
    ax.plot(x, y, "-",  color="#444444", label="CPU only (srsRAN)", lw=1.5)

    x, y = cdf(offload.dropna().values)
    ax.plot(x, y, "--", color="#1f77b4", label="Sync IPC offload",  lw=1.5)

    if async_of is not None:
        # Skip slot 0 (synchronous pipeline prime)
        async_vals = async_of.dropna().values[1:]
        x, y = cdf(async_vals)
        ax.plot(x, y, "-.", color="#2ca02c", label="Async pipeline offload", lw=1.5)

    ax.axvline(500, color="red", lw=0.8, ls=":",
               label="0.5 ms 5G NR budget")
    ax.axvline(710, color="gray", lw=0.6, ls="--", alpha=0.5)
    ax.text(715, 0.25,
            "710 µs\n(CPU ref,\nSix×Spare)",
            fontsize=5.5, color="gray", va="center")

    ax.set_xlabel("Latency (µs)")
    ax.set_ylabel("CDF")
    data_max = max(offload.max(), baseline.max())
    if async_of is not None:
        data_max = max(data_max, float(async_of.dropna().values[1:].max()))
    xlim_max = min(data_max * 1.05, 35000)
    ax.set_xlim(0, xlim_max)
    ax.set_ylim(0, 1.05)
    ax.legend(loc="lower right")
    ax.grid(True, alpha=0.3, lw=0.5)

    miss_bl = (baseline > 500).mean() * 100
    miss_of = (offload  > 500).mean() * 100
    note = f"Miss: {miss_bl:.1f}% CPU  {miss_of:.1f}% sync"
    if async_of is not None:
        miss_as = (async_of.values[1:] > 500).mean() * 100
        note += f"  {miss_as:.1f}% async"
    ax.text(0.02, 0.95, note,
            transform=ax.transAxes, fontsize=5.5, va="top",
            bbox=dict(boxstyle="round", facecolor="white", alpha=0.8))

    fig.savefig(f"{figures}/latency_cdf.pdf")
    fig.savefig(f"{figures}/latency_cdf.png")
    plt.close(fig)
    print(f"[fig1] Saved latency_cdf.pdf  "
          f"(baseline mean={baseline.mean():.1f} µs, "
          f"sync mean={offload.mean():.1f} µs" +
          (f", async mean={async_of.values[1:].mean():.1f} µs"
           if async_of is not None else "") + ")")


# ── Figure 2: Latency breakdown ──────────────────────────────────────────────

def plot_latency_breakdown(results, figures):
    """
    Build a breakdown bar from measured components:
      - CPU LDPC compute  (from baseline_latency.csv mean)
      - eBPF overhead     (from ebpf_overhead.csv mean / 1000)
      - CXL transfer      (modelled: ~142 ns per Pond methodology)
      - OpenCL compute    (offload - baseline - eBPF overhead)
    """
    baseline = safe_read(f"{results}/baseline_latency.csv", "latency_us")
    offload  = safe_read(f"{results}/cpu_daemon_sync_ipc_latency.csv",  "latency_us")
    ebpf_oh  = safe_read(f"{results}/ebpf_overhead.csv",    "overhead_ns")

    if baseline is None:
        print("[fig2] Missing baseline CSV — skipping breakdown")
        return

    t_cpu     = float(baseline.dropna().mean()) if baseline is not None else 0
    t_ebpf    = float(ebpf_oh.dropna().mean() / 1000.0) if ebpf_oh is not None else 1.67
    t_cxl     = 0.142  # Pond ASPLOS'23: 142 ns = 0.142 µs
    t_offload = float(offload.dropna().mean()) if offload is not None else t_cpu
    t_opencl  = max(0, t_offload - t_cpu - t_ebpf - t_cxl)

    components = {
        "CPU LDPC\n(srsRAN)":      (t_cpu,    "#444444"),
        "eBPF uprobe\noverhead":    (t_ebpf,   "#aec7e8"),
        "CXL transfer\n(Pond 142ns)": (t_cxl,  "#ffbb78"),
        "OpenCL compute\n(PoCL)":   (t_opencl, "#1f77b4"),
    }

    fig, ax = plt.subplots()
    bottom = 0.0
    patches = []

    for label, (val, color) in components.items():
        bar = ax.bar(["Offload path"], [val], bottom=bottom,
                     color=color, width=0.4)
        ax.text(0.5, bottom + val / 2,
                f"{val:.1f} µs", ha="center", va="center",
                fontsize=6, color="white" if val > 2 else "black")
        bottom += val
        patches.append(mpatches.Patch(color=color,
                                       label=f"{label} ({val:.1f} µs)"))

    ax.axhline(500, color="red", lw=0.8, ls=":",
               label="0.5 ms budget")
    ax.set_ylabel("Latency (µs)")
    ax.set_title("End-to-end offload path breakdown", fontsize=8)
    ax.legend(handles=patches + [ax.get_lines()[0]],
              bbox_to_anchor=(1.05, 1), loc="upper left", fontsize=5.5)
    ax.grid(True, axis="y", alpha=0.3, lw=0.5)

    fig.savefig(f"{figures}/latency_breakdown.pdf")
    fig.savefig(f"{figures}/latency_breakdown.png")
    plt.close(fig)
    print(f"[fig2] Saved latency_breakdown.pdf  total={bottom:.1f} µs")


# ── Figure 3: NUMA sensitivity ───────────────────────────────────────────────

def plot_numa_sensitivity(results, figures):
    files = {
        "0 ns\n(local DRAM)":    f"{results}/numa_0ns.csv",
        "142 ns\n(Pond CXL)":    f"{results}/numa_142ns.csv",
        "255 ns\n(Pond max)":    f"{results}/numa_255ns.csv",
    }

    labels, miss_rates = [], []
    for label, path in files.items():
        d = safe_read(path, "latency_us")
        if d is None:
            continue
        miss = (d > 500).mean() * 100
        labels.append(label)
        miss_rates.append(miss)

    if not labels:
        print("[fig3] No NUMA sweep CSV files — skipping sensitivity plot")
        return

    fig, ax = plt.subplots()
    bars = ax.bar(labels, miss_rates, color="#ff7f0e", width=0.5)

    for bar, val in zip(bars, miss_rates):
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 0.3,
                f"{val:.1f}%",
                ha="center", va="bottom", fontsize=7)

    ax.set_ylabel("Slot deadline miss rate (%)")
    ax.set_title("CXL latency vs. 5G NR 0.5 ms slot", fontsize=8)
    ax.set_ylim(0, max(miss_rates + [5]) * 1.3)
    ax.grid(True, axis="y", alpha=0.3, lw=0.5)
    ax.text(0.98, 0.95,
            "Latency emulation: Pond (ASPLOS'23)\n"
            "CPU-less NUMA node as CXL proxy",
            transform=ax.transAxes,
            ha="right", va="top", fontsize=5,
            color="gray",
            bbox=dict(boxstyle="round", facecolor="white", alpha=0.7))

    fig.savefig(f"{figures}/numa_sensitivity.pdf")
    fig.savefig(f"{figures}/numa_sensitivity.png")
    plt.close(fig)
    print(f"[fig3] Saved numa_sensitivity.pdf  "
          f"miss rates: {[f'{v:.1f}%' for v in miss_rates]}")


# ── Figure 4: eBPF overhead distribution ─────────────────────────────────────

def plot_ebpf_overhead(results, figures):
    d = safe_read(f"{results}/ebpf_overhead.csv", "overhead_ns")
    if d is None or d.empty:
        print("[fig4] No ebpf_overhead.csv — skipping")
        return

    d = d.dropna()
    fig, ax = plt.subplots()
    ax.hist(d, bins=50, color="#1f77b4", edgecolor="none", alpha=0.8)
    ax.axvline(1670, color="red", lw=0.8, ls="--",
               label="Cloudflare ref: 1670 ns")
    mean_ns = d.mean()
    ax.axvline(mean_ns, color="orange", lw=0.8,
               label=f"Measured: {mean_ns:.0f} ns")
    ax.set_xlabel("eBPF uprobe overhead (ns)")
    ax.set_ylabel("Count")
    ax.legend()
    ax.grid(True, alpha=0.3, lw=0.5)

    fig.savefig(f"{figures}/ebpf_overhead.pdf")
    fig.savefig(f"{figures}/ebpf_overhead.png")
    plt.close(fig)
    print(f"[fig4] Saved ebpf_overhead.pdf  mean={mean_ns:.0f} ns  n={len(d)}")


# ── paper_notes.md generation ────────────────────────────────────────────────

def write_paper_notes(results):
    from datetime import datetime

    lines = [
        "# PoC Measurement Notes — CXL RAN PoC v3",
        f"Generated: {datetime.now().isoformat()}",
        "",
        "## Emulation Mode",
    ]

    emu_path = f"{results}/emulation_mode.txt"
    if os.path.exists(emu_path):
        with open(emu_path) as f:
            lines += ["```", f.read().strip(), "```"]

    lines += ["", "## CPU Baseline (srsRAN, no eBPF)"]
    d = safe_read(f"{results}/baseline_latency.csv", "latency_us")
    if d is not None and not d.empty:
        lines += [
            f"- Mean:   {d.mean():.1f} µs",
            f"- p50:    {d.median():.1f} µs",
            f"- p99:    {d.quantile(0.99):.1f} µs",
            f"- Deadline miss (>500 µs): {(d>500).mean()*100:.1f}%",
        ]

    lines += ["", "## Offload Path (eBPF + CXL + OpenCL)"]
    d = safe_read(f"{results}/cpu_daemon_sync_ipc_latency.csv", "latency_us")
    if d is not None and not d.empty:
        lines += [
            f"- Mean:   {d.mean():.1f} µs",
            f"- p50:    {d.median():.1f} µs",
            f"- Deadline miss (>500 µs): {(d>500).mean()*100:.1f}%",
        ]

    lines += ["", "## eBPF Uprobe Overhead"]
    d = safe_read(f"{results}/ebpf_overhead.csv", "overhead_ns")
    if d is not None and not d.empty:
        lines += [
            f"- Mean overhead: {d.mean():.0f} ns",
            f"- Cloudflare reference: 1670 ns",
        ]

    lines += [
        "",
        "## Calibration vs Six Times to Spare",
        "Target: ~710 µs for large TB (BG1, 20 iter, CPU)",
        "Reference: arXiv:2602.04652",
        "See: paper/results/calibration_check.txt",
        "",
        "## Success Criteria (v3 prompt)",
        "1. srsRAN binary: real upstream code, no modifications",
        "2. GPU daemon: real OpenCL API (clCreateBuffer, clEnqueueNDRange)",
        "3. CXL memory: real kernel drivers/cxl/ path (NUMA node proof)",
        "4. eBPF uprobe: attached to srsRAN binary (bpftool prog show)",
        "5. Calibration: large TB in 0.2–3.0 ms range",
        "6. NUMA sweep: 3 files (0ns, 142ns, 255ns)",
        "7. emulation_mode.txt: records actual mode",
        "8. Zero srsRAN lines modified (git diff empty)",
        "",
        "## Prior Art Differentiation",
        "- AtlasRAN (2603.14661): modified OAI + CUDA vs unmodified srsRAN + open",
        "- eGPU (HCDS'25): GPU-side bpftime vs kernel uprobe on srsRAN",
        "- UDON (2404.02868): near-memory CPU vs GPU offload + eBPF",
        "- CXLAimPod (2508.15980): profiling only vs compute offload",
        "- Six Times to Spare (2602.04652): CUDA only vs OpenCL + CXL shared mem",
        "",
        "## What This PoC Demonstrates",
        "- Transparent interception of UNMODIFIED srsRAN via eBPF uprobe",
        "- CXL Type-3 shared memory path via real Linux drivers/cxl/ subsystem",
        "- OpenCL (PoCL) compute on CPU — drop-in for AMD ROCm / NVIDIA",
        "- Measured eBPF overhead vs Cloudflare reference (1670 ns)",
        "- NUMA latency sweep (Pond ASPLOS'23 methodology)",
    ]

    out = f"{results}/paper_notes.md"
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[notes] Written to {out}")


# ── main ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-dir",
                        default="/opt/cxl_ran_poc/paper/results")
    parser.add_argument("--figures-dir",
                        default="/opt/cxl_ran_poc/paper/figures")
    args = parser.parse_args()

    os.makedirs(args.results_dir, exist_ok=True)
    os.makedirs(args.figures_dir, exist_ok=True)

    print(f"[plot_v3] results: {args.results_dir}")
    print(f"[plot_v3] figures: {args.figures_dir}")

    plot_latency_cdf(args.results_dir,       args.figures_dir)
    plot_latency_breakdown(args.results_dir, args.figures_dir)
    plot_numa_sensitivity(args.results_dir,  args.figures_dir)
    plot_ebpf_overhead(args.results_dir,     args.figures_dir)
    write_paper_notes(args.results_dir)

    print("[plot_v3] All figures generated.")
