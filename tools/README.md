<!-- SPDX-License-Identifier: GPL-2.0-only -->
# CoFDP tools

Small user-space tools for running CoFDP.

| Tool | What it does |
|---|---|
| [`fp-remap`](#fp-remap) | Maps a directory tree to a set of PIDs, i.e. makes it a Flexible P-Directory (F2FS and XFS). |
| [`pidirfs-mount`](#pidirfs-mount) | Mounts the CoFDP F2FS (PiDir-FS) with `fdp_log_n=N` per-PID logs. |
| [`fdp-reset`](#fdp-reset) | Makes FEMU (ConfFDP) print its write and GC counters, then resets the emulated SSD. |
| [`fdp-ruh-status`](#fdp-ruh-status) | Prints the Reclaim Unit Handle Status, i.e. the PIDs, of an FDP namespace. |

All tools need root (`CAP_SYS_ADMIN`, or access to the NVMe device). They exit with
status 0 on success and 1 on any failure, including usage errors.

## Build

```bash
make -C tools                          # all four tools, in tools/
make -C tools fp-remap pidirfs-mount   # only the tools that do not need xNVMe
sudo make -C tools install             # into /usr/local/sbin (PREFIX=..., DESTDIR=...)
```

`fdp-reset` and `fdp-ruh-status` use [xNVMe](https://xnvme.io) (tested with 0.7.4,
the version [`scripts/build/xnvme.sh`](../scripts/build/xnvme.sh) installs). The
Makefile finds it with `pkg-config xnvme`. Without pkg-config, set the flags
yourself, e.g.
`make -C tools XNVME_CFLAGS=-I$HOME/opt/include XNVME_LIBS="-L$HOME/opt/lib -lxnvme"`.

## fp-remap

```
fp-remap <directory_path> <pid> [pid ...]
```

Maps `<directory_path>` and everything below it to the given PIDs, so it becomes a
Flexible P-Directory. Later writes to files in the tree are spread pseudo-randomly
over these PIDs, and new files inherit them. Data that is already written is not
moved. Each `<pid>` is a decimal number from 0 to 30 (`010` is PID 10; `0x3` is
rejected). Together, the PIDs form a bitmap (bit *n* = PID *n*). The kernel adds
bit 31, which marks a Flexible P-Directory.

```console
$ sudo tools/fp-remap /home/femu/f2fs_fdp_mount/fp1fp3fp5fp7 1 3
PID bitmap: 0x0000000a (bits: 1 3)
Detected: F2FS
Remapped '/home/femu/f2fs_fdp_mount/fp1fp3fp5fp7' -> 0x8000000a
```

The file system type comes from `fstatfs()`:

| File system | ioctl | Number | Argument |
|---|---|---|---|
| F2FS with PiDir-FS (`mkfs.f2fs -O lost_found`) | `F2FS_IOC_FP_REMAP` = `_IOW(0xf5, 16, __u32)` ([f2fs.h](../linux-fdp/include/uapi/linux/f2fs.h)) | `0x4004f510` | `__u32` PID bitmap |
| XFS with `mkfs.xfs -d fdp_pids=N` | `XFS_IOC_COFDP_REMAP` = `_IOW('X', 62, struct xfs_cofdp_remap_arg)` ([xfs_fs.h](../linux-fdp/fs/xfs/libxfs/xfs_fs.h)) | `0x4008583e` | `{ __u32 new_pid_bitmap; __u32 flags = 0; }` |

XFS rejects PIDs at or above its `fdp_pids`. F2FS uses PID *p* as *p* `%` `fdp_log_n`.
F2FS can also create a Flexible P-Directory without this tool: `mkdir fp1fp3` maps
the new directory to PIDs 1 and 3. XFS does not parse directory names, so on XFS
use `mkdir` followed by `fp-remap`.

## pidirfs-mount

```
pidirfs-mount [--discard-unit=segment|block | --nodiscard]
              [--dev DEVICE] [--dir DIRECTORY] <nr_pid_logs>
```

Mounts `DEVICE` on `DIRECTORY` as `f2fs`, with no mount flags and the options below.
The defaults are `/dev/nvme0n1` and `/home/femu/f2fs_fdp_mount`, the device and the
mount point that the experiment scripts use in the FEMU guest.

| Invocation | Mount options |
|---|---|
| `pidirfs-mount N` | `fdp_log_n=N,discard,nobarrier,discard_unit=segment` |
| `pidirfs-mount --discard-unit=block N` | `fdp_log_n=N,discard,nobarrier,discard_unit=block` |
| `pidirfs-mount --nodiscard N` | `fdp_log_n=N,nodiscard,nobarrier` |

For example, `sudo tools/pidirfs-mount 8` is the same as

```bash
sudo mount -t f2fs -o fdp_log_n=8,discard,nobarrier,discard_unit=segment /dev/nvme0n1 /home/femu/f2fs_fdp_mount
```

`fdp_log_n` is the number of per-PID logs of each log type (PISA). The log index is
also the PID that F2FS writes to the device, so set `N` to the number of PIDs of the
device: 8 for the FEMU preset in [`configs/femu/`](../configs/femu) (4 RGs x 2
placement handles). `N` is a decimal number from 1 to 21; other values are rejected.
On success, the tool prints
`Successfully mounted DEVICE on DIRECTORY with options: ...`.

## fdp-reset

```
fdp-reset <device>          e.g. sudo tools/fdp-reset /dev/nvme0n1
```

Sends ConfFDP's FEMU reset command to the namespace of `<device>`. This is an I/O
Management Send (opcode 1Dh) with management operation 2, a FEMU extension
(`NVME_IOMS_MO_FEMU_RESET` in [nvme.h](../ConfFDP/hw/femu/nvme.h)). It goes through
xNVMe (Linux NVMe passthrough, `thrpool` interface) and carries:

* CDW10 `0x00010002` (MO 2, MOS 1)
* a 528-byte data buffer, sent zero-filled, which FEMU does not read

In its FDP mode (`femu_mode=6`, [`run-fdp.sh`](../ConfFDP/femu-scripts/run-fdp.sh))
and multi-stream mode (`femu_mode=7`), FEMU then does the following:

1. It prints the counters accumulated since the previous reset to its output
   (`./log` of `run-fdp.sh`). Both modes print the pages copied by garbage
   collection (`...copied`). FDP mode also prints the 4 KiB pages written per PID
   (`RG r STREAM h : written`). The write amplification factor is 1 + copied /
   (sum of all written), see [ConfFDP/README.md](../ConfFDP/README.md).
2. It resets its FTL to an empty SSD and clears the counters.

The data in FEMU's memory backend is not erased, but the FTL no longer counts it as
valid. Run `fdp-reset` before formatting the device for a run, so that the FTL and
the file system start out empty together. Run it again after the measured workload
to get the workload's counters. On success, the tool prints
`DEVICE: FEMU reset command completed`. On devices other than ConfFDP, the command
fails (exit status 1) or has no effect.

## fdp-ruh-status

```
fdp-ruh-status <device>     e.g. sudo tools/fdp-ruh-status /dev/nvme0n1
```

Reads the Reclaim Unit Handle Status: an I/O Management Receive (opcode 12h) with
management operation 1, MOS 1 and an 8208-byte buffer (NUMD 2051). The buffer holds
the 16-byte header and up to 256 descriptors. The tool prints the number of
descriptors, then the PID, RUHID, EARUTR and RUAMW of each one, as unsigned decimal
numbers and in device order. If the device reports more than 256 descriptors, it
prints the first 256 and a warning on stderr. For ConfFDP with 4 RGs x 2 placement
handles, the output is:

```console
$ sudo tools/fdp-ruh-status /dev/nvme0n1
nruhsd 8
pid ruhid earutr ruamw
0 0 0 0
2 2 0 0
4 4 0 0
6 6 0 0
1 1 0 0
3 3 0 0
5 5 0 0
7 7 0 0
```

`mkfs.f2fs -O lost_found` reads the same data to create one Initial P-Directory
(`p0` ... `p7`) per PID.

## License

GPL-2.0-only (see the SPDX headers). The license text is
[`linux-fdp/LICENSES/preferred/GPL-2.0`](../linux-fdp/LICENSES/preferred/GPL-2.0);
see also [`LICENSE`](../LICENSE).
