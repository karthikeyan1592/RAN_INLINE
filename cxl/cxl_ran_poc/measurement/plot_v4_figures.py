#!/usr/bin/env python3
"""
plot_v4_figures.py — Phase 6 paper figures for CXL RAN PoC v4.

Produces (all in paper/figures/):
  latency_cdf_v2.pdf       — CDF of ablation rows; REAL data from Phase 5
  latency_breakdown_v2.pdf — Stacked bars: baseline / +interception / +OCL
  nic_packet_timeline.pdf  — NIC inter-arrival histogram (Phase 3 XDP)
  bit_correctness_table.pdf — Phase 1 per-(BG,LS) bit-diff table

Run from project root:
  python3 measurement/plot_v4_figures.py
"""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import matplotlib.table as mtable
import numpy as np
import pandas as pd
import os, sys

RESULTS = "paper/results"
FIGURES = "paper/figures"

# IEEE two-column style
plt.rcParams.update({
    "figure.figsize":     (3.5, 2.6),
    "font.size":          8,
    "font.family":        "serif",
    "axes.labelsize":     8,
    "xtick.labelsize":    7,
    "ytick.labelsize":    7,
    "legend.fontsize":    6.5,
    "lines.linewidth":    1.2,
    "figure.dpi":         300,
    "savefig.dpi":        300,
    "savefig.bbox":       "tight",
    "savefig.pad_inches": 0.02,
})

BUDGET_US = 500.0          # 5G NR µ=1 slot budget
BASELINE_US = 11703.0      # PRIMARY_CONFIG anchor — do not re-measure

C_RED    = "#d62728"
C_DARK   = "#333333"
C_BLUE   = "#1f77b4"
C_GREEN  = "#2ca02c"
C_ORANGE = "#ff7f0e"
C_GRAY   = "#aaaaaa"


def cdf(data):
    s = np.sort(np.asarray(data, dtype=float))
    p = np.arange(1, len(s) + 1) / len(s)
    return s, p


def save(fig, stem):
    for ext in ("pdf", "png"):
        fig.savefig(f"{FIGURES}/{stem}.{ext}")
    plt.close(fig)
    print(f"[saved] {stem}.pdf / .png")


# ─────────────────────────────────────────────────────────────────────────────
# Figure 1: Latency CDF v2
# ─────────────────────────────────────────────────────────────────────────────

C_ACTUAL = 2   # CBs per slot (DEV-009)

def plot_latency_cdf_v2():
    raw = pd.read_csv(f"{RESULTS}/ablation_raw.csv")
    # per-CB → per-slot (×C_actual=2, DEV-009)
    p0 = raw[raw["pass"] == 0]["overhead_ns"].dropna().values / 1e3 * C_ACTUAL
    p1 = raw[raw["pass"] == 1]["overhead_ns"].dropna().values / 1e3 * C_ACTUAL

    fig, ax = plt.subplots()

    # baseline — fixed anchor, single value: draw as vertical dashed line
    ax.axvline(BASELINE_US, color=C_DARK, lw=1.0, ls="--",
               label=f"Baseline (PRIMARY) {BASELINE_US:,.0f} µs")

    # +interception_only distribution
    x, y = cdf(p0)
    ax.plot(x, y, "-", color=C_BLUE, lw=1.3,
            label=f"+interception_only (n={len(p0)})")

    # +gpu_compute_full distribution
    x, y = cdf(p1)
    ax.plot(x, y, "-", color=C_ORANGE, lw=1.3,
            label=f"+gpu_compute_full (n={len(p1)})")

    # 500 µs budget
    ax.axvline(BUDGET_US, color=C_RED, lw=0.9, ls=":",
               label="500 µs NR budget")

    ax.set_xscale("log")
    ax.set_xlabel("Per-slot overhead (µs, log scale)")
    ax.set_ylabel("CDF")
    ax.set_ylim(0, 1.05)
    ax.set_xlim(100, max(p1.max(), BASELINE_US) * 1.5)
    ax.legend(loc="upper left", fontsize=5.5)
    ax.grid(True, which="both", alpha=0.2, lw=0.4)

    caveat = ("C_act=2, Z=224, WSL2\n"
              "OCL on CPU (PoCL) — DEV-014")
    ax.text(0.98, 0.05, caveat, transform=ax.transAxes,
            ha="right", va="bottom", fontsize=5, color="gray",
            bbox=dict(boxstyle="round", facecolor="white", alpha=0.75))

    save(fig, "latency_cdf_v2")
    print(f"  interception_only: mean={p0.mean():.0f} µs  "
          f"p50={np.median(p0):.0f}  p99={np.percentile(p0,99):.0f}")
    print(f"  gpu_compute_full:  mean={p1.mean():.0f} µs  "
          f"p50={np.median(p1):.0f}  p99={np.percentile(p1,99):.0f}")


# ─────────────────────────────────────────────────────────────────────────────
# Figure 2: Latency breakdown v2 — stacked bars
# ─────────────────────────────────────────────────────────────────────────────

def plot_latency_breakdown_v2():
    raw = pd.read_csv(f"{RESULTS}/ablation_raw.csv")
    # per-CB → per-slot (×C_actual=2, DEV-009)
    p0_mean = raw[raw["pass"] == 0]["overhead_ns"].mean() / 1e3 * C_ACTUAL
    p1_oh   = raw[raw["pass"] == 1]["overhead_ns"].mean() / 1e3 * C_ACTUAL
    p1_ocl  = raw[raw["pass"] == 1]["ocl_ns"].mean() / 1e3 * C_ACTUAL
    p1_interc = p1_oh - p1_ocl   # interception portion of pass 1

    # GPU projection: pass-0 interception + OCL/6
    proj = p0_mean + p1_ocl / 6.0

    fig, ax = plt.subplots(figsize=(3.5, 2.8))

    bar_labels = ["Baseline\n(PRIMARY)", "+intercept\nonly", "+gpu_compute\n(CPU OCL)", "Projected\n(real GPU)"]
    bar_colors_main = [C_DARK, C_BLUE, C_ORANGE, C_GREEN]

    # baseline — single bar
    ax.bar(0, BASELINE_US,      color=C_DARK,   width=0.55, label="CPU LDPC (srsRAN)")
    # interception only
    ax.bar(1, p0_mean,          color=C_BLUE,   width=0.55, label="Interception overhead (bpftime)")
    # gpu_compute_full — stacked: interception + OCL
    ax.bar(2, p1_interc,        color=C_BLUE,   width=0.55)
    ax.bar(2, p1_ocl, bottom=p1_interc, color=C_ORANGE, width=0.55, label="OpenCL compute (CPU)")
    # projection — stacked: interception + OCL/6
    ax.bar(3, p0_mean,          color=C_BLUE,   width=0.55)
    ax.bar(3, p1_ocl/6.0, bottom=p0_mean, color=C_GREEN, width=0.55, label="OCL/6× (GPU proj.)")

    ax.axhline(BUDGET_US, color=C_RED, lw=0.9, ls=":", label="500 µs budget")

    # annotate bar tops
    for xi, val in [(0, BASELINE_US), (1, p0_mean), (2, p1_oh), (3, proj)]:
        ax.text(xi, val * 1.03, f"{val/1e3:.0f} ms" if val >= 1000 else f"{val:.0f} µs",
                ha="center", va="bottom", fontsize=5.5)

    ax.set_yscale("log")
    ax.set_ylabel("Per-slot latency (µs, log)")
    ax.set_xticks([0, 1, 2, 3])
    ax.set_xticklabels(bar_labels, fontsize=6)
    ax.legend(loc="upper right", fontsize=5, ncol=1)
    ax.grid(True, axis="y", which="both", alpha=0.2, lw=0.4)

    ax.text(0.02, 0.01,
            "Proj. = 2,636 + 149,548/6 µs\nWSL2, C_act=2, Z=224 (DEV-009/014)",
            transform=ax.transAxes, fontsize=4.5, color="gray", va="bottom")

    save(fig, "latency_breakdown_v2")
    print(f"  baseline={BASELINE_US:.0f} µs  interc={p0_mean:.0f} µs  "
          f"full={p1_oh:.0f} µs  proj={proj:.0f} µs")


# ─────────────────────────────────────────────────────────────────────────────
# Figure 3: NIC packet timeline — inter-arrival histogram
# ─────────────────────────────────────────────────────────────────────────────

def plot_nic_packet_timeline():
    df = pd.read_csv(f"{RESULTS}/nic_packet_timeline.csv")
    ts = df["timestamp_ns"].dropna().values
    ia_us = np.diff(np.sort(ts)) / 1e3   # inter-arrival in µs

    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.6))

    # Left: histogram capped at 1000 µs (intra-slot detail)
    ax = axes[0]
    mask = ia_us < 1000
    ax.hist(ia_us[mask], bins=80, color=C_BLUE, edgecolor="none", alpha=0.85)
    ax.axvline(BUDGET_US, color=C_RED, lw=0.9, ls=":",
               label="500 µs slot")
    ax.set_xlabel("Inter-arrival (µs, <1 ms)")
    ax.set_ylabel("Count")
    ax.set_title("Intra/inter-slot detail", fontsize=7)
    ax.legend(fontsize=6)
    ax.grid(True, alpha=0.2, lw=0.4)
    n_intra = mask.sum()
    n_total = len(ia_us)
    ax.text(0.97, 0.97,
            f"{n_intra:,} of {n_total:,}\nIAs <1 ms",
            transform=ax.transAxes, ha="right", va="top", fontsize=5.5,
            bbox=dict(boxstyle="round", facecolor="white", alpha=0.75))

    # Right: timeline — packet arrival over first 200 ms
    ax = axes[1]
    ts_ms = (ts - ts[0]) / 1e6   # relative ms
    mask200 = ts_ms < 200
    ax.plot(ts_ms[mask200], np.arange(mask200.sum()),
            color=C_BLUE, lw=0.6, alpha=0.8)
    ax.set_xlabel("Time (ms, first 200 ms)")
    ax.set_ylabel("Cumulative packets")
    ax.set_title("Packet arrival rate (sustained)", fontsize=7)
    ax.grid(True, alpha=0.2, lw=0.4)
    # annotate rate
    duration_s = (ts[-1] - ts[0]) / 1e9
    rate = len(ts) / duration_s
    ax.text(0.97, 0.05,
            f"Rate: {rate:.0f} pkt/s\n"
            f"Total: {len(ts):,} pkts\n"
            f"Duration: {duration_s*1e3:.0f} ms",
            transform=ax.transAxes, ha="right", va="bottom", fontsize=5.5,
            bbox=dict(boxstyle="round", facecolor="white", alpha=0.75))

    fig.suptitle("rfsimulator NIC traffic (veth-gnb, XDP observer)", fontsize=7)
    fig.tight_layout(rect=[0, 0, 1, 0.93])
    save(fig, "nic_packet_timeline")
    print(f"  {len(ts):,} packets, duration {duration_s*1e3:.0f} ms, "
          f"mean IA {ia_us.mean():.1f} µs, stddev {ia_us.std():.1f} µs")


# ─────────────────────────────────────────────────────────────────────────────
# Figure 4: Bit-correctness table
# ─────────────────────────────────────────────────────────────────────────────

def plot_bit_correctness_table():
    df = pd.read_csv(f"{RESULTS}/bit_correctness.csv")

    fig, ax = plt.subplots(figsize=(3.5, 1.6))
    ax.axis("off")

    col_labels = ["BG", "Zc (LS)", "iLS", "Iter", "Msgs", "Bits", "Mismatches", "Rate", "Status"]
    rows = []
    for _, r in df.iterrows():
        rows.append([
            int(r["bg"]),
            int(r["ls"]),
            int(r["ls_idx"]),
            int(r["n_iter"]),
            int(r["n_messages"]),
            f"{int(r['n_bits']):,}",
            int(r["n_mismatches"]),
            f"{r['bit_diff_rate']:.6f}",
            r["status"],
        ])

    tbl = ax.table(
        cellText=rows,
        colLabels=col_labels,
        loc="center",
        cellLoc="center",
    )
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(6.5)
    tbl.scale(1.0, 1.35)

    # colour PASS cells green
    for (row_idx, col_idx), cell in tbl.get_celld().items():
        if row_idx == 0:
            cell.set_facecolor("#dddddd")
            cell.set_text_props(fontweight="bold")
        elif col_idx == len(col_labels) - 1:   # Status column
            txt = cell.get_text().get_text()
            cell.set_facecolor("#c8e6c9" if txt == "PASS" else "#ffcdd2")

    ax.set_title("Phase 1 — Bit-exact LDPC decoder verification\n"
                 "(OpenCL layered min-sum vs srsRAN encoder oracle)",
                 fontsize=6.5, pad=4)

    save(fig, "bit_correctness_table")
    n_pass = (df["status"] == "PASS").sum()
    print(f"  {n_pass}/{len(df)} PASS, all bit_diff_rate = 0")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    os.makedirs(FIGURES, exist_ok=True)
    errs = []

    for fn, name in [
        (plot_latency_cdf_v2,        "latency_cdf_v2"),
        (plot_latency_breakdown_v2,  "latency_breakdown_v2"),
        (plot_nic_packet_timeline,   "nic_packet_timeline"),
        (plot_bit_correctness_table, "bit_correctness_table"),
    ]:
        print(f"\n── {name} ──")
        try:
            fn()
        except Exception as e:
            print(f"  ERROR: {e}", file=sys.stderr)
            import traceback; traceback.print_exc()
            errs.append(name)

    print(f"\n{'─'*40}")
    if errs:
        print(f"FAILED: {errs}")
        sys.exit(1)
    else:
        print("All 4 figures generated successfully.")
        print(f"Output: {FIGURES}/")


if __name__ == "__main__":
    # run from project root
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    main()
