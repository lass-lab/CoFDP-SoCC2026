#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Plot CacheLib cachebench throughput + fp-remap events graph (Fig. 16(a)).
Same style as plot_result_filebench.py.

Input:  result.csv  — columns: time_sec, ops_sec, hit_ratio_pct
        events.csv  — fp-remap event log of run_experiment_cachelib.py
                      (default: next to result.csv)

Output: result.pdf (default: next to result.csv)
"""

import argparse
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches


def build_rg_timeline(events, total_duration):
    """Build (times, rg_counts) for step plot.
    fp3 starts with 4 RGs allocated (fp-remap fp3 1 3 5 7 before the run)."""
    times = [0]
    counts = [4]
    for ev in sorted(events, key=lambda x: x[0]):
        t, rgs = ev[0], ev[1]
        times.append(t)
        counts.append(len(rgs))
    times.append(total_duration)
    counts.append(counts[-1])
    return times, counts


def load_csv(path):
    """Load result.csv → (time_list, ops_list, hit_ratio_list)."""
    times, ops, hits = [], [], []
    with open(path) as f:
        reader = csv.reader(f)
        next(reader, None)  # skip header
        for row in reader:
            if len(row) < 2:
                continue
            try:
                times.append(float(row[0]))
                ops.append(float(row[1]))
                if len(row) > 2:
                    hits.append(float(row[2]))
                else:
                    hits.append(0.0)
            except ValueError:
                continue
    return times, ops, hits


def plot(csv_path, pdf_path, events):
    times, ops, hits = load_csv(csv_path)
    if not times:
        sys.exit(f"No data in {csv_path}")

    total_duration = max(times)
    rg_times, rg_counts = build_rg_timeline(events, total_duration)

    # ── Figure setup (paper-quality) ──
    plt.rcParams.update({
        "font.size": 14,
        "axes.labelsize": 16,
        "axes.titlesize": 16,
        "xtick.labelsize": 13,
        "ytick.labelsize": 13,
        "legend.fontsize": 12,
        "font.family": "serif",
    })

    fig, ax1 = plt.subplots(figsize=(8, 3.5))

    # ── Left Y-axis: OPS/s (red solid line) ──
    ax1.plot(times, ops, color="red", linewidth=1.5, label="OPS/s")
    ax1.set_xlabel("Time (sec)")
    ax1.set_ylabel("OPS/s", color="black")
    ax1.tick_params(axis="y", labelcolor="black")
    ax1.set_ylim(bottom=0)
    ax1.set_xlim(left=0, right=total_duration)

    # ── Right Y-axis: Allocated RG # (black dashed step) ──
    ax2 = ax1.twinx()
    ax2.step(rg_times, rg_counts, where="post",
             color="black", linewidth=2.0, linestyle="--", label="Number of Allocated RG")
    ax2.set_ylabel("Allocated RG #", color="black")
    ax2.tick_params(axis="y", labelcolor="black")
    max_rg = max(rg_counts) if rg_counts else 4
    ax2.set_ylim(bottom=0, top=max_rg + 0.5)
    ax2.set_yticks(range(0, max_rg + 1))

    # ── fp-remap event arrows (blue) ──
    xax_trans = ax1.get_xaxis_transform()
    y_arrow = -0.18
    for ev in events:
        t, rgs = ev[0], ev[1]
        ax1.annotate(
            "",
            xy=(t, 0),        xycoords=xax_trans,
            xytext=(t, y_arrow), textcoords=xax_trans,
            arrowprops=dict(arrowstyle="-|>", color="blue", lw=2.0),
            annotation_clip=False,
        )
        rg_str = "{" + ",".join(str(r) for r in rgs) + "}"
        ax1.text(
            t, y_arrow - 0.06, rg_str,
            transform=xax_trans,
            ha="center", va="top", fontsize=11,
            color="blue", fontweight="bold",
            clip_on=False,
        )

    # ── Legend ──
    line_ops = mpatches.Patch(color="red", label="OPS/s")
    line_rg = plt.Line2D([0], [0], color="black", linewidth=2, linestyle="--",
                          label="Number of Allocated RG")
    arrow_patch = plt.Line2D([0], [0], marker="^", color="blue", linestyle="None",
                              markersize=8, label="fp-remap")
    ax1.legend(handles=[line_rg, line_ops, arrow_patch],
               loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.15), frameon=False)

    ax1.grid(True, alpha=0.3)
    fig.subplots_adjust(bottom=0.25, top=0.88)

    fig.savefig(pdf_path, bbox_inches="tight", dpi=300)
    plt.close(fig)
    print(f"  PDF saved → {pdf_path}")


def main():
    default_result_dir = os.path.join(os.environ.get("RESULT_DIR", "/home/femu/results"),
                                      "result_cachelib")

    ap = argparse.ArgumentParser(description="Plot CacheLib + fp-remap results")
    ap.add_argument("--csv", default=os.path.join(default_result_dir, "result.csv"),
                    help="Path to result.csv")
    ap.add_argument("--pdf",
                    help="Output PDF path (default: result.pdf next to --csv)")
    ap.add_argument("--events",
                    help="fp-remap event log of the run (default: events.csv next to --csv)")
    args = ap.parse_args()

    run_dir = os.path.dirname(os.path.abspath(args.csv))
    pdf_path = args.pdf or os.path.join(run_dir, "result.pdf")
    events_path = args.events or os.path.join(run_dir, "events.csv")
    if not os.path.exists(events_path):
        sys.exit(f"No fp-remap event log {events_path}: pass the events.csv of the run with --events")

    events = []
    with open(events_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            t = float(parts[0])
            rgs = [int(x) for x in parts[1].split(":")]
            lat = float(parts[2]) if len(parts) > 2 else -1
            events.append((t, rgs, lat))

    plot(args.csv, pdf_path, events)


if __name__ == "__main__":
    main()
