#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write amplification of each run from the counter dumps in a ConfFDP log.

Every FEMU reset (tools/fdp-reset, sent by the recipes and by
scripts/common/finish-run.sh) prints the counters accumulated since the
previous reset to FEMU's standard output (build-femu/log on the host; the
whole format is in ConfFDP/README.md, section 7). After a banner, the RU
geometry and the reclaim units of every RG, which are not used here, a dump
has these lines:

    RG <r> channel <a>-<b> / lun <c>-<d>             one line per RG
    RG <r> STREAM <h> : written : <pages>            one line per PID
    print_sungjin(ssd->sungjin_stat.block_erased) : {<blocks>}
    print_sungjin(ssd->sungjin_stat.copied) : {<pages>}
    ...
    INITIALIZED current time: <date>

"written" is the number of 4 KiB pages the host wrote with PID
r * handle_number + h, "copied" the number of pages garbage collection copied,
so the write amplification factor is

    WAF = 1 + copied / sum(written)

The "print_sungjin(sum_written)" line of the dump is always 0 (KNOWN_ISSUES
FEMU-8), so the written pages are summed from the STREAM lines. The STREAM
lines FEMU prints when it starts, before any reset, are not a dump.

Usage: femu-waf.py [-p] <FEMU log>
"""

import argparse
import re
import sys

RE_RG = re.compile(r"RG (\d+) channel \d+-\d+ / lun \d+-\d+")
RE_STREAM = re.compile(r"RG (\d+) STREAM (\d+) : written :\s*(\d+)")
RE_STAT = re.compile(r"print_sungjin\(ssd->sungjin_stat\.(\w+)\) : \{(-?\d+)\}")
RE_TIME = re.compile(r"INITIALIZED current time: (.*\S)")


def parse_dumps(lines):
    """Yield one dict per complete dump: written {(rg, handle): pages}, stats, time."""
    dump = None
    for line in lines:
        if RE_RG.search(line):
            if dump is None:
                dump = {"written": {}, "stats": {}, "time": ""}
            continue
        if dump is None:
            continue
        m = RE_STREAM.search(line)
        if m:
            dump["written"][(int(m.group(1)), int(m.group(2)))] = int(m.group(3))
            continue
        m = RE_STAT.search(line)
        if m:
            dump["stats"][m.group(1)] = int(m.group(2))
            continue
        m = RE_TIME.search(line)
        if m:
            dump["time"] = m.group(1)
            yield dump
            dump = None


def main():
    ap = argparse.ArgumentParser(
        description="WAF = 1 + copied / sum(written) of every FEMU counter dump")
    ap.add_argument("log", help="FEMU log (QEMU standard output)")
    ap.add_argument("-p", "--per-pid", action="store_true",
                    help="also print the pages written per PID")
    args = ap.parse_args()

    with open(args.log, errors="replace") as f:
        dumps = list(parse_dumps(f))
    if not dumps:
        sys.exit(f"no counter dump in {args.log}")

    print(f"{'dump':>4}  {'written':>12}  {'copied':>12}  {'erased':>9}  {'WAF':>7}  time")
    for i, d in enumerate(dumps, 1):
        written = sum(d["written"].values())
        copied = d["stats"].get("copied", 0)
        erased = d["stats"].get("block_erased", 0)
        waf = f"{1 + copied / written:7.3f}" if written else f"{'-':>7}"
        print(f"{i:>4}  {written:>12}  {copied:>12}  {erased:>9}  {waf}  {d['time']}")
        if args.per_pid and d["written"]:
            handles = max(h for _, h in d["written"]) + 1
            for (rg, h), pages in sorted(d["written"].items(),
                                         key=lambda e: e[0][0] * handles + e[0][1]):
                print(f"{'':>6}PID {rg * handles + h:>2} (RG {rg}, handle {h}): {pages}")


if __name__ == "__main__":
    main()
