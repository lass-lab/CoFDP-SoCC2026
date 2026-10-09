#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Multi-tenant fio benchmark on a file system (Fig. 14).

Each tenant of the tenant description (INI file, one section per tenant) runs
one fio job on its own file, e.g. /home/femu/f2fs_fdp_mount/fp1/testfile, and
starts and stops at its own time. The tenants' directories are P-Directories
whose PIDs scripts/tenants/fig14.sh sets with fp-remap before the run.

Tenant keys: qdepth, iosize_byte, rwratio (fraction of reads), rwpattern
(rand | seq), direct (default 1), start_time_second, run_time_second,
filepath.

The script prints per-second bandwidth, IOPS and latency of every tenant and
a summary, and writes one PDF per metric into the log directory. The
per-second percentiles are computed from fio's latency logs, which hold
averages over --log-avg-msec windows.

Usage:
  sudo python3 bench_tenant_sep_fs.py [--desc tenant_desc_fs] [--log-dir DIR]
       [--filesize 4g] [--log-avg-msec 10]
  python3 bench_tenant_sep_fs.py --plot-only --log-dir DIR [--desc tenant_desc_fs]
"""

import argparse
import configparser
import json
import os
import subprocess
import sys
import threading
import time

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

DEFAULT_DESC     = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tenant_desc_fs")
DEFAULT_LOG      = os.path.join(os.environ.get("RESULT_DIR", "/home/femu/results"),
                                "fio_tenant_logs_fs")
DEFAULT_FILESIZE = "4g"


# ──────────────────────────────────────────────
# Parsing
# ──────────────────────────────────────────────

def parse_tenant_desc(path):
    cfg = configparser.ConfigParser()
    cfg.read(path)
    tenants = []
    for sec in cfg.sections():
        tenants.append({
            "name":               sec,
            "qdepth":             int(cfg[sec]["qdepth"]),
            "iosize_byte":        int(cfg[sec]["iosize_byte"]),
            "rwratio":            float(cfg[sec]["rwratio"]),
            "rwpattern":          cfg[sec]["rwpattern"],
            "direct":             int(cfg[sec].get("direct", "1")),
            "start_time_second":  int(cfg[sec]["start_time_second"]),
            "run_time_second":    int(cfg[sec]["run_time_second"]),
            "filepath":           cfg[sec]["filepath"],
        })
    return tenants


def rw_to_fio(rwpattern, rwratio):
    """Return (fio_rw_string, rwmixread_or_None)."""
    pct = int(rwratio * 100)
    if rwpattern == "rand":
        if pct == 100: return "randread",  None
        if pct == 0:   return "randwrite", None
        return "randrw", pct
    else:
        if pct == 100: return "read",  None
        if pct == 0:   return "write", None
        return "rw", pct


# ──────────────────────────────────────────────
# fio job builder (file-system version)
# ──────────────────────────────────────────────

def build_fio_cmd(tenant, filepath, filesize, log_dir, log_avg_msec):
    name = tenant["name"]
    rw, mix = rw_to_fio(tenant["rwpattern"], tenant["rwratio"])

    cmd = [
        "fio",
        "--name",           name,
        "--filename",       filepath,
        "--size",           filesize,
        "--rw",             rw,
        "--bs",             str(tenant["iosize_byte"]),
        "--iodepth",        str(tenant["qdepth"]),
        "--direct",         str(tenant["direct"]),
        "--ioengine",       "io_uring",
        "--runtime",        str(tenant["run_time_second"]),
        "--time_based",
        "--output-format",  "json+",
        "--output",         f"{log_dir}/{name}_result.json",
        "--write_lat_log",  f"{log_dir}/{name}",
        "--write_bw_log",   f"{log_dir}/{name}",
        "--write_iops_log", f"{log_dir}/{name}",
        "--log_avg_msec",   str(log_avg_msec),
        "--percentile_list", "50:90:99:99.9:99.99",
    ]
    if mix is not None:
        cmd += ["--rwmixread", str(mix)]
    return cmd


# ──────────────────────────────────────────────
# Runner (one thread per tenant)
# ──────────────────────────────────────────────

def run_tenant(tenant, filepath, filesize, log_dir, log_avg_msec, global_start_time, results):
    name  = tenant["name"]
    delay = tenant["start_time_second"]

    wait = global_start_time + delay - time.time()
    if wait > 0:
        time.sleep(wait)

    elapsed = time.time() - global_start_time
    print(f"  [{elapsed:6.1f}s] >>> {name} started  ({filepath})")

    cmd = build_fio_cmd(tenant, filepath, filesize, log_dir, log_avg_msec)
    proc = subprocess.run(cmd, capture_output=True, text=True)

    elapsed = time.time() - global_start_time
    if proc.returncode != 0:
        print(f"  [{elapsed:6.1f}s] !!! {name} fio error:\n{proc.stderr[:400]}")
        results[name] = None
    else:
        print(f"  [{elapsed:6.1f}s] <<< {name} finished")
        results[name] = True


# ──────────────────────────────────────────────
# Log file parsers
# ──────────────────────────────────────────────

def read_log(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            parts = line.split(",")
            if len(parts) < 2:
                continue
            try:
                t_s = int(parts[0]) // 1000
                val = float(parts[1].strip())
                rows.append((t_s, val))
            except ValueError:
                continue
    return rows


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


# ──────────────────────────────────────────────
# Display helpers
# ──────────────────────────────────────────────

HDR = (f"{'Sec':>5} | {'RD_BW':>9} {'WR_BW':>9} {'IOPS':>7} | "
       f"{'Lat_avg':>9} {'Lat_p50':>9} {'Lat_p99':>9} {'Lat_p99.9':>10}")
SEP = "-" * len(HDR)

# NOTE: fio's log suffix .N is the job index, not the I/O direction (that is
# the third column of each record). Every tenant runs one job, so all records
# are in the *.1.log files and are reported as reads; latency and the totals
# are not affected. See KNOWN_ISSUES (SCR-12).
def print_per_second(name, log_dir):
    lat_read  = read_log(f"{log_dir}/{name}_lat.1.log")
    lat_write = read_log(f"{log_dir}/{name}_lat.2.log")
    bw_read   = read_log(f"{log_dir}/{name}_bw.1.log")
    bw_write  = read_log(f"{log_dir}/{name}_bw.2.log")
    iops_read = read_log(f"{log_dir}/{name}_iops.1.log")
    iops_write= read_log(f"{log_dir}/{name}_iops.2.log")

    all_lat = group_by_second(lat_read + lat_write)
    bw_r    = {t: sum(vs)/1024 for t, vs in group_by_second(bw_read).items()}
    bw_w    = {t: sum(vs)/1024 for t, vs in group_by_second(bw_write).items()}
    iops    = {t: sum(vs) for t, vs in group_by_second(iops_read + iops_write).items()}

    seconds = sorted(set(all_lat) | set(bw_r) | set(bw_w) | set(iops))
    if not seconds:
        print("  (no log data)")
        return

    print(HDR)
    print(SEP)
    for s in seconds:
        lats   = all_lat.get(s, [])
        avg_us = (sum(lats) / len(lats) / 1000) if lats else 0
        p50_us = compute_percentile(lats, 50)  / 1000
        p99_us = compute_percentile(lats, 99)  / 1000
        p999_us= compute_percentile(lats, 99.9)/ 1000
        print(
            f"{s:>5} | "
            f"{bw_r.get(s,0):>8.2f}M {bw_w.get(s,0):>8.2f}M {iops.get(s,0):>7.0f} | "
            f"{avg_us:>8.1f}µ {p50_us:>8.1f}µ {p99_us:>8.1f}µ {p999_us:>9.1f}µ"
        )


def print_summary(name, log_dir):
    path = f"{log_dir}/{name}_result.json"
    if not os.path.exists(path):
        print("  (no result file)")
        return
    with open(path) as f:
        data = json.load(f)

    for job in data.get("jobs", []):
        for rw in ("read", "write"):
            io = job.get(rw, {})
            if io.get("io_bytes", 0) == 0:
                continue
            clat = io.get("clat_ns", {})
            pct  = clat.get("percentile", {})

            def ns_to_us(key):
                return pct.get(key, 0) / 1000

            bw_mb   = io.get("bw", 0) / 1024
            iops_v  = io.get("iops", 0)
            avg_us  = clat.get("mean", 0) / 1000
            p50_us  = ns_to_us("50.000000")
            p90_us  = ns_to_us("90.000000")
            p99_us  = ns_to_us("99.000000")
            p999_us = ns_to_us("99.900000")
            p9999_us= ns_to_us("99.990000")

            print(
                f"  [{rw:5s}] BW={bw_mb:7.1f} MB/s  IOPS={iops_v:7.0f}  "
                f"avg={avg_us:8.1f}µs  "
                f"p50={p50_us:8.1f}µs  p90={p90_us:8.1f}µs  "
                f"p99={p99_us:8.1f}µs  p99.9={p999_us:9.1f}µs  p99.99={p9999_us:9.1f}µs"
            )


# ──────────────────────────────────────────────
# PDF plot
# ──────────────────────────────────────────────

def load_tenant_series(name, log_dir, start_offset=0):
    lat_r  = group_by_second(read_log(f"{log_dir}/{name}_lat.1.log"))
    lat_w  = group_by_second(read_log(f"{log_dir}/{name}_lat.2.log"))
    bw_r   = group_by_second(read_log(f"{log_dir}/{name}_bw.1.log"))
    bw_w   = group_by_second(read_log(f"{log_dir}/{name}_bw.2.log"))
    iops_r = group_by_second(read_log(f"{log_dir}/{name}_iops.1.log"))
    iops_w = group_by_second(read_log(f"{log_dir}/{name}_iops.2.log"))

    all_lat = {}
    for sec, vs in lat_r.items():
        all_lat.setdefault(sec, []).extend(vs)
    for sec, vs in lat_w.items():
        all_lat.setdefault(sec, []).extend(vs)

    raw_sec = sorted(set(all_lat) | set(bw_r) | set(bw_w) | set(iops_r) | set(iops_w))
    if not raw_sec:
        return None
    # Drop the first and last 1 second (warmup/teardown noise)
    raw_sec = raw_sec[1:-1]
    if not raw_sec:
        return None

    def pick(d, sec):
        vs = d.get(sec, [])
        return sum(vs) / len(vs) if vs else 0.0

    avg_us   = [pick(all_lat, s) / 1000 for s in raw_sec]
    p99_us   = [compute_percentile(all_lat.get(s, []), 99)   / 1000 for s in raw_sec]
    p999_us  = [compute_percentile(all_lat.get(s, []), 99.9) / 1000 for s in raw_sec]
    p9999_us = [compute_percentile(all_lat.get(s, []), 99.99)/ 1000 for s in raw_sec]

    rd_bw   = [sum(bw_r.get(s,  [0])) / 1024 for s in raw_sec]
    wr_bw   = [sum(bw_w.get(s,  [0])) / 1024 for s in raw_sec]
    tot_bw  = [r + w for r, w in zip(rd_bw, wr_bw)]
    rd_iops = [sum(iops_r.get(s, [0])) for s in raw_sec]
    wr_iops = [sum(iops_w.get(s, [0])) for s in raw_sec]
    tot_iops= [r + w for r, w in zip(rd_iops, wr_iops)]

    seconds = [s + start_offset for s in raw_sec]

    return dict(seconds=seconds,
                avg_us=avg_us, p99_us=p99_us, p999_us=p999_us, p9999_us=p9999_us,
                rd_bw=rd_bw, wr_bw=wr_bw, tot_bw=tot_bw,
                rd_iops=rd_iops, wr_iops=wr_iops, tot_iops=tot_iops)


TENANT_COLORS = {
    "tenant0": "#FFCE9F",  # orange
    "tenant1": "#66B2FF",  # blue
    "tenant2": "#97D077",  # green
}
FALLBACK_COLORS = ["#FFCE9F", "#66B2FF", "#97D077", "#e377c2"]

LAT_KEYS = {"avg_us", "p99_us", "p999_us", "p9999_us"}


def _save_metric_page_pdf(pdf_path, all_series, key, ylabel):
    plt.rcParams.update({
        "font.family":      "sans-serif",
        "font.size":        12,
        "axes.labelsize":   13,
        "xtick.labelsize":  11,
        "ytick.labelsize":  11,
        "legend.fontsize":  11,
        "axes.linewidth":   0.8,
    })

    fig, ax = plt.subplots(figsize=(5, 2.5))

    fallback_idx = 0
    for name, s in all_series.items():
        color = TENANT_COLORS.get(name)
        if color is None:
            color = FALLBACK_COLORS[fallback_idx % len(FALLBACK_COLORS)]
            fallback_idx += 1

        # latency: µs → ms
        values = [v / 1000 for v in s[key]] if key in LAT_KEYS else s[key]

        ax.plot(s["seconds"], values, label=name, linewidth=3, color=color)

    ax.set_xlabel("Time (s)", fontsize=13)
    ax.set_ylabel(ylabel, fontsize=13)
    ax.set_ylim(bottom=0)
    ax.legend(loc="lower left", bbox_to_anchor=(-0.15, 1, 1.2, 0.5),
              ncol=3, frameon=False, fontsize=13, mode="expand",
              borderpad=0,
              handletextpad=0.3,
              handlelength=0.7,
              columnspacing=0.5
              )

    ax.grid(True, alpha=0.3, linewidth=0.5)
    fig.tight_layout()
    fig.savefig(pdf_path, bbox_inches="tight", dpi=300)
    plt.close(fig)
    print(f"  PDF saved → {pdf_path}")


def plot_results(tenants, log_dir, pdf_path):
    all_series   = {}
    for tenant in tenants:
        name = tenant["name"]
        s    = load_tenant_series(name, log_dir, start_offset=tenant["start_time_second"])
        if s is None:
            print(f"  [plot] no data for {name}, skipping")
            continue
        all_series[name]   = s

    if not all_series:
        print("  [plot] nothing to plot")
        return

    metrics = [
        ("avg_us",   "avg_lat",   "Latency (ms)"),
        ("p99_us",   "p99_lat",   "Latency (ms)"),
        ("p999_us",  "p999_lat",  "Latency (ms)"),
        ("p9999_us", "p9999_lat", "Latency (ms)"),
        ("rd_bw",    "rd_bw",     "BW (MB/s)"),
        ("wr_bw",    "wr_bw",     "BW (MB/s)"),
        ("tot_bw",   "tot_bw",    "BW (MB/s)"),
        ("rd_iops",  "rd_iops",   "IOPS"),
        ("wr_iops",  "wr_iops",   "IOPS"),
        ("tot_iops", "tot_iops",  "IOPS"),
    ]

    base = pdf_path.replace(".pdf", "")
    for key, suffix, ylabel in metrics:
        out = f"{base}_{suffix}.pdf"
        _save_metric_page_pdf(out, all_series, key, ylabel)

    print(f"  PDFs saved → {base}_*.pdf")


# ──────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Multi-tenant filesystem IO benchmark")
    ap.add_argument("--desc",      default=DEFAULT_DESC)
    ap.add_argument("--log-dir",   default=DEFAULT_LOG)
    ap.add_argument("--filesize",  default=DEFAULT_FILESIZE,
                    help="fio file size per tenant (default: 4g)")
    ap.add_argument("--log-avg-msec", type=int, default=10,
                    help="fio --log_avg_msec: averaging window for bw/iops/lat logs (default: 10)")
    ap.add_argument("--plot-only", action="store_true",
                    help="Skip benchmark, re-plot from existing logs in --log-dir")
    args = ap.parse_args()

    if not args.plot_only and os.geteuid() != 0:
        sys.exit("Run as root (sudo).")

    os.makedirs(args.log_dir, exist_ok=True)

    tenants = parse_tenant_desc(args.desc)
    if not tenants:
        sys.exit(f"No tenants found in {args.desc}")

    pdf_path = os.path.join(args.log_dir, "bench_result.pdf")

    # ── Plot-only mode ──
    if args.plot_only:
        print(f"[plot-only] Reading logs from {args.log_dir}")
        plot_results(tenants, args.log_dir, pdf_path)
        return

    total_secs = max(t["start_time_second"] + t["run_time_second"] for t in tenants)

    print(f"Filesize : {args.filesize} per tenant")
    print(f"Tenants  : {len(tenants)}")
    print(f"Duration : ~{total_secs}s")
    print(f"Logs     : {args.log_dir}\n")
    for t in tenants:
        rw, mix = rw_to_fio(t["rwpattern"], t["rwratio"])
        print(f"  {t['name']:10s}  file={t['filepath']}  "
              f"rw={rw}({mix})  bs={t['iosize_byte']//1024}KiB  "
              f"qd={t['qdepth']}  direct={t['direct']}  "
              f"start={t['start_time_second']}s  run={t['run_time_second']}s")
    print()

    # ── Pre-allocate test files ──
    print("Pre-allocating test files ...")
    for t in tenants:
        fp = t["filepath"]
        if not os.path.exists(fp) or os.path.getsize(fp) == 0:
            d = os.path.dirname(fp)
            os.makedirs(d, exist_ok=True)
            print(f"  {fp} ({args.filesize}) ...", end="", flush=True)
            subprocess.run(
                ["fio", "--name=precreate", f"--filename={fp}",
                 f"--size={args.filesize}", "--rw=write", "--bs=1m",
                 "--ioengine=libaio", "--direct=1", "--iodepth=32",
                 "--runtime=1", "--create_only=1"],
                capture_output=True, text=True,
            )
            print(" done")
        else:
            print(f"  {fp} already exists ({os.path.getsize(fp) // (1024**2)} MiB)")
    print()

    results      = {}
    global_start = time.time() + 0.5

    threads = []
    for tenant in tenants:
        th = threading.Thread(
            target=run_tenant,
            args=(tenant, tenant["filepath"], args.filesize, args.log_dir,
                  args.log_avg_msec, global_start, results),
            daemon=True,
        )
        th.start()
        threads.append(th)

    # ── Live status bar ──
    while any(th.is_alive() for th in threads):
        elapsed = time.time() - global_start
        active  = [t["name"] for t in tenants
                   if t["start_time_second"] <= elapsed
                   < t["start_time_second"] + t["run_time_second"]]
        print(f"\r  t={elapsed:5.1f}s  active={active}      ", end="", flush=True)
        time.sleep(1)
    print()

    for th in threads:
        th.join()

    # ── Results ──
    print("\n" + "=" * 80)
    print("RESULTS (per-second)")
    print("=" * 80)
    for tenant in tenants:
        name = tenant["name"]
        print(f"\n{'─'*60}")
        print(f" {name}  (rw={rw_to_fio(tenant['rwpattern'],tenant['rwratio'])[0]}"
              f"  bs={tenant['iosize_byte']//1024}KiB  qd={tenant['qdepth']})")
        print(f"{'─'*60}")
        print_per_second(name, args.log_dir)

    print("\n" + "=" * 80)
    print("SUMMARY (overall tail latency)")
    print("=" * 80)
    for tenant in tenants:
        print(f"\n {tenant['name']}")
        print_summary(tenant["name"], args.log_dir)

    print(f"\nRaw logs saved to: {args.log_dir}/")

    # ── PDF ──
    plot_results(tenants, args.log_dir, pdf_path)


if __name__ == "__main__":
    main()
