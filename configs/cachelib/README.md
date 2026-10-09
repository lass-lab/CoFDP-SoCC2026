<!-- SPDX-License-Identifier: Apache-2.0 -->
# CacheLib (cachebench) configurations

The `cachebench` configurations of the CoFDP CacheLib experiments: one
directory per workload and in it one file per stack. cachebench accepts the
`//` comments in the files and resolves `pop.json` and `sizes.json` relative
to the configuration file, so keep each directory together. These two files
are the upstream CacheLib distributions (`test_configs/hit_ratio/<workload>`,
`test_configs/ssd_perf/kvcache_l2_wc`).

## Build and run

Build `cachebench` with `scripts/build/cachelib.sh`, which runs
`FDP_CACHELIB/contrib/build.sh` (install liburing >= 2.2 first); the binary is
`FDP_CACHELIB/opt/cachelib/bin/cachebench`. The changes of the fork are
described in [`FDP_CACHELIB/COFDP.md`](../../FDP_CACHELIB/COFDP.md).

In the FEMU guest, [`scripts/cachelib/run.sh`](../../scripts/cachelib/run.sh)
runs one workload on one stack:

    sudo scripts/cachelib/run.sh cofdp cdn     # <stack> <workload>
    sudo scripts/common/finish-run.sh          # FEMU prints the counters of the run

The stacks are `cofdp`, `kernelfdp-pc`, `kernelfdp-f2fs` and `userfdp`, the
workloads `cdn`, `kvc`, `gfl`, `gal`, `gff` and `gaf`. The recipe resets the
device, formats and mounts the file system, prepares the directories that the
configuration names and runs

    cachebench --json_test_config configs/cachelib/<directory>/<file> --progress=100

[`scripts/README.md`](../../scripts/README.md) describes these steps and how
the write amplification is computed from the counters. The co-running tenant
recipes (`scripts/tenants/`) use `isol.json`, and the fp-remap recipe for
CacheLib (`scripts/elasticity/fig16a-cachelib.sh`) uses
`kvcache_l2_wc/config.json`.

The configurations contain their paths literally: `/home/femu/f2fs_fdp_mount`
(the mount point of PiDir-FS or F2FS in the FEMU guest), `/home/femu/tenant1`
(`isol.json`) and `/dev/nvme0n1` (UserFDP). For other paths, pass an edited
copy to the recipes as `CACHELIB_CONFIG`.

## Workloads

| Directory | Workload |
|---|---|
| `cdn/` | cdn |
| `kvcache/` | kvc |
| `graph_cache_leader_fbobj/` | GFL (graph cache leader, fbobj) |
| `graph_cache_leader_assocs/` | GAL (graph cache leader, assocs) |
| `graph_cache_follower_fbobj/` | GFF (graph cache follower, fbobj) |
| `graph_cache_follower_assocs/` | GAF (graph cache follower, assocs) |
| `kvcache_l2_wc/` | upstream's `ssd_perf/kvcache_l2_wc` workload (online generator) |
| `kvcache_multiclient/` | kvc |
| `examples/` | UserFDP and single-file examples |

## Files

`navyFileDirectIO` selects the I/O mode of the Navy files: `false` is
buffered I/O through the page cache, `true` or not set is `O_DIRECT`
(`FDP_CACHELIB/COFDP.md`, section 2). The UserFDP configurations leave it
unset: their data path is NVMe passthrough on `/dev/ng0n1`, which the open
mode of `/dev/nvme0n1` does not affect.

Used by the recipes:

| File | Stack | `nvmCachePaths` | `navyFileDirectIO` |
|---|---|---|---|
| `cofdp.json` | CoFDP | Flexible P-Directories `pp0pp2pp4pp6`, `pp1pp3pp5pp7` | `false` |
| `pc.json` (cdn, kvc) | KernelFDP-PC | Initial P-Directories `p4`, `p5` | `false` |
| `kernel.json` | KernelFDP-F2FS (directories made with `mkdir`); KernelFDP-PC for the graph workloads, which have no `pc.json` (same settings) | `p4`, `p5` | `false` |
| `user.json` | UserFDP: NVMe FDP passthrough, io_uring with queue depth 1 | `/dev/nvme0n1` | not set |
| `isol.json` (cdn, GFF) | CacheLib tenant of the co-running tenants | `/home/femu/tenant1/p0`, `p1` (P-Directories bound there by `scripts/tenants/fig15-bind.sh`) | `false` |
| `kvcache_l2_wc/config.json` | CoFDP, both files below the Flexible P-Directory `fp3` (the recipe remaps `fp3` with fp-remap) | `fp3/large/`, `fp3/small/` | `true` |

Not used by the recipes:

| File | Setup | `nvmCachePaths` | `navyFileDirectIO` |
|---|---|---|---|
| `0313.json` (cdn, kvc, GFL) | CoFDP in the layout of `kvcache_l2_wc/config.json`, with smaller caches | `fp3/large/`, `fp3/small/` | `true` |
| `pp.json` (cdn, kvc) | CoFDP with other cache, region and workload sizes | `pp6pp4pp2pp0`, `pp7pp5pp3pp1` | `false` |
| `multi.json` (cdn) | HybridFDP, 4096 MiB per file, 1 GiB regions | `p6`, `p7` | `false` |
| `kvcache_multiclient/hybrid.json` | HybridFDP, 25600 MiB per file, 1 GiB regions | `p4`, `p5` | `false` |
| `user_buffer.json` (cdn, kvc) | Navy on the raw block device without FDP, synchronous I/O | `/dev/nvme0n1` | `false` |
| `examples/userfdp-cuser.json` | UserFDP | `/dev/nvme0n1` | not set |
| `examples/userfdp-consistency.json` | UserFDP with `checkConsistency` | `/dev/nvme0n1` | not set |
| `examples/f2fs-single-dir.json` | Navy on one cache file (no HybridFDP): `mycache` itself or, if `mycache` is a directory, a file in a new subdirectory that cachebench deletes at exit | `mycache` | `false` |

The file-system paths are below `/home/femu/f2fs_fdp_mount` except in
`isol.json`. The kernel accepts the P-Directory prefixes `fp` and `pp`; a
Flexible P-Directory name lists its PIDs in any order, so `pp0pp2pp4pp6` and
`pp6pp4pp2pp0` both map to the PIDs 0, 2, 4 and 6. With the ConfFDP preset of
4 reclaim groups x 2 placement handles (8 PIDs, PID p in RG p / 2), each of the
two Flexible P-Directories of `cofdp.json` and `pp.json` covers all four RGs.

## HybridFDP (`enableHyBridFDP`)

With `"enableHyBridFDP": true` and two directories in `nvmCachePaths`,
cachebench creates `<dir 0>/navy_cache0` and `<dir 1>/navy_cache1`, each
fallocated to `nvmCacheSizeMB`, and Navy uses one file per engine instead of
RAID-0: the first file in sorted path order backs BigHash (the small object
cache, SOC, starting at offset 0), the second one BlockCache (the large object
cache, LOC). Placement comes from the P-Directory of each file; CacheLib sends
no PID. On F2FS the `navy_cache0` file is pinned (`F2FS_IOC_SET_PIN_FILE`).
All configurations list their directories in sorted order (KNOWN_ISSUES
CL-17). With `fp3/large/` and `fp3/small/`, the SOC file is the one in
`large/`.

## Notes

* Capacity: in HybridFDP mode `nvmCacheSizeMB` is the size of each of the two
  files, so `cofdp.json`, `kernel.json` and `pc.json` (6144 MiB per file) have
  twice the NVM cache of `user.json`, which sets the same value for its one
  device (KNOWN_ISSUES CL-9).
* GFL: `user.json` sets `numKeys` to 7,160,557, `cofdp.json` and
  `kernel.json` to 716,055 (KNOWN_ISSUES CL-10).
* Operations (`cofdp`, `kernel`, `pc`, `user`): cdn 1.6M x 48 threads =
  76.8M, kvc 25M x 4 = 100M, graph workloads 5M x 48 = 240M.
* In the `cofdp`/`kernel`/`pc`/`user` configurations, BlockCache regions are
  512 MiB with 30 in-memory region buffers (`navyNumInmemBuffers`);
  `isol.json` uses 1 GiB regions.
* UserFDP: BigHash and BlockCache use the RUH-status placement handles 1 and
  2 (KNOWN_ISSUES CL-M1).
* Output (`FDP_CACHELIB/COFDP.md`, section 5): progress lines report
  thousands of operations (`K ops completed`) followed by NVM read/write, SET
  (allocate) and GET (find) latency lines. At exit the device prints a
  `~~~~FileDevice` line with the bytes written to the SOC and the LOC file
  (HybridFDP; both 0 otherwise). The `NVM bytes written (nand)` and
  `dev write amplification` lines are always 0; the write amplification comes
  from ConfFDP's counters.
* Some files carry keys this CacheLib version ignores (e.g. `useLegacyKeyGen`
  in `kvcache_l2_wc/config.json`).

## License

The configurations are based on CacheLib's cachebench test configurations
(`cachelib/cachebench/test_configs`) and, like CacheLib, are licensed under the
Apache License 2.0 ([`FDP_CACHELIB/LICENSE`](../../FDP_CACHELIB/LICENSE)).
