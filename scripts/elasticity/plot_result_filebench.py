#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Plot filebench throughput + fp-remap events graph (Fig. 17).

Input:  result.csv  — columns: time_sec, kops_sec
        events.csv  — fp-remap event log of run_experiment_filebench.py
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


# Allocated RG count over time (derived from events)
# Before any event: fp3 already has 1 RG allocated
# After each event: len(pid_list) RGs allocated
# The RG count changes at each event time
def build_rg_timeline(events, total_duration):
    """Build (times, rg_counts) for step plot."""
    times = [0]
    counts = [1]  # fp3 starts with 1 RG already allocated
    for ev in sorted(events, key=lambda x: x[0]):
        t, rgs = ev[0], ev[1]
        times.append(t)
        counts.append(len(rgs))
    times.append(total_duration)
    counts.append(counts[-1])
    return times, counts


def load_csv(path):
    """Load result.csv → (time_list, ops_list).
    CSV stores kops_sec; multiply by 1000 to get OPS/s."""
    times, ops = [], []
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader, None)
        for row in reader:
            if len(row) < 2:
                continue
            try:
                times.append(float(row[0]))
                ops.append(float(row[1]) * 1000.0)
            except ValueError:
                continue
    return times, ops


def plot(csv_path, pdf_path, events):
    times, ops = load_csv(csv_path)
    if not times:
        sys.exit(f"No data in {csv_path}")

    total_duration = max(times)
    rg_times, rg_counts = build_rg_timeline(events, total_duration)

    # ── Figure setup (paper-quality, one-column two-column A4) ──
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
    # get_xaxis_transform(): x=data coords, y=axes fraction (0=bottom, 1=top)
    xax_trans = ax1.get_xaxis_transform()
    y_arrow = -0.18  # below x-axis in axes fraction
    for ev in events:
        t, rgs = ev[0], ev[1]
        # Blue arrow pointing up (from y_arrow up to axes bottom=0)
        ax1.annotate(
            "",
            xy=(t, 0),        xycoords=xax_trans,
            xytext=(t, y_arrow), textcoords=xax_trans,
            arrowprops=dict(arrowstyle="-|>", color="blue", lw=2.0),
            annotation_clip=False,
        )
        # Label below arrow: the PIDs of the event
        rg_str = "{" + ",".join(str(r) for r in rgs) + "}"
        ax1.text(
            t, y_arrow - 0.06, rg_str,
            transform=xax_trans,
            ha="center", va="top", fontsize=11,
            color="blue", fontweight="bold",
            clip_on=False,
        )

    # ── Legend ──
    line_kops = mpatches.Patch(color="red", label="OPS/s")
    line_rg = plt.Line2D([0], [0], color="black", linewidth=2, linestyle="--",
                          label="Number of Allocated RG")
    arrow_patch = plt.Line2D([0], [0], marker="^", color="blue", linestyle="None",
                              markersize=8, label="fp-remap")
    ax1.legend(handles=[line_rg, line_kops, arrow_patch],
               loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.15), frameon=False)

    ax1.grid(True, alpha=0.3)
    fig.subplots_adjust(bottom=0.25, top=0.88)

    fig.savefig(pdf_path, bbox_inches="tight", dpi=300)
    plt.close(fig)
    print(f"  PDF saved → {pdf_path}")


def main():
    default_result_dir = os.path.join(os.environ.get("RESULT_DIR", "/home/femu/results"),
                                      "result_filebench")

    ap = argparse.ArgumentParser(description="Plot filebench + fp-remap results")
    ap.add_argument("--csv", default=os.path.join(default_result_dir, "result.csv"),
                    help="Path to result.csv (time_sec, kops_sec)")
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
