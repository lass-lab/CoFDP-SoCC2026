#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Filebench + fp-remap experiment runner (Fig. 17).

1. Launches filebench with the given workload (the fileserver personality)
2. Records the throughput of every SAMPLE_WINDOW seconds → result.csv
3. Runs fp-remap on the workload's directory at the times of FP_REMAP_EVENTS
   → events.csv (time, PIDs, wall time of the fp-remap command)
4. After completion, calls plot_result_filebench.py to generate the PDF

The workload must run longer than the last event (fig17-filebench.sh sets its
run line accordingly).

Usage:
  sudo python3 run_experiment_filebench.py --workload /path/to/fileserver.f
       [--fp-remap tools/fp-remap] [--fp3 /home/femu/f2fs_fdp_mount/fp3]
       [--result-dir DIR]
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

REPO_ROOT          = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_WORKLOAD   = os.path.join(REPO_ROOT, "filebench-fdp", "fileserver_f2fs.f")
DEFAULT_FP_REMAP   = os.environ.get("FP_REMAP", os.path.join(REPO_ROOT, "tools", "fp-remap"))
# the directory the fileserver personalities write to ($dir)
DEFAULT_FP3_PATH   = os.path.join(os.environ.get("COFDP_MNT", "/home/femu/f2fs_fdp_mount"), "fp3")
DEFAULT_RESULT_DIR = os.path.join(os.environ.get("RESULT_DIR", "/home/femu/results"),
                                  "result_filebench")
SAMPLE_WINDOW     = 60   # seconds: ops accumulated per sample

# fp-remap schedule: (delay_from_start_sec, pid_list)
FP_REMAP_EVENTS = [
    (600,  [1,3,5]),
    (1200,  [3]),
    (1800,  [1,3,5,7]),
    (2400,  [1]),
    (3000,  [1,3]),
    (3600,  [1,3,5,7]),
]



# ──────────────────────────────────────────────
# Throughput parser
# ──────────────────────────────────────────────

def parse_io_summary(line):
    """Parse filebench IO Summary line.

    Example:
      14.958: IO Summary: 82079 ops 8206.121 ops/s 1172/1173 rd/wr 10125.8mb/s 1.938ms/op

    Returns (timestamp_sec, cumulative_ops) or None.
    """
    m = re.match(r'\s*(\d+\.?\d*):\s+IO Summary:\s+(\d+)\s+ops\b', line)
    if m:
        return float(m.group(1)), int(m.group(2))
    return None


# ──────────────────────────────────────────────
# fp-remap scheduler
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
            log[-1] = (elapsed, rgs, latency_us)  # append latency to log entry
        except Exception as e:
            print(f"  [{elapsed:7.1f}s] !!! fp-remap error: {e}")


# ──────────────────────────────────────────────
# Main experiment
# ──────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Filebench + fp-remap experiment")
    ap.add_argument("--workload",   default=DEFAULT_WORKLOAD,
                    help="Path to filebench workload file (.f)")
    ap.add_argument("--fp-remap",   default=DEFAULT_FP_REMAP,
                    help="Path to the fp-remap tool")
    ap.add_argument("--fp3",        default=DEFAULT_FP3_PATH,
                    help="Directory whose PIDs fp-remap changes (the workload's $dir)")
    ap.add_argument("--result-dir", default=DEFAULT_RESULT_DIR,
                    help="Output directory for results")
    # NOTE: not used; samples always cover SAMPLE_WINDOW seconds. See
    # KNOWN_ISSUES (SCR-M2).
    ap.add_argument("--interval",   type=int, default=SAMPLE_WINDOW,
                    help=f"ignored: samples cover {SAMPLE_WINDOW} s")
    args = ap.parse_args()

    if os.geteuid() != 0:
        sys.exit("Run as root (sudo).")

    if not os.path.exists(args.workload):
        sys.exit(f"Workload file not found: {args.workload}")

    os.makedirs(args.result_dir, exist_ok=True)
    csv_path = os.path.join(args.result_dir, "result.csv")
    pdf_path = os.path.join(args.result_dir, "result.pdf")

    print(f"Workload : {args.workload}")
    print(f"fp-remap : {args.fp_remap}  {args.fp3}")
    print(f"Interval : {SAMPLE_WINDOW}s")
    print(f"Events   :")
    for delay, rgs in FP_REMAP_EVENTS:
        print(f"  t={delay:4d}s  fp_remap {{{','.join(str(r) for r in rgs)}}}")
    print()

    # ── Launch filebench (original .f file, no modification) ──
    print("Starting filebench ...")
    proc = subprocess.Popen(
        ["filebench", "-f", args.workload],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    samples = []          # [(elapsed_sec, kops), ...]
    remap_log = []
    bench_started = threading.Event()
    start_time_box = [None]
    last_kops = [0.0]     # shared: updated by reader, printed by monitor
    prev_summary = [None] # (fb_timestamp, cumulative_ops) — start of current window
    window_start = [None] # (fb_timestamp, cumulative_ops) — reset every SAMPLE_WINDOW

    # ── Reader thread ──
    # filebench prints cumulative IO Summary every ~10s.
    # We accumulate ops over SAMPLE_WINDOW seconds, then record one sample.
    def reader():
        for line in proc.stdout:
            line = line.rstrip()
            if not bench_started.is_set():
                print(f"  [prealloc] {line}")
                if "Running..." in line:
                    start_time_box[0] = time.time()
                    bench_started.set()
                    print(f"\n  >>> Benchmark started (t=0). fp-remap armed.\n")
            else:
                result = parse_io_summary(line)
                if result is not None:
                    fb_ts, cum_ops = result
                    elapsed = time.time() - start_time_box[0]

                    # initialise window on first summary
                    if window_start[0] is None:
                        window_start[0] = (fb_ts, cum_ops)

                    win_ts, win_ops = window_start[0]
                    dt = fb_ts - win_ts

                    # compute instantaneous kops for monitor display
                    if prev_summary[0] is not None:
                        prev_ts, prev_ops = prev_summary[0]
                        d = fb_ts - prev_ts
                        if d > 0:
                            last_kops[0] = ((cum_ops - prev_ops) / d) / 1000.0

                    # record sample once window is full
                    if dt >= SAMPLE_WINDOW:
                        interval_ops = cum_ops - win_ops
                        kops = (interval_ops / dt) / 1000.0
                        last_kops[0] = kops
                        samples.append((round(elapsed), kops))
                        print(f"\n  [sample t={elapsed:6.0f}s]  {interval_ops} ops / {dt:.1f}s = {kops:.3f} KOPS/s  ← recorded")
                        window_start[0] = (fb_ts, cum_ops)  # reset window

                    prev_summary[0] = (fb_ts, cum_ops)

    # ── Monitor thread: print current throughput every second ──
    def monitor():
        bench_started.wait()
        while proc.poll() is None:
            elapsed = time.time() - start_time_box[0]
            print(f"\r  t={elapsed:6.0f}s  {last_kops[0]:.3f} KOPS/s", end="", flush=True)
            time.sleep(1)

    reader_thread = threading.Thread(target=reader, daemon=True)
    reader_thread.start()
    monitor_thread = threading.Thread(target=monitor, daemon=True)
    monitor_thread.start()

    # ── Wait for benchmark to start, then arm fp-remap scheduler ──
    print("  Waiting for pre-allocation ...")
    bench_started.wait()
    start_time = start_time_box[0]

    remap_thread = threading.Thread(
        target=schedule_fp_remap,
        args=(FP_REMAP_EVENTS, args.fp_remap, args.fp3, start_time, remap_log),
        daemon=True,
    )
    remap_thread.start()

    # ── Wait for filebench to finish ──
    try:
        proc.wait()
    except KeyboardInterrupt:
        print("\n  Interrupted.")
        proc.terminate()
        proc.wait()

    reader_thread.join(timeout=5)

    proc.wait()
    total_time = time.time() - start_time
    print(f"\nFilebench finished ({total_time:.0f}s)")

    # ── Save CSV ──
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["time_sec", "kops_sec"])
        for t, k in samples:
            writer.writerow([t, f"{k:.3f}"])

    print(f"  CSV saved → {csv_path}  ({len(samples)} samples)")

    # ── Save events log ──
    events_path = os.path.join(args.result_dir, "events.csv")
    with open(events_path, "w") as f:
        f.write("# time_sec,rg_indices (colon-separated),latency_us\n")
        for entry in remap_log:
            t, rgs = entry[0], entry[1]
            lat = entry[2] if len(entry) > 2 else -1
            f.write(f"{t:.0f},{':'.join(str(r) for r in rgs)},{lat:.1f}\n")
    print(f"  Events saved → {events_path}")

    # ── Plot ──
    plot_script = os.path.join(os.path.dirname(__file__), "plot_result_filebench.py")
    if os.path.exists(plot_script):
        print("\nGenerating PDF ...")
        subprocess.run([
            sys.executable, plot_script,
            "--csv", csv_path,
            "--pdf", pdf_path,
            "--events", events_path,
        ])
    else:
        print(f"\n  plot_result_filebench.py not found, skipping PDF generation.")
        print(f"  Run manually: python3 plot_result_filebench.py --csv {csv_path} --pdf {pdf_path} --events {events_path}")


if __name__ == "__main__":
    main()
