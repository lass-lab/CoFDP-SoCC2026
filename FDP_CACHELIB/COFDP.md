# CoFDP changes in this CacheLib fork

This tree is CacheLib (upstream commit `6c9ca69d`, March 2024, with the
upstream submodule pins) extended for the CacheLib experiments of CoFDP
("CoFDP: User-Kernel Cooperative Storage for Flexible Data Placement SSDs",
SoCC'26). One `cachebench` binary serves all stacks of the paper:

* **CoFDP, KernelFDP-F2FS and KernelFDP-PC** run Navy in the *HybridFDP*
  mode (section 1): the small and the large object cache each get their own
  file in their own directory of the kernel file system, and the directory
  decides the placement.
* **UserFDP** runs upstream's NVMe FDP passthrough mode on the raw device,
  with the changes of section 3.

Build with `./contrib/build.sh` from this directory, as upstream
(`scripts/build/cachelib.sh` does this); the binary is
`opt/cachelib/bin/cachebench`. This fork additionally requires liburing
(>= 2.2), installed before the build, for the io_uring engine and the NVMe
passthrough path. The configurations and how to run them are in
[`configs/cachelib/`](../configs/cachelib/README.md).

## 1. HybridFDP: one file per Navy engine

With `"enableHyBridFDP": true` and two directories in `nvmCachePaths`,
cachebench creates `<dir 0>/navy_cache0` and `<dir 1>/navy_cache1`, each
fallocated to `nvmCacheSizeMB` (so the device is twice `nvmCacheSizeMB`), and
Navy uses one file per engine instead of RAID-0 striping
(`NavyConfig::setPerEngineFiles`, `Device::usesPerEngineFiles`):

| file (sorted path order) | engine | offsets |
|---|---|---|
| file 0 (`EngineFile::kSmallObjectCache`) | BigHash, the small object cache (SOC) | BigHash starts at offset 0 |
| file 1 (`EngineFile::kLargeObjectCache`) | BlockCache, the large object cache (LOC) | regions start at the end of the metadata area, as upstream |

BigHash and BlockCache read, write and flush through the `EngineFile`
overloads of `Device`, which address one file with an engine-relative offset.
CacheLib sends no placement ID in this mode (NVMe FDP passthrough is not set
up even if `deviceEnableFDP` is set): with CoFDP the directories are Flexible
P-Directories of the PiDir-FS mount, so the kernel tags the writes of each
file with the PIDs of its directory.

After it is fallocated, a file whose name contains `navy_cache0` (the SOC
file) is pinned with `F2FS_IOC_SET_PIN_FILE`, so that F2FS updates it in place
and its garbage collection skips it; on other file systems the ioctl fails
and a warning is logged.

The mode assumes exactly two directories listed in sorted order,
`navyBigHashSizePct` 50 and no cache persistence (`cacheDir`); other
combinations are not rejected (KNOWN_ISSUES CL-13, CL-17).

## 2. I/O mode of Navy files: `navyFileDirectIO`

`cache_config.navyFileDirectIO` (`NavyConfig::setDirectIo`) selects how Navy
opens its cache files and devices: `true` (default) opens them with
`O_DIRECT` as upstream does, `false` uses buffered I/O through the page cache.
Unlike upstream there is no silent fallback to buffered I/O when the file
system rejects `O_DIRECT`; the open then fails. The values that the
configurations in `configs/cachelib` set are listed in its README.

## 3. UserFDP: NVMe FDP passthrough

* Write commands carry the placement ID in DSPEC (CDW13[31:16]); the
  directive type DTYPE is not set (KNOWN_ISSUES CL-12). The emulated FDP SSD
  (ConfFDP) only looks at DSPEC.
* Passthrough commands use a timeout of 10,000,000 ms instead of the kernel
  default, and the NVMe character device is opened read-write.
* With an asynchronous I/O engine (io_uring or libaio), each piece of a read
  or write (at most the maximum I/O or write size of the device) is issued as
  consecutive 512 KiB I/Os, because passthrough commands larger than 512 KiB
  fail (the kernel maps the user buffer page by page). Each of them waits for
  the previous one to complete (KNOWN_ISSUES CL-N1), and only the result of
  the last 512 KiB I/O of a piece is checked (KNOWN_ISSUES CL-14).
* The placement handles are upstream's: BigHash and BlockCache get the
  RUH-status entries 1 and 2 (KNOWN_ISSUES CL-M1).

## 4. Navy tuning

* BlockCache regions larger than 256 MiB are allowed (the configurations use
  512 MiB and 1 GiB regions).
* cachebench applies `navyNumInmemBuffers` (upstream parses but ignores it);
  `BlockCacheConfig::setCleanRegions` no longer lowers the number of in-memory
  buffers, whose library default is 100 instead of 2.
* A returned in-memory buffer wakes one waiter instead of all
  (`RegionManager`).
* The I/O timeout that triggers a slow-I/O log is 1000 s instead of 1 s. The
  error logs for failed I/Os and the request scheduler's logs for stalled
  dispatches, stalled I/Os and failed health checks are disabled.

## 5. cachebench

* Workloads: `test_config.testDurationSec` ends the run after that many
  seconds (checked every 1024 operations per thread, KNOWN_ISSUES CL-16). The
  workload generator seeds its generators from `srand()` with a fixed seed
  (`kWorkloadSeed` in `WorkloadGenerator.cpp`) instead of `folly::Random`
  (KNOWN_ISSUES CL-N2).
* Progress (`--progress`): the line reports thousands of operations
  (`K ops completed`, upstream: millions) and is followed by NVM read and
  write latency lines and the latencies of `allocate` (`SET`) and, with
  `--report_api_latency`, of `find` (`GET`). The estimates cover a sliding
  window of about one minute, not only the last interval (KNOWN_ISSUES
  CL-N3).
* Summary: the NVM read and write latencies include the average; the API
  latency blocks of `--report_api_latency` include a `stddev` row that is
  always 0 (KNOWN_ISSUES CL-15). NAND writes are not queried from the devices,
  so the `NVM bytes written (nand)` and `dev write amplification` lines report
  0; the write amplification of the emulated FDP SSD comes from ConfFDP's
  counters (see `scripts/README.md`).
* At exit, every Navy file device prints

      ~~~~~~~~~~~~~~~~~~~~FileDevice
      <bytes written to the SOC file> <bytes written to the LOC file>

  The two counters count the writes of the HybridFDP mode; in the other modes
  both are 0.
