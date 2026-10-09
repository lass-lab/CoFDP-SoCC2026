#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Plot-only script for bench_tenant_fdp_sep.py results (Figs. 6 and 7): one
small PDF per metric, without legend.

Usage:
  python3 bench_tenant_fdp_sep_plot.py --desc tenant_desc_fdp_femu --log-dir DIR
       [--out-dir DIR/plots] [--prefix bench_fdp_result]
"""

import argparse
import configparser
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages


# ──────────────────────────────────────────────
# Tenant desc parser
# ──────────────────────────────────────────────

def parse_tenant_desc(path):
    cfg = configparser.ConfigParser()
    cfg.read(path)
    tenants = []
    for sec in cfg.sections():
        tenants.append({
            "name":              sec,
            "qdepth":            int(cfg[sec]["qdepth"]),
            "iosize_byte":       int(cfg[sec]["iosize_byte"]),
            "rwratio":           float(cfg[sec]["rwratio"]),
            "rwpattern":         cfg[sec]["rwpattern"],
            "start_time_second": int(cfg[sec]["start_time_second"]),
            "run_time_second":   int(cfg[sec]["run_time_second"]),
            "pid":               int(cfg[sec]["pid"]),
        })
    return tenants


# ──────────────────────────────────────────────
# Log parsers
# ──────────────────────────────────────────────

def read_log_split(path):
    reads, writes = [], []
    if not os.path.exists(path):
        return reads, writes
    with open(path) as f:
        for line in f:
            parts = line.split(",")
            if len(parts) < 3:
                continue
            try:
                t_s = int(parts[0]) // 1000
                val = float(parts[1].strip())
                direction = int(parts[2].strip())
                if direction == 0:
                    reads.append((t_s, val))
                else:
                    writes.append((t_s, val))
            except ValueError:
                continue
    return reads, writes


def group_by_second(rows):
    d = {}
    for t, v in rows:
        d.setdefault(t, []).append(v)
    return d


def compute_percentile(values, pct):
    if not values:
        return 0.0
    s = sorted(values)
    idx = max(0, int(len(s) * pct / 100) - 1)
    return s[idx]


def load_tenant_series(name, log_dir, start_offset=0):
    lat_r_raw,  lat_w_raw  = read_log_split(f"{log_dir}/{name}_lat.1.log")
    bw_r_raw,   bw_w_raw   = read_log_split(f"{log_dir}/{name}_bw.1.log")
    iops_r_raw, iops_w_raw = read_log_split(f"{log_dir}/{name}_iops.1.log")

    lat_r  = group_by_second(lat_r_raw)
    lat_w  = group_by_second(lat_w_raw)
    bw_r   = group_by_second(bw_r_raw)
    bw_w   = group_by_second(bw_w_raw)
    iops_r = group_by_second(iops_r_raw)
    iops_w = group_by_second(iops_w_raw)

    all_lat = {}
    for sec, vs in lat_r.items():
        all_lat.setdefault(sec, []).extend(vs)
    for sec, vs in lat_w.items():
        all_lat.setdefault(sec, []).extend(vs)

    raw_sec = sorted(set(all_lat) | set(bw_r) | set(bw_w) | set(iops_r) | set(iops_w))
    if not raw_sec:
        return None
    raw_sec = raw_sec[:-2]  # drop last second (fio teardown artifact)
    if not raw_sec:
        return None

    def pick(d, sec):
        vs = d.get(sec, [])
        return sum(vs) / len(vs) if vs else 0.0

    avg_ms   = [pick(all_lat, s) / 1_000_000 for s in raw_sec]
    p99_ms   = [compute_percentile(all_lat.get(s, []), 99)    / 1_000_000 for s in raw_sec]
    p999_ms  = [compute_percentile(all_lat.get(s, []), 99.9)  / 1_000_000 for s in raw_sec]
    p9999_ms = [compute_percentile(all_lat.get(s, []), 99.99) / 1_000_000 for s in raw_sec]

    rd_bw   = [sum(bw_r.get(s,  [0])) / 1024 for s in raw_sec]
    wr_bw   = [sum(bw_w.get(s,  [0])) / 1024 for s in raw_sec]
    tot_bw  = [r + w for r, w in zip(rd_bw, wr_bw)]
    rd_iops = [sum(iops_r.get(s, [0])) for s in raw_sec]
    wr_iops = [sum(iops_w.get(s, [0])) for s in raw_sec]
    tot_iops= [r + w for r, w in zip(rd_iops, wr_iops)]

    seconds = [s + start_offset for s in raw_sec]

    return dict(seconds=seconds,
                avg_us=avg_ms, p99_us=p99_ms, p999_us=p999_ms, p9999_us=p9999_ms,
                rd_bw=rd_bw, wr_bw=wr_bw, tot_bw=tot_bw,
                rd_iops=rd_iops, wr_iops=wr_iops, tot_iops=tot_iops)


# ──────────────────────────────────────────────
# Plot
# ──────────────────────────────────────────────

COLORS = ["#ff7f0e", "#1f77b4", "#2ca02c"]

METRICS = [
    ("avg_us",   "Average_Completion_Latency", "Latency (ms)"),
    ("p99_us",   "p99_Tail_Latency",           "Latency (ms)"),
    ("p999_us",  "p99.9_Tail_Latency",         "Latency (ms)"),
    ("p9999_us", "p99.99_Tail_Latency",        "Latency (ms)"),
    ("rd_bw",    "Read_Throughput",            "BW (MB/s)"),
    ("wr_bw",    "Write_Throughput",           "BW (MB/s)"),
    ("tot_bw",   "Total_Throughput",           "BW (MB/s)"),
    ("rd_iops",  "Read_IOPS",                 "IOPS"),
    ("wr_iops",  "Write_IOPS",                "IOPS"),
    ("tot_iops", "Total_IOPS",                "IOPS"),
]


def save_metric_pdf(pdf_path, all_series, key, ylabel):
    fig, ax = plt.subplots(figsize=(4,1.7))

    for i, (name, s) in enumerate(all_series.items()):
        ax.plot(s["seconds"], s[key],
                label=name, linewidth=2.2, color=COLORS[i % len(COLORS)])

    ax.set_xlabel("Time (s)", fontsize=13,labelpad=-5)
    ax.xaxis.set_label_coords(0.25,-0.2)
    ax.set_ylabel(ylabel,     fontsize=13)
    ax.tick_params(axis="both", labelsize=13)
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)
    plt.tight_layout()

    with PdfPages(pdf_path) as pdf:
        pdf.savefig(fig, bbox_inches="tight", dpi=300)
    plt.close(fig)
    print(f"  → {pdf_path}")


def main():
    ap = argparse.ArgumentParser(description="Plot bench_tenant_fdp_sep results")
    ap.add_argument("--desc",    required=True, help="Tenant desc file (e.g. tenant_desc_fdp)")
    ap.add_argument("--log-dir", required=True, help="fio log directory (e.g. result)")
    ap.add_argument("--out-dir", default=None,  help="Output dir for PDFs (default: same as log-dir)")
    ap.add_argument("--prefix",  default="bench_fdp_result", help="PDF filename prefix")
    args = ap.parse_args()

    out_dir = args.out_dir or args.log_dir
    os.makedirs(out_dir, exist_ok=True)

    tenants = parse_tenant_desc(args.desc)
    if not tenants:
        sys.exit(f"No tenants in {args.desc}")

    all_series = {}
    for t in tenants:
        name = t["name"]
        s = load_tenant_series(name, args.log_dir, start_offset=t["start_time_second"])
        if s is None:
            print(f"  [skip] no data for {name}")
        else:
            all_series[name] = s

    if not all_series:
        sys.exit("No data found.")

    print(f"Tenants : {list(all_series.keys())}")
    print(f"Out dir : {out_dir}\n")

    for key, label, ylabel in METRICS:
        pdf_path = os.path.join(out_dir, f"{args.prefix}_{label}.pdf")
        save_metric_pdf(pdf_path, all_series, key, ylabel)


if __name__ == "__main__":
    main()
