#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
fio + fp-remap experiment runner (Fig. 16(b)).

1. Launches a fio randrw workload (default 1200 s) on a file in the directory
   whose PIDs fp-remap changes
2. Parses fio --status-interval output for periodic throughput
3. Records throughput every SAMPLE_WINDOW seconds → result.csv
4. Runs fp-remap at the times of FP_REMAP_EVENTS → events.csv
5. After completion, calls plot_result_fio.py to generate the PDF

Usage:
  sudo python3 run_experiment_fio.py [--filename FILE] [--fp-remap tools/fp-remap]
       [--fp3 DIR] [--result-dir DIR]
  sudo python3 run_experiment_fio.py --runtime 3600
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import threading
import time

# ──────────────────────────────────────────────
# Configuration
# ──────────────────────────────────────────────

REPO_ROOT         = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MOUNT_POINT       = os.environ.get("COFDP_MNT", "/home/femu/f2fs_fdp_mount")
DEFAULT_FP_REMAP  = os.environ.get("FP_REMAP", os.path.join(REPO_ROOT, "tools", "fp-remap"))
DEFAULT_FP3_PATH  = os.path.join(MOUNT_POINT, "fp3")
DEFAULT_RESULT_DIR = os.path.join(os.environ.get("RESULT_DIR", "/home/femu/results"),
                                  "result_fio")
DEFAULT_FILENAME  = os.path.join(MOUNT_POINT, "fp3", "fiotestfile")
DEFAULT_FILESIZE  = "8g"
DEFAULT_RUNTIME   = 1200
SAMPLE_WINDOW     = 10   # seconds

# fp-remap schedule: (delay_from_start_sec, pid_list)
FP_REMAP_EVENTS = [
    (200,  [1,3,5]),
    (500,  [3]),
    (700,  [1,3,5,7]),
    (1000,  [1,3]),
]

# ──────────────────────────────────────────────
# fio output parser
# ──────────────────────────────────────────────

# fio status-interval lines look like:
#   jobs: 1 (f=1): [m(1)][50.0%][r=1234KiB/s,w=5678KiB/s][r=308,w=1419 IOPS][eta 00m:30s]
#   jobs: 1 (f=1): [m(1)][50.0%][r=12.3MiB/s,w=45.6MiB/s][r=308,w=1419 IOPS][eta 00m:30s]
# We parse read/write BW (convert to MB/s).
RE_BW = re.compile(
    r'\[r=([\d.]+)([KMGkmg]i?B/s),\s*w=([\d.]+)([KMGkmg]i?B/s)\]'
)


def parse_bw_unit(value, unit):
    """Convert fio BW value+unit to MB/s."""
    unit_lower = unit.lower()
    v = float(value)
    if unit_lower.startswith('kib'):
        return v / 1024.0
    elif unit_lower.startswith('kb'):
        return v / 1000.0
    elif unit_lower.startswith('mib'):
        return v
    elif unit_lower.startswith('mb'):
        return v
    elif unit_lower.startswith('gib'):
        return v * 1024.0
    elif unit_lower.startswith('gb'):
        return v * 1000.0
    return v


def parse_status_line(line):
    """Parse fio status line for BW.
    Returns (read_mbps, write_mbps) or None.
    """
    m = RE_BW.search(line)
    if m:
        r_mbps = parse_bw_unit(m.group(1), m.group(2))
        w_mbps = parse_bw_unit(m.group(3), m.group(4))
        return r_mbps, w_mbps
    return None


# ──────────────────────────────────────────────
# fp-remap scheduler (same as filebench version)
# ──────────────────────────────────────────────

# NOTE: an event is recorded in events.csv even when fp-remap fails; the
# failure is only printed. See KNOWN_ISSUES (SCR-11).
def schedule_fp_remap(events, fp_remap_bin, fp3_path, start_time, log):
    """Run fp_remap commands at scheduled times."""
    for delay, rgs in sorted(events, key=lambda x: x[0]):
        wait = start_time + delay - time.time()
        if wait > 0:
            time.sleep(wait)

        rg_args = [str(r) for r in rgs]
        cmd = ["sudo", fp_remap_bin, fp3_path] + rg_args
        elapsed = time.time() - start_time
        print(f"  [{elapsed:7.1f}s] >>> fp-remap {{{','.join(rg_args)}}}")
        print(f"  [{elapsed:7.1f}s]     cmd: {' '.join(cmd)}")
        log.append((elapsed, rgs))

        try:
            t0 = time.time()
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            latency_us = (time.time() - t0) * 1_000_000.0
            if result.returncode != 0:
                print(f"  [{elapsed:7.1f}s] !!! fp-remap failed (rc={result.returncode}, {latency_us:.1f}us)")
                if result.stderr.strip():
                    print(f"  [{elapsed:7.1f}s]     stderr: {result.stderr.strip()[:200]}")
            else:
                print(f"  [{elapsed:7.1f}s]     fp-remap OK  ({latency_us:.1f}us)")
                if result.stdout.strip():
                    print(f"  [{elapsed:7.1f}s]     {result.stdout.strip()[:200]}")
            log[-1] = (elapsed, rgs, latency_us)
        except Exception as e:
            print(f"  [{elapsed:7.1f}s] !!! fp-remap error: {e}")


# ──────────────────────────────────────────────
# Main experiment
# ──────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="fio + fp-remap experiment")
    ap.add_argument("--filename",   default=DEFAULT_FILENAME,
                    help="fio test file path")
    ap.add_argument("--filesize",   default=DEFAULT_FILESIZE,
                    help="fio file size (default: 8g)")
    ap.add_argument("--runtime",    type=int, default=DEFAULT_RUNTIME,
                    help=f"fio runtime in seconds (default: {DEFAULT_RUNTIME})")
    ap.add_argument("--fp-remap",   default=DEFAULT_FP_REMAP,
                    help="Path to the fp-remap tool")
    ap.add_argument("--fp3",        default=DEFAULT_FP3_PATH,
                    help="Directory whose PIDs fp-remap changes")
    ap.add_argument("--result-dir", default=DEFAULT_RESULT_DIR,
                    help="Output directory for results")
    args = ap.parse_args()

    if os.geteuid() != 0:
        sys.exit("Run as root (sudo).")

    os.makedirs(args.result_dir, exist_ok=True)
    csv_path = os.path.join(args.result_dir, "result.csv")
    pdf_path = os.path.join(args.result_dir, "result.pdf")

    print(f"fio file   : {args.filename} ({args.filesize})")
    print(f"Runtime    : {args.runtime}s")
    print(f"fp-remap   : {args.fp_remap}  {args.fp3}")
    print(f"Sample     : {SAMPLE_WINDOW}s window")
    print(f"Events     :")
    for delay, rgs in FP_REMAP_EVENTS:
        print(f"  t={delay:4d}s  fp_remap {{{','.join(str(r) for r in rgs)}}}")
    print()

    # ── Build fio command ──
    fio_cmd = [
        "fio",
        f"--name=fp_remap_test",
        f"--filename={args.filename}",
        f"--size={args.filesize}",
        "--rw=randrw",
        "--rwmixread=50",
        "--bs=1m",
        "--iodepth=32",
        "--numjobs=1",
        "--ioengine=libaio",
        "--direct=1",
        f"--runtime={args.runtime}",
        "--time_based",
        "--group_reporting",
        "--status-interval=10",
        "--eta=always",
    ]

    print(f"fio cmd: {' '.join(fio_cmd)}")
    print()

    # ── Launch fio ──
    print("Starting fio ...")
    proc = subprocess.Popen(
        fio_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    samples = []          # [(elapsed_sec, mbps), ...]
    remap_log = []
    bench_started = threading.Event()
    start_time_box = [None]
    last_mbps = [0.0]

    # ── Reader thread ──
    # Waits for "Jobs:" line (file layout done, actual I/O begins) → t=0
    def reader():
        window_start_time = [None]
        window_bw_samples = []  # [(r_mbps, w_mbps), ...]

        for line in proc.stdout:
            line = line.rstrip()

            if not bench_started.is_set():
                print(f"  [layout] {line}")
                # "Jobs: N (f=N)" marks the start of actual I/O
                if line.strip().startswith("Jobs:"):
                    start_time_box[0] = time.time()
                    window_start_time[0] = start_time_box[0]
                    bench_started.set()
                    print(f"\n  >>> fio I/O started (t=0). fp-remap armed.\n")
                continue

            elapsed = time.time() - start_time_box[0]

            # Try to parse BW from status line
            parsed = parse_status_line(line)
            if parsed:
                r_mbps, w_mbps = parsed
                total_mbps = r_mbps + w_mbps
                last_mbps[0] = total_mbps
                window_bw_samples.append((r_mbps, w_mbps))

                # Check if window is full
                window_elapsed = time.time() - window_start_time[0]
                if window_elapsed >= SAMPLE_WINDOW and window_bw_samples:
                    avg_r = sum(s[0] for s in window_bw_samples) / len(window_bw_samples)
                    avg_w = sum(s[1] for s in window_bw_samples) / len(window_bw_samples)
                    avg_mbps = avg_r + avg_w
                    samples.append((round(elapsed), avg_mbps))
                    print(f"\n  [sample t={elapsed:6.0f}s]  r={avg_r:.1f} w={avg_w:.1f} = {avg_mbps:.1f} MB/s  ← recorded")
                    window_start_time[0] = time.time()
                    window_bw_samples = []
            else:
                # Print non-status lines (summaries, etc.)
                if line.strip():
                    print(f"  [fio] {line}")

    # ── Monitor thread ──
    def monitor():
        bench_started.wait()
        while proc.poll() is None:
            elapsed = time.time() - start_time_box[0]
            print(f"\r  t={elapsed:6.0f}s  {last_mbps[0]:.1f} MB/s", end="", flush=True)
            time.sleep(1)

    reader_thread = threading.Thread(target=reader, daemon=True)
    reader_thread.start()
    monitor_thread = threading.Thread(target=monitor, daemon=True)
    monitor_thread.start()

    # ── Wait for fio I/O to start, then arm fp-remap scheduler ──
    print("  Waiting for file layout ...")
    bench_started.wait()
    start_time = start_time_box[0]

    remap_thread = threading.Thread(
        target=schedule_fp_remap,
        args=(FP_REMAP_EVENTS, args.fp_remap, args.fp3, start_time, remap_log),
        daemon=True,
    )
    remap_thread.start()

    # ── Wait for fio to finish ──
    try:
        proc.wait()
    except KeyboardInterrupt:
        print("\n  Interrupted.")
        proc.terminate()
        proc.wait()

    reader_thread.join(timeout=5)

    total_time = time.time() - start_time
    print(f"\nfio finished ({total_time:.0f}s)")

    # ── Save CSV ──
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["time_sec", "mbps"])
        for t, m in samples:
            writer.writerow([t, f"{m:.1f}"])

    print(f"  CSV saved → {csv_path}  ({len(samples)} samples)")

    # ── Save events log ──
    events_path = os.path.join(args.result_dir, "events.csv")
    with open(events_path, "w") as f:
        f.write("# time_sec,rg_indices (colon-separated)\n")
        for entry in remap_log:
            t, rgs = entry[0], entry[1]
            f.write(f"{t:.0f},{':'.join(str(r) for r in rgs)}\n")
    print(f"  Events saved → {events_path}")

    # ── Plot ──
    plot_script = os.path.join(os.path.dirname(__file__), "plot_result_fio.py")
    if os.path.exists(plot_script):
        print("\nGenerating PDF ...")
        subprocess.run([
            sys.executable, plot_script,
            "--csv", csv_path,
            "--pdf", pdf_path,
            "--events", events_path,
        ])
    else:
        print(f"\n  plot_result_fio.py not found, skipping PDF generation.")
        print(f"  Run manually: python3 plot_result_fio.py --csv {csv_path} --pdf {pdf_path} --events {events_path}")


if __name__ == "__main__":
    main()
