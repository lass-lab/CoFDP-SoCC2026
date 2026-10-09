#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
CacheLib cachebench + fp-remap experiment runner (Fig. 16(a)).

1. Launches cachebench with the given JSON config, whose cache files are in
   the directory whose PIDs fp-remap changes
2. Parses progress lines for OPS/s and hit ratio
3. Records throughput every SAMPLE_WINDOW seconds → result.csv
4. Runs fp-remap at the times of FP_REMAP_EVENTS → events.csv
5. After completion, calls plot_result_cachelib.py to generate the PDF

Usage:
  sudo python3 run_experiment_cachelib.py [--cachebench PATH] [--config PATH]
       [--fp-remap tools/fp-remap] [--fp3 DIR] [--result-dir DIR]
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
DEFAULT_CACHEBENCH = os.environ.get(
    "CACHEBENCH", os.path.join(REPO_ROOT, "FDP_CACHELIB", "opt", "cachelib", "bin", "cachebench"))
# cache files in fp3/large/ and fp3/small/ of /home/femu/f2fs_fdp_mount
DEFAULT_CONFIG     = os.path.join(REPO_ROOT, "configs", "cachelib", "kvcache_l2_wc", "config.json")
DEFAULT_FP_REMAP   = os.environ.get("FP_REMAP", os.path.join(REPO_ROOT, "tools", "fp-remap"))
DEFAULT_FP3_PATH   = os.path.join(os.environ.get("COFDP_MNT", "/home/femu/f2fs_fdp_mount"), "fp3")
DEFAULT_RESULT_DIR = os.path.join(os.environ.get("RESULT_DIR", "/home/femu/results"),
                                  "result_cachelib")
SAMPLE_WINDOW      = 15   # seconds

# fp-remap schedule: (delay_from_start_sec, pid_list)
FP_REMAP_EVENTS = [
    (2000, [1]),
    (4000, [1,3,5,7]),
    (6000, [1,3]),
    (8000, [1,3,5]),
    (10000, [1]),

]

# ──────────────────────────────────────────────
# cachebench output parser
# ──────────────────────────────────────────────

# Progress lines:
#   05:50:22      50.27K ops completed. Hit Ratio  12.69% (RAM   6.32%, NVM   3.61%)
#   07:28:11       0.18M ops completed. Hit Ratio   5.10% (RAM   2.87%, NVM   2.29%)
#   05:50:07       0.00K ops completed. Hit Ratio -6372.22% (RAM   5.56%, NVM   0.17%)
RE_PROGRESS = re.compile(
    r'(\d{2}:\d{2}:\d{2})\s+'
    r'([\d.]+)([MKk]?)\s+ops completed\.\s+'
    r'Hit Ratio\s+(-?[\d.]+)%'
    r'\s*\(RAM\s+(-?[\d.]+)%,\s*NVM\s+(-?[\d.]+)%\)'
)


def parse_ops_value(num_str, suffix):
    """Parse ops count like '0.18M', '1500K', '1234'."""
    v = float(num_str)
    s = suffix.upper()
    if s == 'M':
        return v * 1_000_000
    elif s == 'K':
        return v * 1_000
    return v


def parse_progress_line(line):
    """Parse cachebench progress line.
    Returns (time_str, cumulative_ops, hit_ratio, ram_ratio, nvm_ratio) or None.
    """
    m = RE_PROGRESS.search(line)
    if m:
        time_str = m.group(1)
        ops = parse_ops_value(m.group(2), m.group(3))
        hit_ratio = float(m.group(4))
        ram_ratio = float(m.group(5))
        nvm_ratio = float(m.group(6))
        return time_str, ops, hit_ratio, ram_ratio, nvm_ratio
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
            log[-1] = (elapsed, rgs, latency_us)
        except Exception as e:
            print(f"  [{elapsed:7.1f}s] !!! fp-remap error: {e}")


# ──────────────────────────────────────────────
# Main experiment
# ──────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="CacheLib cachebench + fp-remap experiment")
    ap.add_argument("--cachebench", default=DEFAULT_CACHEBENCH,
                    help="Path to cachebench binary")
    ap.add_argument("--config",     default=DEFAULT_CONFIG,
                    help="Path to cachebench JSON config")
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

    print(f"cachebench : {args.cachebench}")
    print(f"config     : {args.config}")
    print(f"fp-remap   : {args.fp_remap}  {args.fp3}")
    print(f"Sample     : {SAMPLE_WINDOW}s window")
    print(f"Events     :")
    for delay, rgs in FP_REMAP_EVENTS:
        print(f"  t={delay:4d}s  fp_remap {{{','.join(str(r) for r in rgs)}}}")
    print()

    # ── Launch cachebench ──
    cb_cmd = [
        "sudo", args.cachebench,
        "-json_test_config", args.config,
        "--progress", "10"
    ]
    print(f"cmd: {' '.join(cb_cmd)}")
    print()
    print("Starting cachebench ...")

    proc = subprocess.Popen(
        cb_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    samples = []          # [(elapsed_sec, ops_per_sec, hit_ratio), ...]
    remap_log = []
    bench_started = threading.Event()
    start_time_box = [None]
    last_ops_s = [0.0]
    last_hit = [0.0]

    # ── Reader thread ──
    def reader():
        prev_ops = [None]       # previous cumulative ops
        prev_wall = [None]      # previous wall time
        window_start_wall = [None]
        window_start_ops = [None]

        for line in proc.stdout:
            line = line.rstrip()

            if not bench_started.is_set():
                print(f"  [setup] {line}")
                # "Total X ops to be run" or first progress line marks start
                parsed = parse_progress_line(line)
                if parsed:
                    start_time_box[0] = time.time()
                    window_start_wall[0] = start_time_box[0]
                    _, cum_ops, _, _, _ = parsed
                    prev_ops[0] = cum_ops
                    prev_wall[0] = start_time_box[0]
                    window_start_ops[0] = cum_ops
                    bench_started.set()
                    print(f"\n  >>> Benchmark started (t=0). fp-remap armed.\n")
                    # Don't skip this first line - it's the first data point
                continue

            parsed = parse_progress_line(line)
            if parsed:
                _, cum_ops, hit_ratio, ram_ratio, nvm_ratio = parsed
                now = time.time()
                elapsed = now - start_time_box[0]

                # Compute instantaneous OPS/s from delta
                if prev_ops[0] is not None and prev_wall[0] is not None:
                    dt = now - prev_wall[0]
                    if dt > 0:
                        last_ops_s[0] = (cum_ops - prev_ops[0]) / dt
                last_hit[0] = hit_ratio

                # Check if window is full
                window_elapsed = now - window_start_wall[0]
                if window_elapsed >= SAMPLE_WINDOW:
                    delta_ops = cum_ops - window_start_ops[0]
                    avg_ops_s = delta_ops / window_elapsed
                    last_ops_s[0] = avg_ops_s
                    samples.append((round(elapsed), avg_ops_s, hit_ratio))
                    print(f"\n  [sample t={elapsed:6.0f}s]  {delta_ops:.0f} ops / {window_elapsed:.1f}s = {avg_ops_s:.0f} OPS/s  hit={hit_ratio:.1f}%  ← recorded")
                    window_start_wall[0] = now
                    window_start_ops[0] = cum_ops

                prev_ops[0] = cum_ops
                prev_wall[0] = now
            else:
                if line.strip():
                    print(f"  [cb] {line}")

    # ── Monitor thread ──
    def monitor():
        bench_started.wait()
        while proc.poll() is None:
            elapsed = time.time() - start_time_box[0]
            print(f"\r  t={elapsed:6.0f}s  {last_ops_s[0]:.0f} OPS/s  hit={last_hit[0]:.1f}%", end="", flush=True)
            time.sleep(1)

    reader_thread = threading.Thread(target=reader, daemon=True)
    reader_thread.start()
    monitor_thread = threading.Thread(target=monitor, daemon=True)
    monitor_thread.start()

    # ── Wait for benchmark to start, then arm fp-remap scheduler ──
    print("  Waiting for setup ...")
    bench_started.wait()
    start_time = start_time_box[0]

    remap_thread = threading.Thread(
        target=schedule_fp_remap,
        args=(FP_REMAP_EVENTS, args.fp_remap, args.fp3, start_time, remap_log),
        daemon=True,
    )
    remap_thread.start()

    # ── Wait for cachebench to finish ──
    try:
        proc.wait()
    except KeyboardInterrupt:
        print("\n  Interrupted.")
        proc.terminate()
        proc.wait()

    reader_thread.join(timeout=5)

    total_time = time.time() - start_time
    print(f"\ncachebench finished ({total_time:.0f}s)")

    # ── Save CSV ──
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["time_sec", "ops_sec", "hit_ratio_pct"])
        for t, ops_s, hr in samples:
            writer.writerow([t, f"{ops_s:.0f}", f"{hr:.2f}"])

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
    plot_script = os.path.join(os.path.dirname(__file__), "plot_result_cachelib.py")
    if os.path.exists(plot_script):
        print("\nGenerating PDF ...")
        subprocess.run([
            sys.executable, plot_script,
            "--csv", csv_path,
            "--pdf", pdf_path,
            "--events", events_path,
        ])
    else:
        print(f"\n  plot_result_cachelib.py not found, skipping PDF generation.")
        print(f"  Run manually: python3 plot_result_cachelib.py --csv {csv_path} --pdf {pdf_path} --events {events_path}")


if __name__ == "__main__":
    main()
