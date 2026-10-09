# YCSB-cpp

Yahoo! Cloud Serving Benchmark([YCSB](https://github.com/brianfrankcooper/YCSB/wiki)) written in C++.
This is a fork of [YCSB-C](https://github.com/basicthinker/YCSB-C) with some additions

 * Tail latency report using [HdrHistogram_c](https://github.com/HdrHistogram/HdrHistogram_c)
 * Modified the workload more similar to the original YCSB
 * Supported databases: LevelDB, RocksDB, LMDB, WiredTiger, SQLite

# Build YCSB-cpp

## Build with Makefile on POSIX

Initialize submodule and use `make` to build.

```
git clone https://github.com/ls4154/YCSB-cpp.git
cd YCSB-cpp
git submodule update --init
make
```

Databases to bind must be specified as build options. You may also need to add additional link flags (e.g., `-lsnappy`).

To bind LevelDB:
```
make BIND_LEVELDB=1
```

To build with additional libraries and include directories:
```
make BIND_LEVELDB=1 EXTRA_CXXFLAGS=-I/example/leveldb/include \
                    EXTRA_LDFLAGS="-L/example/leveldb/build -lsnappy"
```

Or modify config section in `Makefile`.

RocksDB build example:
```
EXTRA_CXXFLAGS ?= -I/example/rocksdb/include
EXTRA_LDFLAGS ?= -L/example/rocksdb -ldl -lz -lsnappy -lzstd -lbz2 -llz4

BIND_ROCKSDB ?= 1
```

## Build with CMake on POSIX

```shell
git submodule update --init
mkdir build
cd build
cmake -DBIND_ROCKSDB=1 -DBIND_WIREDTIGER=1 -DBIND_LMDB=1 -DBIND_LEVELDB=1 -DWITH_SNAPPY=1 -DWITH_LZ4=1 -DWITH_ZSTD=1 ..
make
```

## Build with CMake+vcpkg on Windows

see [BUILD_ON_WINDOWS](BUILD_ON_WINDOWS.md)

## Running

Load data with leveldb:
```
./ycsb -load -db leveldb -P workloads/workloada -P leveldb/leveldb.properties -s
```

Run workload A with leveldb:
```
./ycsb -run -db leveldb -P workloads/workloada -P leveldb/leveldb.properties -s
```

Load and run workload B with rocksdb:
```
./ycsb -load -run -db rocksdb -P workloads/workloadb -P rocksdb/rocksdb.properties -s
```

Pass additional properties:
```
./ycsb -load -db leveldb -P workloads/workloadb -P rocksdb/rocksdb.properties \
    -p threadcount=4 -p recordcount=10000000 -p leveldb.cache_size=134217728 -s
```

# CoFDP

This fork drives the RocksDB experiments of CoFDP ("CoFDP: User-Kernel
Cooperative Storage for Flexible Data Placement SSDs", SoCC '26). It contains
no placement logic: placement comes from the RocksDB fork `ycsb` is linked with
and from the storage stack below it (for CoFDP, the P-Directory placement shim
described in [`FDP_ROCKSDB_F2FS/docs/COFDP.md`](../FDP_ROCKSDB_F2FS/docs/COFDP.md)).
Build one `ycsb` per RocksDB fork, with the headers and the library of that
fork: [`scripts/build/ycsb.sh`](../scripts/build/ycsb.sh) builds `ycsb-f2fs`
and `ycsb-torfs`, and [`scripts/ycsb/run.sh`](../scripts/ycsb/run.sh) runs the
workloads (see [`scripts/README.md`](../scripts/README.md)).

## Changes to YCSB-cpp

* **Zipfian constant.** theta is 0.90 instead of 0.99 for the `zipfian` request
  distribution and, through `SkewedLatestGenerator`, for `latest`
  (`core/zipfian_generator.h`). The scrambled zipfian generator's zeta
  constant for 10^10 + 1 items is precomputed as 90.5 (exact: 90.5699,
  `core/scrambled_zipfian_generator.h`); any other `zipfian_const` computes it
  at startup.
* **Load keys.** The workload property `loadkeydistribution` selects the keys
  of the load phase. `sequential` (the default) is YCSB's load: every insert
  takes the next key of the insert sequence, so `recordcount` distinct records
  are created. With `requestdistribution`, every insert takes a key drawn from
  the request distribution (`requestdistribution`), as a read or update does,
  so keys repeat and the load phase becomes a write-only overwrite stream; the
  write-only workloads `workloads/workload_{zipfian,uniform,latest}` set it.
* **Status lines** (`-s`) also print `stdev=` for each operation type, in ns;
  the other latencies are in us.
* **Latency CSVs.** With HdrHistogram measurements (the default), every status
  report rewrites `<hdrhistogram.csv_dir>/ycsbhist<OP>` (e.g. `ycsbhistREAD`)
  with the latency distribution of that operation type recorded so far, in
  HdrHistogram's percentile CSV format with values in ns.
  `hdrhistogram.csv_dir` defaults to `/home/femu/isol`; an empty value disables
  the files, and a directory that cannot be written is reported once on stderr.
* **Load/run pause.** `ycsb` always sleeps 720 s between the load and the run
  phase, even when only `-load` or `-run` is given; `sleepafterload` is ignored.
* **RocksDB binding.** The env built from `rocksdb.env_uri`/`rocksdb.fs_uri` is
  re-applied after `rocksdb.optionsfile` has been loaded. Upstream silently
  drops it in that case, which would run the TorFS configuration on POSIX.

Known issues ([`KNOWN_ISSUES.md`](../KNOWN_ISSUES.md)): YCSB-3 (fixed 720 s
pause), YCSB-7 (stdev in ns) and YCSB-8 (approximate zeta constant).

## Configuration

The `rocksdb.optionsfile` paths in the properties files below are relative to
`YCSB-cpp-fdp/` (this directory), so run `ycsb` from here, for example:
```
./ycsb -load -db rocksdb -P workloads/workload_zipfian -P rocksdb/rocksdb.properties_f2fs -s
```
From any other directory, pass `-p rocksdb.optionsfile=<absolute path>` after
the `-P` file; `-p rocksdb.dbname=...` overrides the DB path in the same way.

| File | Purpose |
|---|---|
| `rocksdb/rocksdb.properties_f2fs` | RocksDB F2FS fork on F2FS (KernelFDP-F2FS, CoFDP); the DB is the mount root `/home/femu/f2fs_fdp_mount` |
| `rocksdb/rocksdb.properties_torfs` | RocksDB TorFS fork on TorFS (UserFDP), `rocksdb.fs_uri=torfs:xnvme:/dev/nvme0n1?be=thrpool` |
| `rocksdb/rocksdb.properties_tenant0`, `rocksdb/rocksdb.properties_tenant1` | RocksDB F2FS fork with the DB in `/home/femu/tenant0` or `/home/femu/tenant1`, into which the run recipe bind-mounts P-Directories (PiDir-Binding) |
| `rocksdb/rocksdb.properties_pp` | RocksDB F2FS fork with the DB inside the Flexible P-Directory `pp7` |
| `rocksdb/rocksdb_f2fs_options.ini`, `rocksdb/rocksdb_torfs_options.ini` | RocksDB options for the F2FS fork (CoFDP, KernelFDP) and for the TorFS fork (UserFDP) |

The two options files differ in `bytes_per_sync` (1 MiB vs. 0),
`writable_file_max_buffer_size` (0 vs. 1 MiB) and `stream_option=2`, the
TorFS fork's level separation (WAL to PID 1, level-L SSTs to PID L+2). Their
`[Version]` entry `rocksdb_version=7.4.4` has no effect, and RocksDB turns
`wal_dir=/` into the DB directory.

| Workload file | Workload |
|---|---|
| `workloads/workload{a,b,c,d,e,f}` | YCSB A-F: 20,971,520 records, 52,428,800 operations |
| `workloads/workload_nutanix` | Nutanix: 41% reads, 2% scans, 57% inserts, zipfian |
| `workloads/workload_{zipfian,uniform,latest}` | write-only, load phase only (`-load`); `loadkeydistribution=requestdistribution` draws the load keys from the request distribution |
| `workloads/workloade_isol` | YCSB-E tenant of the multi-tenant experiment |
| `workloads/workloadc_isol`, `workloads/workload_nutanix_{isol,multi,2multi}` | smaller variants for multi-tenant runs |
| `workloads/workload_small_nut` | small Nutanix configuration for smoke tests |
