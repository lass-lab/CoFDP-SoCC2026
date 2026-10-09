# CoFDP changes in this RocksDB fork

This tree is RocksDB 10.0 (upstream `main` at c3b71a67) with the changes that
CoFDP ("CoFDP: User-Kernel Cooperative Storage for Flexible Data Placement
SSDs", SoCC'26) uses to run RocksDB on PiDir-FS and on the kernel FDP
baselines. Compared with upstream it adds

1. the **P-Directory placement shim** (`env/pdir_placement.{h,cc}`), which
   puts WAL files and the SSTs of different LSM levels into different
   P-Directories,
2. the **experiment instrumentation** (`env/cofdp_stats.{h,cc}`): an ops
   timeline with per-interval latencies and a POSIX I/O latency breakdown,
   and
3. a few `db_bench` changes (section 3).

Both modules are owned by `PosixFileSystem`. `FileSystem::pdir_placement()`
and `FileSystem::cofdp_stats()` return `nullptr` for every other file system,
including wrappers such as the ones `db_bench --report_file_operations` or
`--simulate_hdd` installs, and the hooks in `db/` then do nothing. The
placement shim works on Linux only, where `RegisterDbPaths` is implemented.

## 1. P-Directory placement shim

On PiDir-FS a file gets the placement IDs (PIDs) of the P-Directory it is
created in. RocksDB keeps its files in one DB directory, so the shim moves WAL
and SST files into P-Directories placed *inside* the DB directory, typically
bind mounts of P-Directories of the PiDir-FS mount:

    mkdir $DB/p0 $DB/p1 $DB/p2
    mount --bind $PIDIRFS/fp0fp2fp4fp6 $DB/p0   # pdir[0]: WAL
    mount --bind $PIDIRFS/fp0fp2fp4fp6 $DB/p1   # pdir[1]: L0 SSTs
    mount --bind $PIDIRFS/fp1fp3fp5fp7 $DB/p2   # pdir[2]: L1 and deeper

The shim knows nothing about PIDs; they follow from what the `p*` entries
are. [`scripts/rocksdb/lib.sh`](../../scripts/rocksdb/lib.sh) creates the
layouts that the experiment scripts use.

**Discovery.** Each time a column family registers its paths (on every DB
open), `PosixFileSystem::RegisterDbPaths` lists the first of them. Every entry
whose name starts with `p`, of any file type, is a P-Directory. The entries
are sorted by name in byte order (`p10` sorts before `p2`) into
`pdir[0..N-1]` and opened `O_RDONLY | O_CLOEXEC` for directory fsyncs. Each
one is reported on stdout, so the output shows the layout in use:

    <db path>/<name> starts with p

A new registration replaces the set and closes the previous descriptors; all
DBs of a process share one set.

**Placement.** Once N > 0 and a DB has been opened on the file system
(`DBImpl` calls `PDirPlacement::NotifyDbOpened()`), paths are rewritten:

| file                 | stored at                                     |
|----------------------|-----------------------------------------------|
| `<dir>/<name>.log`   | `<dir>/pdir[0]/<name>.log`                    |
| `<dir>/<number>.sst` | `<dir>/pdir[min(L + 1, N - 1)]/<number>.sst`  |
| any other file       | unchanged (MANIFEST, CURRENT, OPTIONS, IDENTITY, LOCK, LOG, ...) |

L is the level the SST is created for, recorded just before the file is
created: 0 for flushes and WAL recovery, the output level for compactions,
and unknown (-1, so `pdir[0]`) for `RepairDB` and for file numbers >= 2^20.
So N = 2 separates the WAL from all SSTs, N = 3 gives WAL | L0 | L1+, N = 4
gives WAL | L0 | L1 | L2+, and so on. The first resolution of each path is
cached for the lifetime of the process.

Rewritten operations: `NewSequentialFile`, `NewRandomAccessFile`,
`NewWritableFile`, `ReopenWritableFile`, `ReuseWritableFile` (both names),
`NewMemoryMappedFileBuffer`, `DeleteFile`, `GetFileSize`, `RenameFile` (both
names) and `LockFile`. All other operations, for example `FileExists`,
`GetChildren`, `LinkFile`, `NumFileLinks`, `GetFileModificationTime`,
`AreFilesSame`, `NewRandomRWFile` and `NewDirectory`, use the path as given.

**Directory fsync.** `DirFsyncOptions::pdir_class` tells
`PosixDirectory::FsyncWithDirOptions` which directory to sync when there are
P-Directories: class 0 (the default) syncs the directory itself, as upstream
does; class k >= 1 syncs `pdir[k - 1]` instead. The first synced WAL write
passes class 1 (WAL), an atomic flush class 2 (L0) and a compaction
`output_level + 2`; non-atomic flushes, `SyncWAL` and all other callers use
class 0. On F2FS, fsyncing a directory triggers a checkpoint.

**Limitations.** The placement state lives in the process: another process
cannot reliably reopen a DB with N > 0 (it looks for every SST in `pdir[0]`),
and `GetChildren` (WAL recovery, `DestroyDB`, obsolete-file purges) does not
look into P-Directories. Files outside the DB directory (`--wal_dir`, WAL
archives, ingestion, checkpoints, backups) are not supported. See section 4.

## 2. Instrumentation

**I/O latency breakdown.** `CoFDPStats::IoTimer` times POSIX calls of
`PosixFileSystem` and its files; each call adds its duration, truncated to
whole microseconds, to its call site. `~DBImpl` prints one line per call site
used so far, sorted by name:

    <name> : <total us>/<calls> = <average us>

The call sites are `Meta` (the open, unlink, stat, rename and lock paths of
`PosixFileSystem`, including path resolution), `PosixWrite` (`write`),
`PosixPositionedWrite` (`pwrite`), `PosixWritableFile::Sync` (`fdatasync`),
`PosixSequentialFile::PositionedRead` and
`PosixRandomAccessFile::PositionedRead` (`pread`), and
`PosixRandomRWFile::Read` / `PosixRandomRWFile::Sync`. The sums cover the
whole life of the file system, not a single DB.

**Ops timeline.** Each `PosixFileSystem` starts a thread when it is created
(the default file system: on first use). The thread prints
`--------------BackgroundStatTimeLapse------------------------` and then,
every 10 seconds,

    <seconds>\t<writes>\t<gets>\t<iterators>\t<delta>\t
      PUT (<n> ops): avg=<x>us p50=<x>us p90=<x>us p95=<x>us p99=<x>us p99.9=<x>us p99.99=<x>us
      GET (<n> ops): ...
      SCAN(<n> ops): ...

The row holds the cumulative numbers of `DB::Write` batches (`Put`, `Delete`,
`Merge`, `Write`, ...), `GetImpl` calls (`Get` and similar, not `MultiGet`)
and `NewIterator` calls, and the increase of their sum since the previous
row. Each latency line covers the operations finished in the last interval,
or reads `  <label>: no ops`. The average is exact; each percentile is the
lower bound of a power-of-two nanosecond bucket (RDB-9), and `SCAN` times
iterator creation only (RDB-8). A `PosixFileSystem` that is destroyed (the
default one only in builds that free static objects, such as ASAN builds)
prints `--------------bye BackgroundStatTimeLapse------------------------ bye`,
all rows again without the delta column, and its I/O breakdown.

Cost: two `clock_gettime` calls and three atomic additions per
`Write`/`Get`/`NewIterator`, and two `clock_gettime` calls and two atomic
additions per timed I/O call. Every build of this fork pays it, so all
configurations that use this fork run the same code.

## 3. db_bench changes

* With `--stats_interval` or `--stats_interval_seconds`, each interval report
  (on stderr) is followed by a `@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@` line on
  stdout.
* The `... finished <n> ops` progress line is not printed.
* `--duration` does not lift the op limit: a run ends after `--duration`
  seconds or after its op count (`--num`, `--reads`, ...), whichever comes
  first (RDB-11). Upstream ignores the op count when `--duration` is set.
* MixGraph never throttles: it does not apply the `--sine_mix_rate` and
  `--benchmark_{read,write}_rate_limit` rate limiters (RDB-10).

## 4. Known issues

These issues are not fixed; [`KNOWN_ISSUES.md`](../../KNOWN_ISSUES.md) lists
them as well.

| ID     | Issue |
|--------|-------|
| RDB-1  | The P-Directory fsync index is not clamped as placement is: a compaction fsyncs `pdir[output_level + 1]`, an atomic flush `pdir[1]`. When the index is N or more (compactions into level N - 1 or deeper, atomic flushes with N = 1), the descriptor is read past the end of the array. The value read depends on the heap: a value <= 0 skips the fsync, an invalid descriptor makes it fail (the error is ignored), and a valid one syncs whatever file it refers to. The P-Directory that received the new SSTs is not synced. |
| RDB-2  | P-Directory fsync errors are ignored and descriptors <= 0 are skipped. Non-atomic flushes and `SyncWAL` fsync the DB directory, not the L0 or WAL P-Directory. |
| RDB-3  | The placement state lives in the process: a process that reopens a DB with N > 0 looks for every SST in `pdir[0]`, and `GetChildren` does not list files inside P-Directories. |
| RDB-4  | P-Directories are ordered by the byte order of their names, not numerically; entries whose names do not start with `p` (e.g. `fp...`) are ignored. |
| RDB-5  | Every entry starting with `p` counts, including regular files, and the most recent registration defines the P-Directories of all DBs in the process. |
| RDB-6  | Any `*.log` or `*.sst` path is rewritten relative to its own directory, also outside the DB directory, while the operations that are not rewritten (e.g. `LinkFile`, `NewRandomRWFile`) use the original path. |
| RDB-7  | `DeleteFile` removes the resolved path instead of the logical one from the resolution cache, so cache entries are never removed. |
| RDB-8  | `SCAN` measures iterator creation only, not `Seek`/`Next`. |
| RDB-9  | Timeline percentiles are power-of-two lower bounds and can be up to 2x below the true value. |
| RDB-10 | MixGraph ignores the sine-rate and benchmark rate limiters (section 3). |
| RDB-11 | `--duration` does not lift the op limit (section 3). |

## 5. Building

    DEBUG_LEVEL=0 make -j$(nproc) db_bench static_lib

This builds the release `db_bench` and `librocksdb.a` in this directory, as
[`scripts/build/rocksdb.sh`](../../scripts/build/rocksdb.sh) `f2fs` does. A
plain `make db_bench` builds with `DEBUG_LEVEL=1` (assertions enabled).

YCSB-cpp-fdp must be compiled with this tree's `include/` and linked with its
`librocksdb.a`, because this fork changes `include/rocksdb/file_system.h`;
[`scripts/build/ycsb.sh`](../../scripts/build/ycsb.sh) `f2fs` does both.
Relink it after rebuilding the library.
