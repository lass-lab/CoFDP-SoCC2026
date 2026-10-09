# TorFS: RocksDB Storage Backend for FDP SSDs

TorFS is a plugin that enables RocksDB to access Flexible Data Placement (FDP) SSDs

# Getting started

## Build

Download RocksDB and TorFS:

```shell
git clone git@github.com:facebook/rocksdb.git
cd rocksdb
git clone git@github.com:SamsungDS/TorFS.git plugin/torfs
```

Install and enable TorFS in micro-bench tool of RocksDB

```shell
DEBUG_LEVEL=0 ROCKSDB_PLUGINS=torfs make -j8 db_bench
```



## Testing with db_bench

TorFS supports xNVMe back-end that provides unified interfaces for multiple IO paths.

|           | io_uring_cmd (character device) | io_uring (block device) | libaio (block device) | SPDK |
| :-------: | :-----------------------------: | :---------------------: | :-------------------: | :--: |
| **xNVMe** |                √                |            O            |           O           |  √   |
|  **raw**  |                X                |            X            |           X           |  X   |

- √: Support IO path and data directive			
- O: Support IO path only, not data directive
- X: Not support IO path and data directive

```bash
./db_bench --benchmarks=<IO pattern> --fs_uri=torfs:<backend>:<dev>?be=<IO path>
e.g.
./db_bench --benchmarks=fillseq --fs_uri=torfs:xnvme:/dev/ng0n1?be=io_uring_cmd
```

# This fork: the UserFDP baseline of CoFDP

This copy of TorFS lives in RocksDB 9.0.1 (`FDP_ROCKSDB_TORFS`) together with
two small RocksDB hooks, and is the UserFDP baseline of the CoFDP evaluation.
Build `db_bench` from the root of that tree:

```shell
DEBUG_LEVEL=0 ROCKSDB_PLUGINS=torfs make -j$(nproc) db_bench
```

The plugin needs xNVMe, liburing, libaio, libnuma, libuuid and libcrypto.
[`scripts/build/rocksdb.sh`](../../../scripts/build/rocksdb.sh) `torfs` also
builds `librocksdb.a` (`static_lib`), which
[`scripts/build/ycsb.sh`](../../../scripts/build/ycsb.sh) `torfs` links into
the YCSB driver `ycsb-torfs`.

## Placement policy (`stream_option`)

Every TorFS read and write command carries DTYPE 2 and, for writes, the
placement ID (PID) of the file as DSPEC (reads use 0). The PID of a file is
chosen by `Options::stream_option` (`db_bench --stream_option`, OPTIONS file
key `stream_option`); every DB passes it to the file system with
`FileSystem::SetStreamOption()` when it is opened. RocksDB tells TorFS the
level of each new SST file with `FSWritableFile::SetSstLevel()` before it sets
the write-life-time hint.

| `stream_option` | WAL | SST file at level L |
| :-- | :-- | :-- |
| 0 | PID 0 | PID 0 |
| 1: write-life-time hint | PID 1 | PID 2 (L0 and the base level), 3 (next level), 4 (deeper levels) |
| 2: per-level PIDs | PID 1 | PID L + 2 |
| 3: reserved, no policy | PID 0 | PID 0 |
| 4: per-level reclaim groups (experimental) | RG byte 1 | RG byte min(L + 2, largest RG) |

MANIFEST files always use PID 7, and the info LOG, OPTIONS, CURRENT and
IDENTITY files PID 0. Blob files are placed like the WAL under policies 2 and
4. Policy 4 puts the reclaim group (RG) into the high byte of the PID and
leaves the placement handle in the low byte; the FEMU FDP device of this
artifact numbers PIDs flat instead (RG = PID / placement handles per RG).

The UserFDP baseline uses `stream_option=2`
([`scripts/rocksdb/run-mixgraph.sh`](../../../scripts/rocksdb/run-mixgraph.sh)
`userfdp`). For example, fillrandom (after resetting the FEMU counters with
[`tools/fdp-reset`](../../../tools/README.md#fdp-reset)):

```shell
sudo ./db_bench --benchmarks="fillrandom,stats" --histogram -subcompactions=8 \
    -max_background_flushes=4 -max_background_compactions=4 \
    -value_size=1024 -num=25165824 -stream_option=2 \
    --fs_uri=torfs:xnvme:/dev/nvme0n1?be=thrpool
```

## Other differences from upstream TorFS

- Each flushed file buffer is padded to a multiple of 16 KiB (upstream: the LBA
  size). `MAX_PID` is 8 and MANIFEST files use PID 7 (upstream: 5 and 4).
- Metadata persistence is disabled (`FDP_META_ENABLED 0`): TorFS state lives in
  memory only, so every process starts with an empty TorFS, and a workload
  must load and run in the same process (TORFS-M1).
- No placeholder files are created in the host file system, so RocksDB
  directory scans (e.g. `DestroyDB()`) do not see TorFS files; run one
  `fresh_db` benchmark per `db_bench` process (TORFS-5).
- RocksDB's info LOG and the TorFS log (`/mnt/rocksdb_torfs.log`) are TorFS
  files on the FDP device. TorFS still creates an empty host file
  `/mnt/rocksdb_torfs.log` at startup, so `/mnt` must be writable (run
  `db_bench` as root, e.g. with `sudo`).
- At exit TorFS prints the share of read bytes served from unflushed write
  buffers (`CACHE HIT <hit> / <read> = <hit * 10000 / read>`, with no newline)
  and the latency of its I/O steps (`<step> : <total us>/<count> = <mean us>`).
- With `Options::is_db_bench` false (clients other than `db_bench`, such as the
  YCSB driver), the DB prints its `rocksdb.stats` property when it is destroyed.

Known issues (also in [`KNOWN_ISSUES.md`](../../../KNOWN_ISSUES.md)): PIDs of
policy 2 are not bounded by the device (L5 shares PID 7 with the MANIFEST, and
L6 uses PID 8, which a device with 8 PIDs does not have; TORFS-2); the reclaim
unit handle status is parsed with a wrong layout (TORFS-3), which only policy 4
uses, and policy 4 sends an uninitialized placement handle (TORFS-6); every
freed range is deallocated twice and failed deallocations are not reported
(TORFS-7); the timestamps of TorFS-hosted logs are not meaningful (TORFS-8);
TorFS errors are logged to the most recently created logger, possibly the one
that raised them (TORFS-10).
