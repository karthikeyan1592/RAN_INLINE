#!/usr/bin/env python3
"""Generate publication-quality figures from PoC measurement CSVs.

Figures produced:
  latency_cdf.pdf          -- baseline vs offload paths CDF
  latency_breakdown.pdf    -- measured + projected component stacked bar
  cxl_kernel_path.pdf      -- three-way CXL validation bar chart (replaces numa_sensitivity)
  ebpf_overhead.pdf        -- eBPF overhead BPF-only vs ftrace distribution
"""

import argparse
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
import pandas as pd

SLOT_BUDGET_US = 500
RESULTS_DIR = os.path.join(os.path.dirname(__file__), "..", "paper", "results")


def _rpath(rel):
    return os.path.join(RESULTS_DIR, rel)


def _save(fig, out_dir, name):
    os.makedirs(out_dir, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(out_dir, f"{name}.{ext}"), dpi=300, bbox_inches="tight")
    plt.close(fig)


# ── Figure 1: latency CDF ──────────────────────────────────────────────────

def plot_cdf(out_dir):
    baseline_path = _rpath("cpu_daemon_sync_ipc_latency.csv")
    offload_path  = _rpath("async_offload_latency.csv")

    fig, ax = plt.subplots(figsize=(3.5, 2.5))
    ax.axvline(SLOT_BUDGET_US, color="red", linestyle="--", linewidth=0.8,
               label="0.5 ms slot budget")

    datasets = []
    if os.path.exists(baseline_path):
        df = pd.read_csv(baseline_path)
        col = "latency_us" if "latency_us" in df.columns else df.columns[1]
        datasets.append((df[col].astype(float) / 1000.0, "CPU daemon (sync)", "C0"))
    if os.path.exists(offload_path):
        df = pd.read_csv(offload_path)
        col = "latency_us" if "latency_us" in df.columns else df.columns[1]
        datasets.append((df[col].astype(float) / 1000.0, "CPU daemon (async)", "C1"))

    # Add host-native reference point (single measurement, shown as vertical line)
    ax.axvline(11703 / 1000.0, color="green", linestyle=":", linewidth=0.8,
               label="Host-native srsRAN (11.7 ms/slot)")

    for data, label, color in datasets:
        xs = np.sort(data)
        ys = np.arange(1, len(xs) + 1) / len(xs)
        ax.plot(xs, ys, label=label, color=color, linewidth=1.0)

    ax.set_xscale("log")
    ax.set_xlabel("Latency (ms/slot)", fontsize=8)
    ax.set_ylabel("CDF", fontsize=8)
    ax.set_title("Per-slot latency CDF", fontsize=9)
    ax.legend(fontsize=6)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    _save(fig, out_dir, "latency_cdf")


# ── Figure 2: latency breakdown stacked bar ────────────────────────────────

def plot_breakdown(out_dir):
    # Measured components (µs/slot)
    components = [
        ("Host-native\n(srsRAN)", 11703, 0, 0, "Measured"),
        ("Sync offload\n(CPU daemon)", 11703, 333, 0, "Measured"),
        ("Async offload\n(ideal)", 11703, 24, 0, "Measured"),
        ("GPU compute\n(projected)", 1951, 287, 0, "Projected"),
    ]
    labels = [c[0] for c in components]
    base   = [c[1] for c in components]
    ipc    = [c[2] for c in components]
    x      = np.arange(len(labels))

    fig, ax = plt.subplots(figsize=(4.5, 3.0))
    bars_base = ax.bar(x, base, label="LDPC compute", color="steelblue")
    bars_ipc  = ax.bar(x, ipc, bottom=base, label="IPC/eBPF overhead", color="tomato")

    ax.axhline(SLOT_BUDGET_US, color="red", linestyle="--", linewidth=0.9,
               label="500 µs slot budget")

    # Annotate GPU bar with "5.8x under budget" note
    gpu_total = base[3] + ipc[3]
    ax.annotate(f"{gpu_total} µs\n(4.5× over budget)",
                xy=(x[3], gpu_total), xytext=(x[3]+0.15, gpu_total + 800),
                fontsize=6, arrowprops=dict(arrowstyle="-", lw=0.5))

    # Annotate host-native
    ax.annotate(f"11703 µs\n(23.4× over budget)",
                xy=(x[0], base[0]), xytext=(x[0]+0.1, base[0] + 800),
                fontsize=6, arrowprops=dict(arrowstyle="-", lw=0.5))

    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=7)
    ax.set_ylabel("Latency (µs/slot)", fontsize=8)
    ax.set_title("Per-slot latency breakdown (MCS28, 273 PRB, C=24 CBs)", fontsize=8)
    ax.legend(fontsize=6, loc="upper right")
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    _save(fig, out_dir, "latency_breakdown")


# ── Figure 3: CXL kernel-path validation (replaces numa_sensitivity) ───────

def plot_cxl_kernel_path(out_dir):
    envs  = ["Host-native\n(no QEMU)", "VM node 0\n(DRAM)", "VM node 1\n(CXL emulated)"]
    us_per_slot = [11703, 16145, 17417]
    colors = ["steelblue", "mediumseagreen", "darkorange"]

    fig, ax = plt.subplots(figsize=(4.0, 3.0))
    bars = ax.bar(envs, us_per_slot, color=colors, width=0.5)

    # KVM overhead brace
    ax.annotate("",
                xy=(0.95, 16145), xytext=(0.95, 11703),
                xycoords=("data", "data"),
                arrowprops=dict(arrowstyle="<->", color="black", lw=0.8))
    ax.text(1.02, (11703 + 16145) / 2, "1.38×\nKVM", fontsize=6, va="center")

    # Node0 vs node1 noise annotation
    ax.annotate("",
                xy=(1.95, 17417), xytext=(1.95, 16145),
                xycoords=("data", "data"),
                arrowprops=dict(arrowstyle="<->", color="gray", lw=0.8))
    ax.text(2.05, (16145 + 17417) / 2, "Δ53 µs/CB\n(noise)", fontsize=5.5,
            va="center", color="gray")

    ax.set_ylabel("Latency (µs/slot)", fontsize=8)
    ax.set_title("CXL kernel-path validation\n(BG1 LS=384 AVX2 I=20, median of 3 runs)",
                 fontsize=8)

    note = ("Node0 vs node1 Δ = 53 µs/CB within ±100 µs noise band\n"
            "QEMU memory-backend-file does not model CXL.mem latency")
    ax.text(0.5, -0.22, note, ha="center", va="top", transform=ax.transAxes,
            fontsize=5.5, color="gray", style="italic")

    ax.tick_params(labelsize=7)
    ax.set_ylim(0, max(us_per_slot) * 1.25)
    fig.tight_layout()
    _save(fig, out_dir, "cxl_kernel_path")


# ── Figure 4: eBPF overhead decomposition ─────────────────────────────────

def plot_ebpf_overhead(out_dir):
    # Three-point isolation: baseline / BPF-map / ftrace
    labels = ["No probe\n(T_base)", "BPF map counter\n(T_bpfmap)", "ftrace recording\n(T_ftrace)"]
    values_ns = [2.1, 9942.8, 9618.8]
    colors = ["steelblue", "darkorange", "tomato"]

    fig, ax = plt.subplots(figsize=(4.0, 2.8))
    bars = ax.bar(labels, values_ns, color=colors, width=0.5)

    ax.axhline(1700, color="gray", linestyle="--", linewidth=0.8,
               label="Bare-metal ref: 1.7 µs (Cloudflare)")
    ax.axhline(9941, color="darkorange", linestyle=":", linewidth=0.7,
               label=f"raw_overhead = {9941} ns")

    # 5.8× annotation
    ax.annotate(f"5.8× vs bare-metal\n(KVM VM-exit/entry cost)",
                xy=(1, 9942.8), xytext=(1.3, 6000),
                fontsize=6, arrowprops=dict(arrowstyle="->", lw=0.6))

    ax.set_ylabel("Overhead (ns/call)", fontsize=8)
    ax.set_title("eBPF uprobe overhead isolation\n(probe_bench 100 k iterations, droplet KVM host)",
                 fontsize=8)
    ax.legend(fontsize=6)
    ax.tick_params(labelsize=7)
    fig.tight_layout()
    _save(fig, out_dir, "ebpf_overhead")


# ── main ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default=os.path.join(
        os.path.dirname(__file__), "..", "paper", "figures"))
    # Legacy positional args kept for compatibility
    parser.add_argument("--baseline", default=None)
    parser.add_argument("--offload",  default=None)
    args = parser.parse_args()

    out = os.path.abspath(args.output_dir)
    print(f"Writing figures to {out}")

    plot_cdf(out)
    print("  latency_cdf.{png,pdf}")

    plot_breakdown(out)
    print("  latency_breakdown.{png,pdf}")

    plot_cxl_kernel_path(out)
    print("  cxl_kernel_path.{png,pdf}")

    plot_ebpf_overhead(out)
    print("  ebpf_overhead.{png,pdf}")

    print("Done.")


if __name__ == "__main__":
    main()
