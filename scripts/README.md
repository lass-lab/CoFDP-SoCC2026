<!-- SPDX-License-Identifier: GPL-2.0-only -->
# CoFDP experiment scripts

Build scripts and one recipe per experiment of the paper. Except for the FEMU
build and the write-amplification parser, which run on the host, everything
runs in the FEMU guest. The recipes run as root and print every command that
changes the system (`+ ...`) before running it.

| Directory | Contents |
|---|---|
| [`common/`](common) | `env.sh` (settings), `lib.sh` (shared steps), `setup-fs.sh`, `umount-all.sh`, `finish-run.sh`, `femu-waf.py` |
| [`build/`](build) | builds: ConfFDP (FEMU) on the host; in the guest the dependencies, xNVMe, the kernel, f2fs-tools, xfsprogs, the tools, the RocksDB forks, YCSB, CacheLib, Filebench, Solana and Aptos |
| [`rocksdb/`](rocksdb) | db_bench fillrandom + mixgraph; the RocksDB directory layouts (`lib.sh`) |
| [`ycsb/`](ycsb) | YCSB on RocksDB |
| [`cachelib/`](cachelib) | CacheLib cachebench |
| [`filebench/`](filebench) | Filebench video server and OLTP |
| [`blockchain/`](blockchain) | PiDir-Binding of the Solana validator and of the Aptos executor benchmark |
| [`fio/`](fio) | fio tenants on the raw FDP namespace |
| [`tenants/`](tenants) | fio tenants on PiDir-FS (Fig. 14), co-running application tenants (Fig. 15) |
| [`elasticity/`](elasticity) | RGs changed at run time with fp-remap (Figs. 16 and 17) |

The FEMU presets are in [`configs/femu/`](../configs/femu), the CacheLib
configurations in [`configs/cachelib/`](../configs/cachelib), the Filebench
personalities in [`filebench-fdp/`](../filebench-fdp) and the YCSB workloads
in [`YCSB-cpp-fdp/workloads/`](../YCSB-cpp-fdp/workloads).

## Setup

On the host (see [`ConfFDP/README.md`](../ConfFDP/README.md) for the guest
image):

```sh
FEMU_INSTALL_DEPS=1 scripts/build/femu.sh     # ConfFDP/build-femu/qemu-system-x86_64
cd ConfFDP/build-femu && ./run-fdp.sh          # the guest, with the default preset
```

`run-fdp.sh` needs a QEMU built with liburing (`aio=io_uring`). Ubuntu 20.04
has no liburing package: there, install
[liburing](https://github.com/axboe/liburing) from source first (`femu.sh`
stops without it).

In the guest, with this repository checked out anywhere (the scripts find it
from their own location), as a user with sudo rights:

```sh
scripts/build/deps.sh                 # Ubuntu packages
scripts/build/xnvme.sh                # xNVMe 0.7.4
scripts/build/kernel.sh && sudo reboot  # image and all modules (struct bio changed)
scripts/build/f2fs-tools.sh           # mkfs.f2fs -O lost_found
scripts/build/xfsprogs.sh             # mkfs.xfs -d fdp_pids=N
scripts/build/tools.sh                # fp-remap, fdp-reset, fdp-ruh-status, pidirfs-mount
scripts/build/rocksdb.sh all          # db_bench and librocksdb.a of both RocksDB forks
scripts/build/ycsb.sh all             # YCSB-cpp-fdp/ycsb-f2fs and ycsb-torfs
scripts/build/cachelib.sh             # cachebench
scripts/build/filebench.sh            # Filebench
scripts/build/solana.sh               # Solana runs only
scripts/build/aptos.sh                # Aptos runs only
sudo tools/fdp-ruh-status /dev/nvme0n1  # 8 PIDs with the default preset
```

`deps.sh` does not install:

* [liburing](https://github.com/axboe/liburing) >= 2.2, needed before
  `rocksdb.sh`, `ycsb.sh` and `cachelib.sh`: CacheLib needs it, and RocksDB
  and YCSB link it. Ubuntu 20.04 has no liburing package and 22.04 ships 2.1,
  so install it from source there.
* [fio](https://github.com/axboe/fio) >= 3.34 for Figs. 6 and 7
  (`io_uring_cmd` with FDP placement); Ubuntu 20.04 ships fio 3.16.
* [rustup](https://rustup.rs), needed before `solana.sh` (`aptos.sh` installs
  it through Aptos' `dev_setup.sh`).

`scripts/build/kernel-module.sh f2fs|xfs` rebuilds and reloads only the F2FS or
XFS module of a kernel built with `kernel.sh`.

## Running a recipe

```sh
sudo scripts/ycsb/run.sh cofdp workloada      # a run
sudo scripts/common/finish-run.sh             # FEMU prints the counters of the run
scripts/common/femu-waf.py ConfFDP/build-femu/log   # on the host: WAF of each run
```

Each recipe

1. sends the FEMU reset (`tools/fdp-reset`): FEMU prints the counters of the
   previous run and resets the emulated SSD to empty,
2. unmounts every mount of the namespace, including the bind mounts of the
   previous run,
3. sends the FEMU reset again, which discards what the unmount wrote,
4. formats and mounts the file system,
5. creates the P-Directories and binds them into the application's
   directories (PiDir-Binding), and
6. runs the workload with its output in
   `$RESULT_DIR/<recipe>-<stack>-<workload>-<RUN_TAG>.log` (default
   `/home/femu/results`); a `.cmd` file next to it records the command line,
   the working directory, the kernel and the start time.

The counters of a run are printed by the next FEMU reset, i.e. by
`finish-run.sh` or by the first step of the next recipe. Runs take up to hours:
start them in `tmux` or with `nohup`. `COFDP_DRY_RUN=1` prints the commands of
a recipe or build script without running them; a `+ cd <dir>` line shows the
directory each command runs in.

Most recipes take a stack:

| Stack | File system | Placement |
|---|---|---|
| `cofdp` | PiDir-FS on F2FS: `mkfs.f2fs -O lost_found`, mounted with `fdp_log_n=8` | P-Directories bound into the application's directories |
| `kernelfdp-f2fs` | F2FS: `mkfs.f2fs` without `lost_found`, mounted with `fdp_log_n=1` | the kernel picks the PIDs from the F2FS log types |
| `kernelfdp-pc` | PiDir-FS as for `cofdp` | each data class of the application bound to its own Initial P-Directory (one PID) |
| `userfdp` | none | TorFS (RocksDB) or CacheLib's FDP passthrough on the raw namespace |

F2FS is mounted with `fdp_log_n=N,discard,nobarrier,discard_unit=segment`
(`tools/pidirfs-mount N`), XFS with `-o discard` or, for Fig. 14,
`-o discard,allocsize=64k`.

Settings ([`common/env.sh`](common/env.sh); pass them through sudo, e.g.
`sudo COFDP_DEV=/dev/nvme1n1 scripts/...`):

| Variable | Default | Meaning |
|---|---|---|
| `COFDP_DEV` | `/dev/nvme0n1` | FDP namespace |
| `COFDP_MNT` | `/home/femu/f2fs_fdp_mount` | mount point of PiDir-FS |
| `COFDP_TENANT0`, `COFDP_TENANT1` | `/home/femu/tenant0`, `/home/femu/tenant1` | directories into which the recipes bind P-Directories |
| `COFDP_NR_PIDS` | `8` | PIDs of the device: `fdp_log_n` and `mkfs.xfs -d fdp_pids` |
| `COFDP_DEVICE` | `femu` | `nvme-format` for a real FDP SSD: reset with `nvme format`, no counters |
| `RESULT_DIR`, `RUN_TAG` | `/home/femu/results`, the start time | where the output goes |
| `FDP_RESET`, `FP_REMAP`, `PIDIRFS_MOUNT`, `MKFS_F2FS`, `MKFS_XFS`, `CACHEBENCH`, ... | built in this tree | programs |

The CacheLib configurations, the Filebench personalities and the YCSB
properties contain `/home/femu/...` paths literally. The recipes pass changed
paths to YCSB and edit copies of the Filebench files; for CacheLib, pass an
edited configuration as `CACHELIB_CONFIG`.

## Write amplification

Every FEMU reset prints the counters accumulated since the previous reset to
FEMU's output (`ConfFDP/build-femu/log` on the host; format in
[`ConfFDP/README.md`](../ConfFDP/README.md), section 7). In such a dump,
`RG <r> STREAM <h> : written : <pages>` is the number of 4 KiB pages the host
wrote with PID `r * handle_number + h` (`r * 2 + h` with the preset) and
`print_sungjin(ssd->sungjin_stat.copied) : {<pages>}` the number of pages that
garbage collection copied, so

```
WAF = 1 + copied / sum(written)
```

where the sum runs over all `STREAM` lines of the dump. The
`print_sungjin(sum_written)` line is always 0 (KNOWN_ISSUES FEMU-8).
[`common/femu-waf.py`](common/femu-waf.py) prints the WAF of every dump of a
log (`-p` also prints the pages written per PID).

## Figures and tables

| Paper | Workload | Script |
|---|---|---|
| Table 3 | ConfFDP preset | [`configs/femu/`](../configs/femu) |
| Table 2, Figs. 12, 13 | RocksDB fillrandom ("fillran.") and mixgraph | [`rocksdb/run-mixgraph.sh`](rocksdb/run-mixgraph.sh) `<stack>` |
| Table 2, Figs. 12, 13 | YCSB A, B, D, E, F, zipfian, uniform, latest, nutanix | [`ycsb/run.sh`](ycsb/run.sh) `<stack> workloada` ... `workload_nutanix` |
| Table 2, Figs. 12, 13 | CacheLib cdn, kvc, GFL, GAL, GFF, GAF | [`cachelib/run.sh`](cachelib/run.sh) `<stack> cdn` ... `gaf` |
| Table 2, Figs. 12, 13 | Filebench video, oltp | [`filebench/run.sh`](filebench/run.sh) `cofdp\|kernelfdp-f2fs video\|oltp` |
| Table 2, Figs. 12, 13 | Solana | [`blockchain/bind-solana.sh`](blockchain/bind-solana.sh) `cofdp\|single-pid`, then the node ([`blockchain-fdp/`](../blockchain-fdp)) |
| Table 2, Figs. 12, 13 | Aptos | [`blockchain/bind-aptos.sh`](blockchain/bind-aptos.sh) `cofdp\|single-pid\|...`, then the benchmark |
| Fig. 13 | write amplification of all of the above | [`common/finish-run.sh`](common/finish-run.sh), [`common/femu-waf.py`](common/femu-waf.py) |
| Figs. 6, 7 | three fio tenants on the raw FDP namespace | [`fio/run-raw-tenants.sh`](fio/run-raw-tenants.sh), [`fio/bench_tenant_fdp_sep_plot.py`](fio/bench_tenant_fdp_sep_plot.py) |
| Fig. 14 | three fio tenants on PiDir-FS (F2FS, XFS) | [`tenants/fig14.sh`](tenants/fig14.sh) `f2fs\|xfs` |
| Fig. 15 | co-running tenants: Solana, CacheLib GFF and cdn, YCSB-E, mixgraph | [`tenants/fig15-bind.sh`](tenants/fig15-bind.sh), [`tenants/fig15-tenant.sh`](tenants/fig15-tenant.sh) |
| Fig. 16(a) | CacheLib with fp-remap | [`elasticity/fig16a-cachelib.sh`](elasticity/fig16a-cachelib.sh) |
| Fig. 16(b) | fio with fp-remap | [`elasticity/fig16b-fio.sh`](elasticity/fig16b-fio.sh) `xfs\|f2fs` |
| Fig. 17(a), (b) | Filebench fileserver with fp-remap on F2FS, XFS | [`elasticity/fig17-filebench.sh`](elasticity/fig17-filebench.sh) `f2fs\|xfs` |

All recipes run on FEMU started with the preset of
[`configs/femu/`](../configs/femu), except Fig. 6 and the FEMU RG = 1 row of
Fig. 14, which use its single-RG variant `cofdp-1rg.env`. Figs. 2, 4 and 18
have no recipe here.

## Notes on the recipes

**RocksDB and YCSB.** The layouts of the four stacks are described in
[`rocksdb/lib.sh`](rocksdb/lib.sh): for CoFDP the WAL and L0 go to the Flexible
P-Directory `pp0pp2pp4pp6` and the deeper levels to `pp1pp3pp5pp7`; for
KernelFDP-PC they go to the Initial P-Directories `p1`, `p2` and `p3`. The
"fillran." column of Table 2 is the fillrandom phase of `run-mixgraph.sh`.
ycsb runs from `YCSB-cpp-fdp/`, to which the `rocksdb.optionsfile` paths of the
properties files are relative. Unless `YCSB_PHASES` is set, `ycsb/run.sh` runs
`-load` only for the write-only specs `workload_{zipfian,uniform,latest}`,
which set `loadkeydistribution=requestdistribution`; `-run` only for
`workload_nutanix` under `kernelfdp-pc` and `userfdp`; and `-load -run`
otherwise. Every `ycsb` invocation sleeps 720 s after the (possibly skipped)
load phase, also with `-load` or `-run` alone (KNOWN_ISSUES YCSB-3). ycsb
writes latency CSV files to `/home/femu/isol` (YCSB property
`hdrhistogram.csv_dir`).

**CacheLib.** `cachebench --progress=100` with the configuration of the stack
(`cofdp.json`, `kernel.json`, `pc.json` or `kernel.json` for the graph
workloads, `user.json`); see
[`configs/cachelib/README.md`](../configs/cachelib/README.md).

**Filebench.** `video_churn.f` and `oltp.f` use `p4` and `p5` below the mount
point: Initial P-Directories for CoFDP, directories made with `mkdir` for
KernelFDP-F2FS (see also [`filebench-fdp/README.md`](../filebench-fdp/README.md)).

* Filebench needs ASLR off: it maps its shared memory at the same address in
  every worker process, and its own per-process switch has no effect.
  `filebench/run.sh` and `fig17-filebench.sh` therefore first run
  `sysctl -w kernel.randomize_va_space=0`, which lasts until the next reboot.
  With ASLR on, a run fails (`Unexpected Process termination`,
  `NO VALID RESULTS!`), possibly with exit status 0; `filebench/run.sh`
  checks its log for these messages.
* `video` ends by itself.
* `oltp` does not end by itself. The recipe stops it with SIGINT
  `FILEBENCH_OLTP_SECONDS` seconds (default 600) after Filebench prints
  `Running...`. With `FILEBENCH_OLTP_OPS=<n>` it stops it instead as soon as
  an IO Summary reports n or more operations; Table 2 lists oltp with 18M
  operations (`FILEBENCH_OLTP_OPS=18000000`).
* Ctrl-C stops either workload the same way. After SIGINT, Filebench prints
  `Aborting...`, exits with status 1 and prints no final IO Summary. The
  recipe treats this as the normal end: it removes the shared-memory file and
  the semaphore set that Filebench leaves behind and prints the last IO
  Summary.

**Blockchain.** The binding scripts run as root and print how to start the
node afterwards, as the user that runs it. The validator of Solana and the
Aptos benchmark keep their data below `/home/femu/tenant0/p1`; the CoFDP
layouts give their data classes their own Initial P-Directories, and the
`single-pid` layouts put everything on one P-Directory (`SINGLE_PDIR`, default
`fp3` for Solana and `p0` for Aptos). In the CoFDP layout of Solana, the
write-ahead log of the blockstore goes to `tenant0/p3`
(`COFDP_SOLANA_WAL_DIR`, see [`blockchain-fdp/README.md`](../blockchain-fdp/README.md));
the `single-pid` layout leaves the variable unset. The Aptos layouts
`write-set`, `ledger` and `hot-shard` are variants of the CoFDP layout. Some
binds reach the application's directories through mount propagation, which
the scripts check (KNOWN_ISSUES BC-8); see also BC-3 for the Aptos checkpoint.

**Figs. 6 and 7.** `fio/run-raw-tenants.sh` resets the device and starts three
fio tenants with io_uring passthrough and FDP placement. The `pid=` value of a
tenant in [`fio/tenant_desc_fdp_femu`](fio/tenant_desc_fdp_femu) is fio's
`--fdp_pli`, an index into the device's list of placement IDs, which ConfFDP
reports handle by handle (`0 2 4 6 1 3 5 7` with the preset, see
`tools/fdp-ruh-status`). The indices 0, 1 and 2 of tenants A, B and C are
therefore:

| FEMU | PIDs | RGs | Figure |
|---|---|---|---|
| preset, 4 RGs x 2 handles ([`configs/femu/`](../configs/femu)) | 0, 2, 4 | 0, 1, 2: one RG per tenant | Fig. 7 |
| [`configs/femu/cofdp-1rg.env`](../configs/femu/cofdp-1rg.env), 1 RG x 8 handles | 0, 1, 2 | 0 for all tenants | Fig. 6 |

On a real FDP SSD, set `COFDP_DEVICE=nvme-format` and `COFDP_DEV=<namespace>`:
the namespace is formatted instead of reset.

**Fig. 14.** Three fio tenants with direct I/O on files in the P-Directories
`fp1`, `fp2` and `fp3`, which get the PIDs 3, 5 and 7 (`FIG14_PIDS`), one RG
each with the preset. The FEMU RG = 1 row uses the same script with FEMU
started with `configs/femu/cofdp-1rg.env`; the real-SSD row sets
`COFDP_DEVICE=nvme-format`, `COFDP_DEV`, `COFDP_NR_PIDS` and `FIG14_PIDS` (see
the script). `fig14.sh --plot-only <log dir>` draws the PDFs again. In the
per-second output and the PDFs, all I/O is reported as reads (KNOWN_ISSUES
SCR-12); the latency is not affected.

**Fig. 15.** Two tenants with disjoint RGs under CoFDP: `tenant0` gets RGs 0
and 1, `tenant1` RGs 2 and 3. The workloads run in two groups.
`fig15-bind.sh` resets the device and prepares a group, `fig15-tenant.sh`
starts one tenant. Start and stop each tenant at the times of Sec. 5.5 (GFF
0-470 s, Solana 30-950 s, YCSB-E 580-950 s; then cdn and mixgraph
1000-2500 s), e.g. from its own terminal; stop a tenant with Ctrl-C:

```sh
# Solana in tenant0; CacheLib GFF, then YCSB-E in tenant1
sudo scripts/tenants/fig15-bind.sh cofdp solana-gff-ycsbe     # or kernelfdp-pc
sudo scripts/tenants/fig15-tenant.sh gff
sudo scripts/tenants/fig15-tenant.sh ycsbe
# Solana, as the node's user in the patched Solana tree:
export COFDP_TENANT_ROOT=/home/femu/tenant0 COFDP_PDIR=p1; unset COFDP_SOLANA_WAL_DIR
NDEBUG=1 multinode-demo/cofdp/setup.sh
NDEBUG=1 multinode-demo/faucet.sh &
NDEBUG=1 multinode-demo/cofdp/bootstrap-validator.sh &
NDEBUG=1 multinode-demo/cofdp/bench-tps.sh

# mixgraph in tenant0; CacheLib cdn in tenant1
sudo scripts/tenants/fig15-bind.sh cofdp mixgraph-cdn         # or kernelfdp-pc
sudo scripts/tenants/fig15-tenant.sh mixgraph
sudo scripts/tenants/fig15-tenant.sh cdn
```

**Figs. 16 and 17.** The drivers in [`elasticity/`](elasticity) start the
workload, run `tools/fp-remap` on the directory `fp3` at the times listed in
their `FP_REMAP_EVENTS`, sample the throughput, and write `result.csv`,
`events.csv` (time, PIDs and wall time of each fp-remap call) and `result.pdf`.
`elasticity/plot_result_{cachelib,fio,filebench}.py --csv <dir>/result.csv`
draws the PDF of a run again; it reads `events.csv` from the same directory
(`--events` to override) and writes `result.pdf` there (`--pdf`).
`fp3` starts with the PIDs 1, 3, 5 and 7 for CacheLib (four RGs) and with PID 3
for fio and Filebench (one RG). An fp-remap call that fails is still listed in
`events.csv` (KNOWN_ISSUES SCR-11). The Filebench driver samples every 60 s and
ignores its `--interval` option (KNOWN_ISSUES SCR-M2). The fileserver
personalities end with `psrun $runtime`, which Filebench runs for 60 s only
(KNOWN_ISSUES SCR-1), so `fig17-filebench.sh` runs a copy whose last line is
`psrun 10 4200` (`FILEBENCH_PSRUN`). The `directio` flags of
`fileserver_xfs.f` have no effect, so it issues buffered I/O like
`fileserver_f2fs.f` (KNOWN_ISSUES SCR-2).

## License

GPL-2.0-only (see the SPDX headers). The license text is
[`linux-fdp/LICENSES/preferred/GPL-2.0`](../linux-fdp/LICENSES/preferred/GPL-2.0);
see also [`LICENSE`](../LICENSE).
