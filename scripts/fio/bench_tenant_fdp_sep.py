#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Multi-tenant FDP IO benchmark on the raw FDP namespace (Figs. 6 and 7).
Parses the tenant description, launches fio per tenant with FDP placement IDs
(PIDs), and reports per-second throughput, latency, and tail latency.

FDP requirements:
  - fio >= 3.34            (FDP support added Mar 2023)
  - NVMe character device  (/dev/ngXnY)  required for io_uring_cmd ioengine
  - FDP enabled on the device (FEMU's FDP mode always is; a real SSD:
    nvme set-feature /dev/nvmeX -f 0x1D -c 1 -s)

Tenant description (INI file, one section per tenant): qdepth, iosize_byte,
rwratio (fraction of reads), rwpattern (rand | seq), start_time_second,
run_time_second and
  pid=<N>   Placement Identifier index (offset into namespace PID list).
            fio passes this as --fdp_pli=<N> so each tenant's writes land
            in a separate Reclaim Unit on the SSD.
Each tenant gets its own equal share of the namespace's LBAs.

Per-tenant IO uses:
  --ioengine  io_uring_cmd   (bypasses block layer; required for NVMe FDP)
  --cmd_type  nvme
  --fdp       1
  --fdp_pli   <pid>

Usage: sudo python3 bench_tenant_fdp_sep.py [--device /dev/nvme0n1]
       [--desc tenant_desc_fdp_femu] [--log-dir DIR]
"""

import argparse
import configparser
import json
import os
import re
import subprocess
import sys
import threading
import time

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages

DEFAULT_DEVICE = os.environ.get("COFDP_DEV", "/dev/nvme0n1")
DEFAULT_DESC   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tenant_desc_fdp_femu")
DEFAULT_LOG    = os.path.join(os.environ.get("RESULT_DIR", "/home/femu/results"),
                              "fio_tenant_fdp_logs")


# ──────────────────────────────────────────────
# Device helpers
# ──────────────────────────────────────────────

def block_to_ng(block_dev):
    """Convert block device path to NVMe character device path.
    /dev/nvme2n1  →  /dev/ng2n1
    io_uring_cmd (FDP) requires the char device, not the block device.
    """
    m = re.match(r"/dev/nvme(\d+)n(\d+)", block_dev)
    if not m:
        sys.exit(f"Cannot derive ng device from '{block_dev}'. "
                 f"Expected format: /dev/nvmeXnY")
    return f"/dev/ng{m.group(1)}n{m.group(2)}"


def get_device_info(block_dev):
    """Return (ncap_bytes, lba_size).

    Uses `nvme id-ns` to get NCAP (Namespace Capacity) rather than NSZE.
    With FDP enabled, the SSD reserves space for Reclaim Units so
    NCAP < NSZE. Sending IO beyond NCAP returns EINVAL.
    Falls back to blockdev --getsize64 (NSZE) if nvme-cli is unavailable.
    """
    bsz_r = subprocess.run(["blockdev", "--getbsz", block_dev],
                            capture_output=True, text=True, check=True)
    lba_size = int(bsz_r.stdout.strip())

    for fmt_flag in ["-o", "--output-format"]:
        try:
            ns_r = subprocess.run(
                ["nvme", "id-ns", block_dev, fmt_flag, "json"],
                capture_output=True, text=True, check=True)
            ns   = json.loads(ns_r.stdout)
            nsze = int(ns["nsze"])
            ncap = int(ns["ncap"])
            print(f"  [ns]  NSZE={nsze} LBAs ({nsze * lba_size // (1024**3)} GiB)  "
                  f"NCAP={ncap} LBAs ({ncap * lba_size // (1024**3)} GiB usable)")
            if ncap < nsze:
                print(f"  [ns]  FDP reserved {(nsze - ncap) * lba_size // (1024**2)} MiB")
            return ncap * lba_size, lba_size
        except Exception:
            continue

    # both syntaxes failed — show stderr for diagnosis
    ns_r = subprocess.run(["nvme", "id-ns", block_dev, "-o", "json"],
                          capture_output=True, text=True)
    print(f"  [ns]  WARNING: nvme id-ns failed, falling back to blockdev")
    if ns_r.stderr.strip():
        print(f"  [ns]  stderr: {ns_r.stderr.strip()[:300]}")
    size_r = subprocess.run(["blockdev", "--getsize64", block_dev],
                            capture_output=True, text=True, check=True)
    return int(size_r.stdout.strip()), lba_size


# ──────────────────────────────────────────────
# Parsing
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
            "rwratio":           float(cfg[sec]["rwratio"]),  # 1.0=100%read 0.0=100%write
            "rwpattern":         cfg[sec]["rwpattern"],       # seq | rand
            "start_time_second": int(cfg[sec]["start_time_second"]),
            "run_time_second":   int(cfg[sec]["run_time_second"]),
            # FDP: placement identifier index into namespace PID list
            "pid":               int(cfg[sec]["pid"]),
        })
    return tenants


def rw_to_fio(rwpattern, rwratio):
    """Return (fio_rw_string, rwmixread_or_None)."""
    pct = int(rwratio * 100)
    if rwpattern == "rand":
        if pct == 100: return "randread",  None
        if pct == 0:   return "randwrite", None
        return "randrw", pct
    else:  # seq
        if pct == 100: return "read",  None
        if pct == 0:   return "write", None
        return "rw", pct


# ──────────────────────────────────────────────
# Alignment helpers
# ──────────────────────────────────────────────

def align_up(value, align):
    return ((value + align - 1) // align) * align

def align_down(value, align):
    return (value // align) * align


# ──────────────────────────────────────────────
# fio job builder  (FDP mode)
# ──────────────────────────────────────────────

def build_fio_cmd(tenant, ng_device, offset_bytes, size_bytes, lba_size, log_dir):
    """Build fio command with FDP options.

    FDP-specific options:
      --ioengine  io_uring_cmd   (bypasses block layer for NVMe passthrough)
      --cmd_type  nvme
      --fdp       1              (enable FDP; fio reads PIDs from device)
      --fdp_pli   <pid>          (select which PID index this tenant uses)
      filename    /dev/ngXnY    (character device required for io_uring_cmd)

    fio internally sets dtype=2 and dspec=<placement handle> in CDW12/CDW13
    of each NVMe write command when fdp=1 is active.
    """
    name = tenant["name"]
    pid  = tenant["pid"]
    rw, mix = rw_to_fio(tenant["rwpattern"], tenant["rwratio"])

    aligned_bs = align_up(tenant["iosize_byte"], lba_size)
    safe_size  = align_down(size_bytes, aligned_bs)
    if safe_size <= 0:
        safe_size = aligned_bs

    cmd = [
        "fio",
        "--name",            name,
        "--filename",        ng_device,        # char device for io_uring_cmd
        "--offset",          str(offset_bytes),
        "--size",            str(safe_size),
        "--rw",              rw,
        "--bs",              str(aligned_bs),
        "--iodepth",         str(tenant["qdepth"]),
        # io_uring_cmd is NVMe passthrough via char device — O_DIRECT not applicable
        # ── FDP ──────────────────────────────────
        "--ioengine",        "io_uring_cmd",
        "--cmd_type",        "nvme",
        "--fdp",             "1",
        "--fdp_pli",         str(pid),
        # ─────────────────────────────────────────
        "--runtime",         str(tenant["run_time_second"]),
        "--time_based",
        "--output-format",   "json+",
        "--output",          f"{log_dir}/{name}_result.json",
        "--write_lat_log",   f"{log_dir}/{name}",
        "--write_bw_log",    f"{log_dir}/{name}",
        "--write_iops_log",  f"{log_dir}/{name}",
        "--log_avg_msec",    "100",
        "--write_hist_log",  f"{log_dir}/{name}",
        "--log_hist_msec",   "100",
        "--percentile_list", "50:90:99:99.9:99.99",
    ]
    if mix is not None:
        cmd += ["--rwmixread", str(mix)]
    return cmd, aligned_bs


# ──────────────────────────────────────────────
# Runner (one thread per tenant)
# ──────────────────────────────────────────────

def run_tenant(tenant, ng_device, offset, size, lba_size, log_dir, global_start_time, results):
    name  = tenant["name"]
    delay = tenant["start_time_second"]

    wait = global_start_time + delay - time.time()
    if wait > 0:
        time.sleep(wait)

    elapsed = time.time() - global_start_time
    print(f"  [{elapsed:6.1f}s] >>> {name}  pid={tenant['pid']}  started")

    cmd, _ = build_fio_cmd(tenant, ng_device, offset, size, lba_size, log_dir)
    proc = subprocess.run(cmd, capture_output=True, text=True)

    elapsed = time.time() - global_start_time
    if proc.returncode != 0:
        print(f"  [{elapsed:6.1f}s] !!! {name} fio error:\n{proc.stderr}")
        results[name] = None
    else:
        print(f"  [{elapsed:6.1f}s] <<< {name} finished")
        results[name] = True


# ──────────────────────────────────────────────
# Log file parsers
# ──────────────────────────────────────────────

def read_log_split(path):
    """Parse a fio log file and split by direction.
    fio log format: time_ms, value, direction, block_size, offset
      direction 0 = READ,  1 = WRITE

    Returns (read_rows, write_rows) each as list of (second, value).
    """
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


# ──────────────────────────────────────────────
# Display helpers
# ──────────────────────────────────────────────

HDR = (f"{'Sec':>5} | {'RD_BW':>9} {'WR_BW':>9} {'IOPS':>7} | "
       f"{'Lat_avg':>9} {'Lat_p50':>9} {'Lat_p99':>9} {'Lat_p99.9':>10}")
SEP = "-" * len(HDR)

def print_per_second(name, log_dir):
    lat_read,  lat_write  = read_log_split(f"{log_dir}/{name}_lat.1.log")
    bw_read,   bw_write   = read_log_split(f"{log_dir}/{name}_bw.1.log")
    iops_read, iops_write = read_log_split(f"{log_dir}/{name}_iops.1.log")

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
        lats    = all_lat.get(s, [])
        avg_us  = (sum(lats) / len(lats) / 1000) if lats else 0
        p50_us  = compute_percentile(lats, 50)   / 1000
        p99_us  = compute_percentile(lats, 99)   / 1000
        p999_us = compute_percentile(lats, 99.9) / 1000
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
    try:
        with open(path) as f:
            data = json.load(f)
    except json.JSONDecodeError:
        print("  (result file empty or corrupt — fio likely failed)")
        return

    for job in data.get("jobs", []):
        for rw in ("read", "write"):
            io = job.get(rw, {})
            if io.get("io_bytes", 0) == 0:
                continue
            clat = io.get("clat_ns", {})
            pct  = clat.get("percentile", {})

            def ns_to_us(key):
                return pct.get(key, 0) / 1000

            bw_mb    = io.get("bw", 0) / 1024
            iops_v   = io.get("iops", 0)
            avg_us   = clat.get("mean", 0) / 1000
            p50_us   = ns_to_us("50.000000")
            p90_us   = ns_to_us("90.000000")
            p99_us   = ns_to_us("99.000000")
            p999_us  = ns_to_us("99.900000")
            p9999_us = ns_to_us("99.990000")

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

    avg_us   = [pick(all_lat, s) / 1_000_000 for s in raw_sec]           # ns → ms
    p99_us   = [compute_percentile(all_lat.get(s, []), 99)   / 1_000_000 for s in raw_sec]
    p999_us  = [compute_percentile(all_lat.get(s, []), 99.9) / 1_000_000 for s in raw_sec]
    p9999_us = [compute_percentile(all_lat.get(s, []), 99.99)/ 1_000_000 for s in raw_sec]

    rd_bw   = [sum(bw_r.get(s,  [0])) / 1024 for s in raw_sec]
    wr_bw   = [sum(bw_w.get(s,  [0])) / 1024 for s in raw_sec]
    tot_bw  = [r + w for r, w in zip(rd_bw, wr_bw)]
    rd_iops = [sum(iops_r.get(s, [0])) for s in raw_sec]
    wr_iops = [sum(iops_w.get(s, [0])) for s in raw_sec]
    tot_iops= [r + w for r, w in zip(rd_iops, wr_iops)]

    # shift to absolute experiment time
    seconds = [s + start_offset for s in raw_sec]

    return dict(seconds=seconds,
                avg_us=avg_us, p99_us=p99_us, p999_us=p999_us, p9999_us=p9999_us,
                rd_bw=rd_bw, wr_bw=wr_bw, tot_bw=tot_bw,
                rd_iops=rd_iops, wr_iops=wr_iops, tot_iops=tot_iops)


def _save_metric_pdf(pdf_path, all_series, key, ylabel):
    """Save a single-metric PDF (no title, paper-ready)."""
    COLORS = ["#ff7f0e", "#1f77b4", "#2ca02c"]

    fig, ax = plt.subplots(figsize=(7, 3.5))

    for i, (name, s) in enumerate(all_series.items()):
        c = COLORS[i % len(COLORS)]
        ax.plot(s["seconds"], s[key], label=name, linewidth=2.2, color=c)

    ax.set_xlabel("Time (s)", fontsize=13)
    ax.set_ylabel(ylabel,     fontsize=13)
    ax.tick_params(axis="both", labelsize=11)
    ax.set_ylim(bottom=0)
    ax.legend(fontsize=11, framealpha=0.85)
    ax.grid(True, alpha=0.3)
    plt.tight_layout()

    with PdfPages(pdf_path) as pdf:
        pdf.savefig(fig, bbox_inches="tight", dpi=300)
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

    # one PDF per metric, next to pdf_path
    out_dir  = os.path.dirname(pdf_path)
    basename = os.path.splitext(os.path.basename(pdf_path))[0]  # e.g. "bench_fdp_result"

    metrics = [
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

    for key, name, ylabel in metrics:
        out_path = os.path.join(out_dir, f"{basename}_{name}.pdf")
        _save_metric_pdf(out_path, all_series, key, ylabel)


# ──────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Multi-tenant FDP NVMe IO benchmark")
    ap.add_argument("--device",  default=DEFAULT_DEVICE,
                    help="NVMe block device (e.g. /dev/nvme0n1). "
                         "/dev/ngXnY is derived automatically for io_uring_cmd.")
    ap.add_argument("--desc",    default=DEFAULT_DESC)
    ap.add_argument("--log-dir", default=DEFAULT_LOG)
    args = ap.parse_args()

    if os.geteuid() != 0:
        sys.exit("Run as root (sudo).")

    ng_device = block_to_ng(args.device)
    if not os.path.exists(ng_device):
        sys.exit(f"Character device {ng_device} not found.\n"
                 f"Make sure FDP is enabled: sudo nvme set-feature "
                 f"{args.device[:-2]} -f 0x1D -c 1 -s")

    os.makedirs(args.log_dir, exist_ok=True)

    tenants = parse_tenant_desc(args.desc)
    if not tenants:
        sys.exit(f"No tenants found in {args.desc}")

    # LBA space partitioned equally per tenant
    dev_bytes, lba_size = get_device_info(args.device)
    lba_count  = dev_bytes // lba_size
    lba_per_t  = lba_count // len(tenants)
    part_size  = align_down(lba_per_t * lba_size, 1 << 20)  # align down to 1 MiB
    total_secs = max(t["start_time_second"] + t["run_time_second"] for t in tenants)

    print(f"Device  : {args.device}  →  {ng_device}  (io_uring_cmd + FDP)")
    print(f"          {dev_bytes // (1024**3)} GiB,  LBA={lba_size}B")
    print(f"Tenants : {len(tenants)}  (LBA separated, {part_size // (1024**2)} MiB/tenant)")
    print(f"Duration: ~{total_secs}s")
    print(f"Logs    : {args.log_dir}\n")
    for i, t in enumerate(tenants):
        rw, mix = rw_to_fio(t["rwpattern"], t["rwratio"])
        print(f"  {t['name']:10s}  pid={t['pid']}  "
              f"offset={i * part_size // (1024**2):6d}MiB  size={part_size // (1024**2)}MiB  "
              f"rw={rw}({mix})  bs={t['iosize_byte']//1024}KiB  "
              f"qd={t['qdepth']}  "
              f"start={t['start_time_second']}s  run={t['run_time_second']}s")
    print()

    results      = {}
    global_start = time.time() + 0.5

    threads = []
    for i, tenant in enumerate(tenants):
        th = threading.Thread(
            target=run_tenant,
            args=(tenant, ng_device, i * part_size, part_size,
                  lba_size, args.log_dir, global_start, results),
            daemon=True,
        )
        th.start()
        threads.append(th)

    # ── Live status bar ──────────────────────────
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

    # ── Results ──────────────────────────────────
    print("\n" + "=" * 80)
    print("RESULTS (per-second)")
    print("=" * 80)
    for tenant in tenants:
        name = tenant["name"]
        print(f"\n{'─'*60}")
        print(f" {name}  pid={tenant['pid']}"
              f"  rw={rw_to_fio(tenant['rwpattern'],tenant['rwratio'])[0]}"
              f"  bs={tenant['iosize_byte']//1024}KiB  qd={tenant['qdepth']}")
        print(f"{'─'*60}")
        print_per_second(name, args.log_dir)

    print("\n" + "=" * 80)
    print("SUMMARY (overall tail latency)")
    print("=" * 80)
    for tenant in tenants:
        print(f"\n {tenant['name']}  pid={tenant['pid']}")
        print_summary(tenant["name"], args.log_dir)

    print(f"\nRaw logs saved to: {args.log_dir}/")

    # ── PDF ──────────────────────────────────────
    pdf_path = os.path.join(args.log_dir, "bench_fdp_result.pdf")
    plot_results(tenants, args.log_dir, pdf_path)


if __name__ == "__main__":
    main()
