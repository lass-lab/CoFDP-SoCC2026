<!-- SPDX-License-Identifier: GPL-2.0-only -->
# ConfFDP (FEMU) presets

The experiments run in a FEMU guest whose FDP SSD is ConfFDP's FDP mode
(`femu_mode=6`, [`ConfFDP/README.md`](../../ConfFDP/README.md), section 7),
started with [`run-fdp.sh`](../../ConfFDP/femu-scripts/run-fdp.sh). The
defaults of `run-fdp.sh` are this preset;
[`cofdp-4rg.env`](cofdp-4rg.env) lists them explicitly.

| Setting | `run-fdp.sh` variable | Value |
|---|---|---|
| user capacity | `ssd_size` | 65536 MiB (64 GiB) |
| channels x dies per channel | `nchs` x `luns_per_ch` | 8 x 8 (64 dies) |
| page / block size | `NAND_PAGE_SIZE_KB` / `NAND_BLOCK_SIZE_MB` | 4 KiB / 16 MiB |
| reclaim groups (RGs) | `rg_number` | 4 (16 dies each) |
| placement handles per RG | `handle_number` | 2 |
| placement IDs (PIDs) | `rg_number * handle_number` | 8 |
| page read / page program | `pg_rd_lat` / `pg_wr_lat` | 40 us / 200 us |
| block erase | `blk_er_lat` | 2 ms |
| channel transfer per page | `ch_xfer_lat` | 25 us |
| GC thresholds | `gc_thres_pcent` / `gc_thres_pcent_high` | 75 % / 95 % |
| guest | `vm_smp` / `vm_mem` | 8 vCPUs / 16 GiB |

PIDs are flat: PID `p` is placement handle `p % 2` of RG `p / 2`, so PIDs 0
and 1 are in RG 0, 2 and 3 in RG 1, 4 and 5 in RG 2, and 6 and 7 in RG 3. A
reclaim unit is one block on every die of an RG.

The recipes in [`scripts/`](../../scripts) follow from this preset:
`mkfs.f2fs -O lost_found` creates the Initial P-Directories `p0` ... `p7`,
PiDir-FS is mounted with `fdp_log_n=8` (`COFDP_NR_PIDS=8`), and PiDir-FS on
XFS is made with `mkfs.xfs -d fdp_pids=8`. The Flexible P-Directories
`pp0pp2pp4pp6` and `pp1pp3pp5pp7` (`pp` is accepted like `fp`) each hold one
placement handle of every RG; fp-remap with the PIDs `1 3 5 7` spreads a
directory over all four RGs, and `3` keeps it in RG 1.

[`cofdp-1rg.env`](cofdp-1rg.env) is the same preset with a single RG of 8
placement handles, so that all PIDs share all dies: the device of Fig. 6 and
of the FEMU RG = 1 row of Fig. 14.

## Running

```sh
cd ConfFDP/build-femu      # scripts/build/femu.sh builds FEMU here
./run-fdp.sh               # the preset (the defaults)

# the same, with the values taken from the file
set -a; . ../../configs/femu/cofdp-4rg.env; set +a; ./run-fdp.sh

# a single RG (Fig. 6, Fig. 14)
set -a; . ../../configs/femu/cofdp-4rg.env; . ../../configs/femu/cofdp-1rg.env; set +a; ./run-fdp.sh
```

The guest image is `$IMGDIR/fdp.qcow2` (`IMGDIR` defaults to `~/images`), and
the guest's SSH port is forwarded to port 8085 of the host. FEMU keeps the
whole SSD in host memory, so the host needs more DRAM than the emulated
capacity plus the guest memory (more than 80 GiB), and enough cores for the 8
vCPUs of the guest plus FEMU's polling and FTL threads.

QEMU's output goes to `build-femu/log`. Every FEMU reset (`tools/fdp-reset`,
sent by each recipe before mkfs and by `scripts/common/finish-run.sh`) prints
the counters accumulated since the previous reset there; compute the write
amplification of each run with `scripts/common/femu-waf.py build-femu/log`.
