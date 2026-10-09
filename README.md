<div align="center">

# CoFDP

### User-Kernel Cooperative Storage for Flexible Data Placement SSDs

*ACM Symposium on Cloud Computing (SoCC '26), November 18–20, 2026, Singapore*

[Overview](#overview) ·
[Repository layout](#repository-layout) ·
[Getting started](#getting-started) ·
[Reproducing the paper](#reproducing-the-paper) ·
[Citation](#citation)

</div>

---

## Overview

NVMe **Flexible Data Placement (FDP)** SSDs let the host steer every write to a
*placement identifier* (PID). The device groups NAND dies into *reclaim groups*
(RGs) and writes each PID into its own *reclaim units*, so data that is placed
well is erased together and never copied by garbage collection. Today the host
can use this in two ways. **UserFDP** rewrites the application around a
user-level storage engine (TorFS, CacheLib's FDP mode) and gives up the page
cache. **KernelFDP** lets the kernel choose PIDs by itself, without application
knowledge and without any notion of RGs.

**CoFDP separates *who decides* placement from *where it is enforced*.** The
operator expresses placement with ordinary directories, called
**P-Directories**. The kernel file system (**PiDir-FS**, implemented for F2FS
and XFS) tags every write of a file with the PID of its P-Directory on the
regular buffered I/O path, so applications keep the page cache, request merging
and I/O scheduling.

```
            user space                                    kernel space
 ┌──────────────────────────────┐   ┌───────────────────────────────────────────────┐
 │ RocksDB   CacheLib   Solana  │   │ VFS ─ page cache                              │
 │    │         │         │     │   │   │                                           │
 │       POSIX file I/O         │   │ PiDir-FS (F2FS / XFS)                         │
 │    │                         │   │   PiDirManager: inode → PID bits (PinodeBlock)│
 │ PiDir-Binding                │   │   PISA: per-PID allocation (adjacent LBAs)    │
 │  ls / mkdir fp1fp3 /         │──▶│ block layer: never merges different PIDs      │
 │  mount --bind / fp-remap     │   │ NVMe driver: PID → DSPEC                      │
 └──────────────────────────────┘   └───────────────────────┬───────────────────────┘
                                                            ▼
                                  FDP SSD (or the ConfFDP emulator): PID → RU → RG
```

**PiDir-Binding** is the whole user-facing interface:

| Step | Command | Effect |
|---|---|---|
| discover | `ls /mnt` | `p0 … p7`: one **Initial P-Directory** per device PID |
| compose | `mkdir /mnt/fp0fp2fp4fp6` | a **Flexible P-Directory**: each allocation picks one of PIDs 0, 2, 4, 6 |
| bind | `mount --bind /mnt/fp0fp2fp4fp6 /db/wal` | an application directory now writes to those PIDs |
| adapt | `fp-remap /mnt/fp0fp2fp4fp6 1 3` | change the PIDs of a directory tree at run time |

### Paper concepts in the code

| Concept (paper) | What it is | Where |
|---|---|---|
| **Initial P-Directory** | one directory per device PID | F2FS: created by `mkfs.f2fs -O lost_found` as `p<PID>` ([`f2fs-tools-fdp/mkfs/f2fs_format.c`](f2fs-tools-fdp/mkfs/f2fs_format.c); PID discovery and PID-tagged writes in [`f2fs_format_pidir.c`](f2fs-tools-fdp/mkfs/f2fs_format_pidir.c)); XFS: `p0` … `p<N-1>`, created at mount ([`linux-fdp/fs/xfs/xfs_cofdp.c`](linux-fdp/fs/xfs/xfs_cofdp.c)) |
| **Flexible P-Directory** | a directory mapped to several PIDs; each allocation picks one of them pseudo-randomly. F2FS parses `mkdir fpXfpY…` (the prefix `pp` is also accepted); on XFS, `fp-remap` sets the PIDs | [`linux-fdp/fs/f2fs/namei.c`](linux-fdp/fs/f2fs/namei.c) |
| **PID inheritance** | new files and subdirectories inherit the parent's PID bits | `f2fs_new_inode()`, `xfs_create()` |
| **PiDirManager / Inode-to-PID table** | in-memory PID bits per inode (bit 31 set = PID bitmap of a Flexible P-Directory) | [`linux-fdp/include/linux/cofdp.h`](linux-fdp/include/linux/cofdp.h), `f2fs_pidir_get/set()` |
| **PinodeBlock** | PID bits persisted in the on-disk inode (F2FS `i_generation`, XFS `di_pad2`) | `fs/f2fs/inode.c`, `fs/xfs/libxfs/xfs_inode_buf.c` |
| **PISA** | PID-aware space allocation: one F2FS log per PID and temperature | `f2fs_pisa_log()` in `fs/f2fs/segment.c` |
| **PID-aware I/O path** | bios and requests carry `fdp_pid`; requests of different PIDs are never merged; the NVMe driver sends the PID in DSPEC | `linux-fdp/block/`, `fs/iomap/`, `drivers/nvme/host/core.c` |
| **fp-remap** | change the PIDs of a Flexible P-Directory and its subtree | [`tools/fp-remap.c`](tools/fp-remap.c), `F2FS_IOC_FP_REMAP`, `XFS_IOC_COFDP_REMAP` |
| **ConfFDP** | FEMU-based FDP SSD with configurable RGs, placement handles and NAND timing | [`ConfFDP/hw/femu/fdp/`](ConfFDP/hw/femu/fdp) |

## Repository layout

The first commit imports the upstream code of Linux, f2fs-tools, xfsprogs, FEMU,
RocksDB, TorFS, CacheLib and YCSB-cpp **unmodified**, and each later commit adds
one component's CoFDP changes. `git show <commit>`, or
`git diff <first-commit> -- <dir>`, therefore shows exactly what CoFDP changes.
`filebench-fdp/` holds modified Filebench workload files, and
`blockchain-fdp/` holds patches against Solana and Aptos.

| Directory | Contents | Upstream base |
|---|---|---|
| [`linux-fdp/`](linux-fdp) | Linux with PiDir-FS for **F2FS** (`fs/f2fs`) and **XFS** (`fs/xfs`), and the PID-aware block layer, iomap and NVMe driver | Linux 6.7.9 |
| [`f2fs-tools-fdp/`](f2fs-tools-fdp) | `mkfs.f2fs -O lost_found`: reads the device's PIDs with xNVMe and creates the Initial P-Directories | f2fs-tools 1.15.0 |
| [`xfsprogs-fdp/`](xfsprogs-fdp) | `mkfs.xfs -d fdp_pids=N`: one allocation group per PID | xfsprogs 6.18.0 |
| [`ConfFDP/`](ConfFDP) | the FDP SSD emulator (FEMU `femu_mode=6`) | FEMU (QEMU 9.0.1) |
| [`tools/`](tools) | `fp-remap`, `pidirfs-mount`, `fdp-reset`, `fdp-ruh-status` | — |
| [`FDP_ROCKSDB_F2FS/`](FDP_ROCKSDB_F2FS) | RocksDB for the CoFDP and KernelFDP stacks | RocksDB 10.0 |
| [`FDP_ROCKSDB_TORFS/`](FDP_ROCKSDB_TORFS) | RocksDB with TorFS, the UserFDP baseline | RocksDB 9.0.1, TorFS |
| [`FDP_CACHELIB/`](FDP_CACHELIB) | CacheLib for CoFDP, KernelFDP and UserFDP (FDP passthrough) | CacheLib `6c9ca69d` |
| [`YCSB-cpp-fdp/`](YCSB-cpp-fdp) | YCSB driver for RocksDB | YCSB-cpp |
| [`filebench-fdp/`](filebench-fdp) | Filebench personalities (video server, OLTP, file server) | Filebench |
| [`blockchain-fdp/`](blockchain-fdp) | Solana and Aptos benchmark patches | Solana 1.18, Aptos |
| [`scripts/`](scripts) | build scripts and one recipe per experiment | — |
| [`configs/`](configs) | CacheLib configurations, the ConfFDP preset | — |

## Getting started

CoFDP runs in a virtual machine whose NVMe SSD is the ConfFDP emulator. The
host builds and boots ConfFDP; everything else is built and run in the guest.
[`scripts/README.md`](scripts/README.md) has the complete instructions.

### 1. Requirements

* **Host**: x86-64 Linux with KVM and `liburing`. ConfFDP keeps the whole
  emulated SSD in pinned host memory, so the host needs more RAM than the
  emulated capacity plus the guest: 64 GiB + 16 GiB with the default preset.
* **Guest image**: the Ubuntu image of the [FEMU project](https://github.com/MoatLab/FEMU),
  stored as `~/images/fdp.qcow2` (override with `IMGDIR=` or `OSIMGF=`).
* **Guest software**: kernel build dependencies and
  [xNVMe](https://xnvme.io/getting_started/index.html) 0.7.4
  (`scripts/build/deps.sh`, `scripts/build/xnvme.sh`).

### 2. Build and boot ConfFDP (host)

```bash
FEMU_INSTALL_DEPS=1 scripts/build/femu.sh   # builds ConfFDP/build-femu/qemu-system-x86_64
cd ConfFDP/build-femu && ./run-fdp.sh       # boots the guest; ssh -p 8085 femu@localhost
```

`run-fdp.sh` emulates a 64 GiB SSD with 8 channels × 8 dies, **4 RGs × 2
placement handles = 8 PIDs**, 40/200 µs page read/program and 2 ms erase, and
gives the guest 8 vCPUs and 16 GiB. Every value can be overridden from the
environment (see [`configs/femu/`](configs/femu)). The emulator's output,
including its measurement dump, goes to `ConfFDP/build-femu/log`.

### 3. Build CoFDP (guest)

```bash
scripts/build/deps.sh && scripts/build/xnvme.sh
scripts/build/kernel.sh && sudo reboot   # installs the image and ALL modules (struct bio changed)
scripts/build/f2fs-tools.sh              # mkfs.f2fs -O lost_found
scripts/build/xfsprogs.sh                # mkfs.xfs -d fdp_pids=N
scripts/build/tools.sh                   # fp-remap, pidirfs-mount, fdp-reset, fdp-ruh-status
sudo tools/fdp-ruh-status /dev/nvme0n1   # lists the 8 PIDs of the emulated SSD
```

`f2fs-tools-fdp` needs xNVMe; its `configure` stops if xNVMe is missing unless
`--without-xnvme` is given (then `-O lost_found` is unavailable).

### 4. A first PiDir-FS session (F2FS)

```bash
sudo tools/fdp-reset /dev/nvme0n1           # empty emulated SSD, counters cleared

sudo f2fs-tools-fdp/mkfs/mkfs.f2fs -f -O lost_found /dev/nvme0n1
#   Info: PiDir-FS: 8 Initial P-Directories: p0 p1 p2 p3 p4 p5 p6 p7

sudo mkdir -p /home/femu/f2fs_fdp_mount
sudo tools/pidirfs-mount 8                  # -o fdp_log_n=8 on /home/femu/f2fs_fdp_mount
M=/home/femu/f2fs_fdp_mount
ls $M                                       # lost+found  p0 … p7

sudo mkdir $M/fp0fp2fp4fp6 $M/fp1fp3fp5fp7  # two Flexible P-Directories, each over all 4 RGs
sudo mkdir -p /data/db/wal /data/db/sst
sudo mount --bind $M/fp0fp2fp4fp6 /data/db/wal
sudo mount --bind $M/fp1fp3fp5fp7 /data/db/sst
# ... run the application on /data/db ...

sudo tools/fp-remap $M/fp1fp3fp5fp7 1 3     # move the SST directory to PIDs 1 and 3 (RGs 0-1)
```

`-O lost_found` makes `mkfs.f2fs` create the PiDir-FS layout; it fails if the
device reports no FDP placement identifiers. `fdp_log_n` is the number of
per-PID logs and equals the number of device PIDs. With 2 handles per RG, PID
`p` belongs to RG `p / 2`. Format the device before every mount (see
[KNOWN_ISSUES.md](KNOWN_ISSUES.md)).

### 5. PiDir-FS on XFS

```bash
sudo scripts/common/umount-all.sh            # the file system and bind mounts of section 4
sudo tools/fdp-reset /dev/nvme0n1
sudo xfsprogs-fdp/mkfs/mkfs.xfs -f -d fdp_pids=8 \
     -m crc=1,reflink=0,rmapbt=0,metadir=0 -i nrext64=0,exchange=0 -n parent=0 /dev/nvme0n1
sudo mount -t xfs -o discard /dev/nvme0n1 /home/femu/f2fs_fdp_mount   # creates p0 … p7
sudo mkdir /home/femu/f2fs_fdp_mount/fp3
sudo tools/fp-remap /home/femu/f2fs_fdp_mount/fp3 1 3 5 7
```

`metadir`, `exchange` and `parent` are newer than the 6.7.9 kernel and must be
off; `reflink`, `rmapbt` and `nrext64` are off as in the recipes. XFS does not
parse `fp…` names: a Flexible P-Directory on XFS is an ordinary directory whose
PIDs are set with `fp-remap`.

### 6. Measuring write amplification

`tools/fdp-reset` sends ConfFDP's reset command: ConfFDP prints its counters
since the previous reset and starts again with an empty SSD. After a workload:

```bash
sudo scripts/common/finish-run.sh                      # guest: FEMU prints the counters of the run
scripts/common/femu-waf.py ConfFDP/build-femu/log      # host: WAF of every run in the log
```

The dump has one `RG <r> STREAM <s> : written : <pages>` line per PID and a
`copied` counter for pages moved by garbage collection, so
`WAF = 1 + copied / Σ written`.

## Reproducing the paper

Every experiment has a recipe under [`scripts/`](scripts); most take the stack
as their first argument (`cofdp`, `kernelfdp-f2fs`, `kernelfdp-pc`, `userfdp`).

| Paper | Experiment | Recipe |
|---|---|---|
| Table 3 | ConfFDP preset | [`configs/femu/`](configs/femu) |
| Table 2, Figs. 12, 13 | RocksDB fillrandom and mixgraph | [`scripts/rocksdb/run-mixgraph.sh`](scripts/rocksdb/run-mixgraph.sh) |
| Table 2, Figs. 12, 13 | YCSB A, B, D, E, F, zipfian, uniform, latest, nutanix | [`scripts/ycsb/run.sh`](scripts/ycsb/run.sh) |
| Table 2, Figs. 12, 13 | CacheLib cdn, kvc, GFL, GAL, GFF, GAF | [`scripts/cachelib/run.sh`](scripts/cachelib/run.sh) |
| Table 2, Figs. 12, 13 | Filebench video, OLTP | [`scripts/filebench/run.sh`](scripts/filebench/run.sh) |
| Table 2, Figs. 12, 13 | Solana, Aptos | [`scripts/blockchain/`](scripts/blockchain) |
| Fig. 13 | write amplification | [`scripts/common/femu-waf.py`](scripts/common/femu-waf.py) |
| Figs. 6, 7 | fio tenants on the raw FDP namespace | [`scripts/fio/`](scripts/fio) |
| Fig. 14 | fio tenants on PiDir-FS (F2FS, XFS) | [`scripts/tenants/fig14.sh`](scripts/tenants/fig14.sh) |
| Fig. 15 | co-running application tenants | [`scripts/tenants/`](scripts/tenants) |
| Fig. 16 | CacheLib and fio with fp-remap | [`scripts/elasticity/`](scripts/elasticity) |
| Fig. 17 | Filebench file server with fp-remap (F2FS, XFS) | [`scripts/elasticity/fig17-filebench.sh`](scripts/elasticity/fig17-filebench.sh) |

## Known issues

Known defects and limitations of the code are listed in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md), each with its trigger, whether the recipes
reach it, and its status. Code comments refer to them by ID, for example
`KNOWN_ISSUES (XFS-4)`.

## Citation

```bibtex
@inproceedings{byeon2026cofdp,
  title     = {{CoFDP}: User-Kernel Cooperative Storage for Flexible Data Placement {SSDs}},
  author    = {Byeon, Sungjin and Park, Jaewan and Hwang, Soon and Lee, Sangyun and
               Min, Kyungwook and Moon, Hajin and Noh, Taewan and Hwang, Joo-Young and
               Han, Jun Young and Kim, Youngjae},
  booktitle = {Proceedings of the ACM Symposium on Cloud Computing (SoCC '26)},
  year      = {2026},
  address   = {Singapore}
}
```

## License

Every component keeps the license of its upstream project: the Linux kernel,
f2fs-tools, xfsprogs and FEMU/QEMU are GPL-2.0; RocksDB is dual GPL-2.0 /
Apache-2.0; CacheLib and YCSB-cpp are Apache-2.0; the Filebench personalities
are CDDL-1.0. The Solana patches are Apache-2.0, and the Aptos patches follow
the license of aptos-core (see [`blockchain-fdp/`](blockchain-fdp)). CoFDP's
changes to a component are released under that component's license; `tools/`
and `scripts/` are GPL-2.0-only. See [LICENSE](LICENSE) for the details.
