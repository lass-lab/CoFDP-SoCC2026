# filebench-fdp

Filebench workload personalities (WML `.f` files) used in the CoFDP evaluation.
This directory contains only workload descriptions, not Filebench itself. Run
them with an unmodified Filebench that reports version 1.5-alpha3, built from
upstream master (see [Filebench build](#filebench-build)).

## Personalities

| File | Paper | Based on (Filebench `workloads/`) | Directories used, under `/home/femu/f2fs_fdp_mount/` |
|---|---|---|---|
| `video_churn.f` | `video` (videoserver): Table 2, Figs. 12 and 13 | `videoserver.f` | `p4` active videos, `p5` passive videos |
| `oltp.f` | `oltp`: Table 2, Figs. 12 and 13 | `oltp.f` | `p4` log file, `p5` data files |
| `fileserver_f2fs.f` | Fig. 17(a), F2FS | `fileserver.f` | `fp3` |
| `fileserver_xfs.f` | Fig. 17(b), XFS | `fileserver.f` | `fp3` |

- For CoFDP, `p4` and `p5` are the Initial P-Directories that
  `mkfs.f2fs -O lost_found` creates (PIDs 4 and 5). For the KernelFDP-F2FS
  baseline they are ordinary directories created with `mkdir`.
- `fp3` is the Flexible P-Directory whose PIDs the Fig. 17 driver changes at run
  time with fp-remap. On F2FS, `mkdir fp3` creates it with PID 3. XFS does not
  parse P-Directory names, so on XFS run `fp-remap fp3 3` after `mkdir fp3`.
- The paths are hard-coded, and Filebench has no environment-variable
  substitution. To use another mount point, edit `$piddir4`/`$piddir5` or `$dir`.

The per-figure run recipes (device reset, mkfs, mount, P-Directory setup and the
Fig. 17 driver) are in the top-level [`scripts/`](../scripts) directory.

## Running

- Filebench prints a cumulative `IO Summary` every 10 s.
- `video_churn.f` and `oltp.f` set `mode quit alldone`: each thread stops at its
  `finishoncount` limit, and the run ends when all threads have stopped. A
  `finishoncount` without `target` counts every flowop its process completes
  (`delay`, `bwlimit` and `finishoncount` itself included), so the limits are
  not loop or I/O counts.
- `video_churn.f` ends by itself.
- `oltp.f` does not finish by itself: its shadow, dbwr and lgwr processes are
  coupled through semaphores (`sempost`/`semblock`), so once one group stops at
  its limit, the others stall. Stop it with SIGINT (Ctrl-C) after the desired
  run time and use the last `IO Summary` printed before the stop. Filebench
  then prints `Aborting...` and exits with status 1, without a final
  `IO Summary`. It leaves behind its 177 MB shared-memory file
  `/tmp/filebench-shm-*` and its System V semaphore set (the set with 128
  semaphores in `ipcs -s`); once Filebench has exited, remove both with `rm`
  and `ipcrm -s <semid>`.
- The fileserver personalities end with `psrun $runtime`, which runs for 60 s;
  see [Known issues](#known-issues).

## Filebench build

```sh
git clone https://github.com/filebench/filebench.git
cd filebench
git checkout 22620e6      # upstream master; reports "Filebench Version 1.5-alpha3"
libtoolize && aclocal && autoheader && automake --add-missing && autoconf
./configure && make && sudo make install
sudo sysctl -w kernel.randomize_va_space=0   # ASLR off until the next reboot
```

- Use master, not the `1.5-alpha3` release tag or tarball (July 2016). In that
  version, `finishoncount` ends the whole run as soon as any thread reaches its
  limit, so `video_churn.f` would stop early; master ends only that thread.
- `make install` is required because the fileserver personalities draw file
  sizes from the `cvar-gamma` library, which Filebench loads from its install
  directory.
- ASLR must be off system-wide (last line above): Filebench maps its shared
  memory at a fixed address in every worker process, and its own per-process
  switch has no effect.

## Known issues

**`psrun $runtime` runs for 60 s** (KNOWN_ISSUES SCR-1; last line of
`fileserver_f2fs.f` and `fileserver_xfs.f`).
- Filebench accepts only literal integers after `psrun`
  (`psrun [period [runtime]]`), so `psrun $runtime` is parsed as a bare `psrun`.
  That means statistics every 10 s and, because these files do not set
  `mode quit alldone`, Filebench's default 60 s run. `$runtime=1200` is ignored.
- To run longer, replace the last line with literal values, e.g.
  `psrun 10 4200`, as the Fig. 17 recipe does. Keep the period positive. A
  negative period resets the statistics every period, but the Fig. 17 driver
  computes throughput from differences between the cumulative `IO Summary`
  lines.

**`directio=1` has no effect** (KNOWN_ISSUES SCR-2; `fileserver_xfs.f`, flowops
`wrtfile1`, `appendfilerand1` and `readfile1`).
- Filebench sets `O_DIRECT` only when it opens a file, using the attributes of
  the opening flowop. Here that is `createfile1`, which has no `directio`. On
  the write, append and read flowops, `directio` only changes buffer alignment,
  so the workload issues buffered I/O, like `fileserver_f2fs.f`.
- Do not add `directio` to `createfile1`. `O_DIRECT` then takes effect, and the
  run fails at the first write whose size is not a multiple of the block size
  (file sizes are gamma-distributed).

## License

The personalities are derived from Filebench workloads and are covered by the
Common Development and Distribution License 1.0 ([LICENSE](LICENSE), copied
verbatim from Filebench). Each file keeps the CDDL header and copyright notice
of the Filebench workload it is based on, followed by
`Portions Copyright 2025-2026 The CoFDP Authors`.
