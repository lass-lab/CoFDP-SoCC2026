# Known issues

Known defects of the CoFDP code in this repository. Unless the status says
*resolved*, they are not fixed. Code comments and component documents refer to
them by ID, for example `see KNOWN_ISSUES (XFS-4)`. IDs are stable labels; their
numbering has gaps.

Each entry describes what happens and gives:

- **Trigger**: when it happens.
- **Experiment path**: whether the recipes in [`scripts/`](scripts/README.md)
  reach it.
- **Where**: files (relative to the repository root) and functions.
- **Status**: *kept to preserve published behaviour* (a fix would change data
  placement, device commands, on-disk images or measurement output), *not on
  the experiment path*, or *resolved* (the entry describes the current
  behaviour).

Entries marked *(documentation only)* are not referenced from the code.

| Section | Directory | IDs |
|---|---|---|
| [Linux block layer, NVMe driver and iomap](#linux-block-layer-nvme-driver-and-iomap) | `linux-fdp/` | BLK-n |
| [F2FS (PiDir-FS)](#f2fs-pidir-fs) | `linux-fdp/fs/f2fs/` | F2FS-n |
| [XFS (PiDir-FS)](#xfs-pidir-fs) | `linux-fdp/fs/xfs/` | XFS-n |
| [f2fs-tools (mkfs.f2fs)](#f2fs-tools-mkfsf2fs) | `f2fs-tools-fdp/` | MKFS-n |
| [xfsprogs (mkfs.xfs)](#xfsprogs-mkfsxfs) | `xfsprogs-fdp/` | XFSPROGS-n |
| [ConfFDP (FEMU)](#conffdp-femu) | `ConfFDP/` | FEMU-n, FEMU-Ln |
| [RocksDB (CoFDP fork)](#rocksdb-cofdp-fork) | `FDP_ROCKSDB_F2FS/` | RDB-n |
| [RocksDB + TorFS](#rocksdb--torfs) | `FDP_ROCKSDB_TORFS/` | TORFS-n |
| [CacheLib](#cachelib) | `FDP_CACHELIB/`, `configs/cachelib/` | CL-n |
| [YCSB](#ycsb) | `YCSB-cpp-fdp/` | YCSB-n |
| [Filebench](#filebench) | `filebench-fdp/` | SCR-1, SCR-2 |
| [Blockchain](#blockchain) | `blockchain-fdp/`, `scripts/blockchain/` | BC-n |
| [Tools](#tools) | `tools/` | none |
| [Scripts](#scripts) | `scripts/` | SCR-n |

## Linux block layer, NVMe driver and iomap

See also XFS-2 and XFS-3 (PID granularity of iomap writeback).

### BLK-1: `bio_init()` leaves `bio->fdp_pid` uninitialised

`bio_init()` does not set `fdp_pid`; only `bio_reset()` clears it. A bio whose
submitter does not set a PID carries whatever its memory held: bios from a bio
set or the per-CPU bio cache keep the PID of their previous user (the bio slabs
are `SLAB_TYPESAFE_BY_RCU`, so `init_on_alloc` does not clear them), and
`bio_kmalloc()` and on-stack bios carry uninitialised memory. The NVMe driver
sends the value of a write as DSPEC, and the block layer does not merge I/Os
whose `fdp_pid` differs. The PiDir-FS data, node and metadata writes of F2FS
and XFS set their PID and are not affected.

- **Trigger**: writes whose submitter sets no PID, e.g. mkfs.f2fs writing
  through the block-device page cache, mkfs.xfs zeroing its log (zero-page
  writes, because ConfFDP does not offer Write Zeroes) and its O_DIRECT
  metadata writes (on-stack bio of `__blkdev_direct_IO_simple()`), XFS log
  recovery (`xfs_rw_bdev()`); reads and discards only for merging.
- **Experiment path**: yes. After earlier I/O in the same boot, the metadata
  writes of mkfs can carry the PID of an earlier write.
- **Where**: `linux-fdp/block/bio.c` `bio_init()`; `fdp_pid` in
  `linux-fdp/include/linux/blk_types.h`.
- **Status**: kept to preserve published behaviour.

### BLK-2: Writes do not set DTYPE, and PIDs are sent as plain indexes

`nvme_setup_rw()` puts the PID into DSPEC (CDW13[31:16]) of every write but
leaves the directive type DTYPE (CDW12[23:20]) at 0. A device that follows the
NVMe FDP specification ignores DSPEC unless DTYPE is 2 (data placement) and
writes to its default placement handle; ConfFDP decodes DSPEC without checking
DTYPE. PiDir-FS sends PIDs below 32 (F2FS: the per-PID log index, XFS: the bit
number), which are the device's placement identifiers only when the device
numbers them from 0 without gaps, as ConfFDP does (RG = PID / placement
handles per RG).

- **Trigger**: writes to an FDP SSD that follows the specification.
- **Experiment path**: yes; ConfFDP is not affected.
- **Where**: `linux-fdp/drivers/nvme/host/core.c` `nvme_setup_rw()`.
- **Status**: kept to preserve published behaviour.

### BLK-3: Writes leave CDW13[15:0] unwritten

To keep DSPEC, `nvme_setup_rw()` does not store the dataset management word for
writes. CDW13[15:0] (dataset management hints and a reserved byte) of a write
therefore holds what the previous command on the same tag left in the
per-request command buffer, e.g. the access frequency 7 (prefetch) of a
readahead read.

- **Trigger**: a write that reuses the tag of a command that set these bits.
- **Experiment path**: yes; ConfFDP ignores these bits.
- **Where**: `linux-fdp/drivers/nvme/host/core.c` `nvme_setup_rw()`;
  `struct nvme_rw_command` in `linux-fdp/include/linux/nvme.h`.
- **Status**: kept to preserve published behaviour.

### BLK-4: Direct I/O does not pass PID 0

`iomap_dio_submit_bio()` copies `iomap->fdp_pid` into the bio only when it is
not 0. A direct write whose PID is 0 keeps the stale `fdp_pid` of its bio
(BLK-1) and is placed under an unrelated PID. The same applies to direct writes
of files without PID bits and of other iomap file systems, which report PID 0.
Buffered writeback passes PID 0.

- **Trigger**: O_DIRECT writes whose PID is 0 (F2FS: PISA log 0, e.g.
  P-Directory `p0`; XFS: a PID pick of 0), or to files without PID bits.
- **Experiment path**: no. The direct-I/O recipes use P-Directories without
  PID 0.
- **Where**: `linux-fdp/fs/iomap/direct-io.c` `iomap_dio_submit_bio()`.
- **Status**: not on the experiment path.

## F2FS (PiDir-FS)

### F2FS-1: Initial segments of per-PID logs are untyped in the SIT

The first segment of every per-PID log other than log 0 is set by mkfs.f2fs
without a SIT type (MKFS-1). The type that the log sets when it is restored at
mount is overwritten when the SIT is loaded, so these segments are typed
HOT_DATA, node logs included. Garbage collection takes the type from the SIT:
it handles such a node segment as a data segment, migrates nothing and cannot
reclaim it until its blocks are invalidated elsewhere. The upstream check that
compares the SSA and SIT types, which would stop GC with an error, is disabled.

- **Trigger**: GC selects the initial segment of a per-PID log
  (`fdp_log_n` > 1).
- **Experiment path**: yes, once GC runs.
- **Where**: `linux-fdp/fs/f2fs/gc.c` `do_garbage_collect()`;
  `linux-fdp/fs/f2fs/segment.c` `build_curseg()`, `build_sit_entries()`.
- **Status**: kept to preserve published behaviour.

### F2FS-2: Per-PID logs are not checkpointed

A checkpoint records the position (segment, block offset, allocation type) and
the summaries of log 0 of each type only; the checkpoint slots of the other
logs keep the values that mkfs.f2fs wrote. When the file system is mounted
again, logs 1 to `fdp_log_n` - 1 restart at their mkfs segments with block
offset 0 although these segments may hold valid blocks, and after a checkpoint
without compacted summaries every data log restores the segment and offset of
log 0. The mount can fail its consistency checks, or later writes can overwrite
valid blocks.

- **Trigger**: mounting a PiDir-FS file system that was mounted and written
  before, also after a crash.
- **Experiment path**: no. The recipes format the device before every mount.
- **Where**: `linux-fdp/fs/f2fs/checkpoint.c` `do_checkpoint()`;
  `linux-fdp/fs/f2fs/segment.c` `read_compacted_summaries()`,
  `read_normal_summaries()`, `write_compacted_summaries()`,
  `write_normal_summaries()`.
- **Status**: not on the experiment path.

### F2FS-3: Roll-forward recovery ignores per-PID logs

The fsync'ed node blocks of a file whose PISA log is not 0 go to a per-PID warm
node log, but roll-forward recovery follows only the warm node chain of log 0,
so data and metadata that such files fsync'ed after the last checkpoint are
lost in a crash. After recovery, `f2fs_allocate_new_segments()` moves only log
0 of each type to a new segment.

- **Trigger**: a crash after an fsync without a later checkpoint.
- **Experiment path**: no.
- **Where**: `linux-fdp/fs/f2fs/recovery.c` `find_fsync_dnodes()`,
  `recover_data()`; `linux-fdp/fs/f2fs/segment.c` `__allocate_new_segment()`.
- **Status**: not on the experiment path.

### F2FS-4: Checkpoint slots of per-PID logs are not validated

The checkpoint sanity check covers the slots of log 0 only; the slots of the
per-PID logs are used as they are, and `fdp_log_n` is not checked against the
layout of the image. A file system made without `-O lost_found` holds NULL_SEGNO
(0xffffffff) in slots 3 to 63, so mounting it with `fdp_log_n` > 1 restores logs
from invalid segment numbers and indexes the SIT out of bounds.

- **Trigger**: mounting a file system made without `-O lost_found` (or with a
  damaged checkpoint) with `fdp_log_n` > 1.
- **Experiment path**: no. The KernelFDP-F2FS recipes mount with `fdp_log_n=1`.
- **Where**: `linux-fdp/fs/f2fs/super.c` `f2fs_sanity_check_ckpt()`;
  `linux-fdp/fs/f2fs/segment.c` `read_compacted_summaries()`,
  `read_normal_summaries()`.
- **Status**: not on the experiment path.

### F2FS-5: Every log starts initialised on segment 0

Unlike upstream, `build_curseg()` puts every log on segment 0 and marks it
initialised, including the pinned and ATGC logs, which are not restored from
the checkpoint. At every checkpoint the pinned log then writes its empty
summary block over the summary block (SSA) of segment 0, which holds valid
blocks, and segment 0 counts as a current segment, so it is never cleaned or
trimmed.

- **Trigger**: every checkpoint.
- **Experiment path**: yes.
- **Where**: `linux-fdp/fs/f2fs/segment.c` `build_curseg()`,
  `__f2fs_save_inmem_curseg()`.
- **Status**: kept to preserve published behaviour.

### F2FS-6: Slack space recycling can choose a full segment *(documentation only)*

For SSR allocation, `f2fs_get_victim()` checks that a candidate segment has a
free block only under `checkpoint=disable`, as in upstream 6.7.9. A segment
whose free blocks were filled after the last checkpoint can be chosen; the next
allocation from it trips a bug check, writes one entry past the summary block
and places the block in the following segment. Many logs make this more likely.

- **Trigger**: SSR allocation (adaptive mode at high utilisation).
- **Experiment path**: possible when a file system runs nearly full.
- **Where**: `linux-fdp/fs/f2fs/gc.c` `f2fs_get_victim()`;
  `linux-fdp/fs/f2fs/segment.c` `change_curseg()`, `f2fs_allocate_data_block()`.
- **Status**: kept to preserve published behaviour.

### F2FS-7: Direct I/O allocates blocks from log 0

In PiDir-FS, `f2fs_iomap_begin()` tags the direct I/O write bios of a mapping
with the PISA log of the inode, but `f2fs_allocate_data_block()` uses log 0
when it is called without an `f2fs_io_info`, as for the blocks that direct
writes allocate and for garbage collection of encrypted, compressed or verity
files. The device places this data by the PID of the bio, but the F2FS segments
of log 0 mix data of several PIDs; for a Flexible P-Directory the PID is drawn
per mapping, independently of the allocation. PID 0 is not passed to the bio
(BLK-4).

- **Trigger**: O_DIRECT writes that allocate blocks in a file whose PISA log is
  not 0.
- **Experiment path**: yes, in the direct-I/O recipes on F2FS
  (`scripts/tenants/fig14.sh`, `scripts/elasticity/fig16a-cachelib.sh`,
  `scripts/elasticity/fig16b-fio.sh`).
- **Where**: `linux-fdp/fs/f2fs/data.c` `f2fs_iomap_begin()`,
  `__allocate_data_block()`; `linux-fdp/fs/f2fs/segment.c`
  `f2fs_allocate_data_block()`; `linux-fdp/fs/f2fs/gc.c` `move_data_block()`.
- **Status**: kept to preserve published behaviour.

### F2FS-8: fp-remap recurses and holds the lock of every directory on its path

`fp_remap_walk_dir()` recurses once per directory level and keeps the `i_rwsem`
of every directory on the current path, with a pinned dentry page and a stack
frame per level, until the subtree is done. A deep tree can overflow the
kernel stack, a lockdep kernel reports recursive locking, lookups in the locked
directories wait for the walk, and a damaged directory whose entry names the
directory itself or an ancestor deadlocks the walk.

- **Trigger**: fp-remap on a deep tree, on a lockdep kernel or on a damaged
  image.
- **Experiment path**: no. The recipes remap shallow trees.
- **Where**: `linux-fdp/fs/f2fs/file.c` `fp_remap_walk_dir()`,
  `fp_remap_walk_dentries()`.
- **Status**: not on the experiment path.

### F2FS-9: fp-remap persists PID bits through inode write-back only

`fp_remap_set_inode_pid()` updates the Inode-to-PID table and `i_generation`
(PinodeBlock) and calls `mark_inode_dirty()`, which does not add the inode to
the inodes that a checkpoint writes. The new bits reach the disk with regular
inode write-back or at sync or umount, so after a crash before that, the files
and directories come back with their old PID bits. An inode write-back that
runs concurrently with the walk can also put the old bits back into the
Inode-to-PID table until the inode is written again.

- **Trigger**: a crash shortly after fp-remap; inode write-back during
  fp-remap.
- **Experiment path**: no.
- **Where**: `linux-fdp/fs/f2fs/file.c` `fp_remap_set_inode_pid()`;
  `linux-fdp/fs/f2fs/inode.c` `f2fs_update_inode()`.
- **Status**: not on the experiment path.

### F2FS-10: fp-remap is not serialised against itself or renames

`f2fs_ioc_fp_remap()` updates the target directory before the walk takes its
lock, and each child directory is updated under its parent's lock. Two
concurrent fp-remaps of one directory can leave the directory with one bitmap
and its descendants with the other; an entry renamed during the walk from a
directory not yet visited into one already visited keeps its old PIDs; open
unlinked and O_TMPFILE inodes are not reached.

- **Trigger**: concurrent fp-remap calls on one tree; renames during fp-remap.
- **Experiment path**: no. The recipes call fp-remap one at a time.
- **Where**: `linux-fdp/fs/f2fs/file.c` `f2fs_ioc_fp_remap()`,
  `fp_remap_walk_dentries()`.
- **Status**: not on the experiment path.

### F2FS-11: Block replacement looks up PISA by node ID

When the segment of a replaced block is not a current segment of a log of its
type, `f2fs_do_replace_block()` calls `f2fs_pisa_log()` with `sum->nid`, the ID
of the node that holds the block address. That is the inode number only for
blocks addressed from the inode; for blocks addressed from a direct node, the
Inode-to-PID table is read at an unrelated index (usually 0, i.e. log 0).

- **Trigger**: roll-forward recovery, and moving blocks between files with
  `F2FS_IOC_MOVE_RANGE` or `FALLOC_FL_COLLAPSE_RANGE`/`FALLOC_FL_INSERT_RANGE`.
- **Experiment path**: no.
- **Where**: `linux-fdp/fs/f2fs/segment.c` `f2fs_do_replace_block()`.
- **Status**: not on the experiment path.

### F2FS-12: The discard thread keeps polling when SBI_NEED_FSCK is set

Unlike upstream, `issue_discard_thread()` does not skip a round when
SBI_NEED_FSCK is set or no discard command is cached, while
`__submit_discard_cmd()` issues nothing once SBI_NEED_FSCK is set. After an
inconsistency sets the flag, every round walks all pending discard commands
under the command lock without issuing any, and no pending discard is issued
again.

- **Trigger**: anything that sets SBI_NEED_FSCK on a mount with real-time
  discard (e.g. F2FS-1).
- **Experiment path**: only after SBI_NEED_FSCK is set.
- **Where**: `linux-fdp/fs/f2fs/segment.c` `issue_discard_thread()`,
  `__submit_discard_cmd()`.
- **Status**: kept to preserve published behaviour.

### F2FS-13: Checkpoints wait for the discards in flight

After waking the discard thread, `f2fs_clear_prefree_segments()` waits until
all submitted discard commands have completed (background and forced
policies). It runs inside the checkpoint while file system operations are
blocked, so every checkpoint on a discard mount also waits for the discards in
flight, including those that the woken thread issues meanwhile.

- **Trigger**: every checkpoint on a mount with real-time discard.
- **Experiment path**: yes. The recipes mount with `discard`.
- **Where**: `linux-fdp/fs/f2fs/segment.c` `f2fs_clear_prefree_segments()`.
- **Status**: kept to preserve published behaviour.

### F2FS-14: The statistics thread prints some wrong values

The statistics thread, started on every mount with real-time discard, prints
its counters once per second. Its output format is fixed and has these
defects: the per-log write counters hold 8 logs per temperature, so with
`fdp_log_n` > 8 the counters of adjacent temperatures overlap; the
`read submit` bytes are read from the bio after `submit_bio()`, when the bio
may have completed and been freed; the `META` line has five conversions for
four arguments (the last value is undefined) and computes its average as
bytes >> (12 / count); several 64-bit counters are printed with `%u` (low 32
bits only); the counters are updated without synchronisation; and the thread
sleeps 1 s between rounds, which can delay umount by up to 1 s.

- **Trigger**: every mount with real-time discard.
- **Experiment path**: yes.
- **Where**: `linux-fdp/fs/f2fs/segment.c` `cofdp_stat_thread()`;
  `linux-fdp/fs/f2fs/data.c` `f2fs_submit_read_bio()`,
  `f2fs_submit_write_bio()`; `struct f2fs_sb_info` in
  `linux-fdp/fs/f2fs/f2fs.h`.
- **Status**: kept to preserve published behaviour.

## XFS (PiDir-FS)

### XFS-1: The AG that PISA picks is not used by the allocator

`xfs_bmap_btalloc_pisa()` picks the PID of a new extent and looks up (and, when
that AG runs short of space, moves) the AG assigned to the PID, but the
allocator does not use it: `xfs_bmap_btalloc_best_length()` targets the inode
and its neighbouring extents through `ap->blkno`, and
`xfs_alloc_vextent_check_args()` resets `args->fsbno` before every allocation.
Data placement therefore follows stock XFS locality; only the PID pick is used,
to tag the writeback bios of the extent.

- **Trigger**: every allocation for an inode with PID bits.
- **Experiment path**: yes.
- **Where**: `linux-fdp/fs/xfs/libxfs/xfs_bmap.c` `xfs_bmap_btalloc_pisa()`;
  `linux-fdp/fs/xfs/xfs_cofdp.c` `xfs_cofdp_pisa_select_ag()`,
  `xfs_cofdp_pisa_update_ag()`.
- **Status**: kept to preserve published behaviour.

### XFS-2: Writeback uses one PID per ioend

Writeback tags an ioend with the PID of the mapping that started it. Only an
ioend that consists of its inline bio gets a freshly picked PID when it is
submitted; the bios of a larger ioend (up to 4096 folios) all keep the first
PID, because iomap has submitted all but the last of them before the ioend is
prepared. For a Flexible P-Directory, large sequential writeback therefore uses
one PID per ioend instead of spreading per bio, and a single-bio ioend can
carry another PID than the one PISA picked for the extent.

- **Trigger**: writeback of more than one bio of contiguous dirty data of a
  file with PID bits.
- **Experiment path**: yes.
- **Where**: `linux-fdp/fs/xfs/xfs_cofdp.c` `xfs_cofdp_tag_ioend()`;
  `linux-fdp/fs/xfs/xfs_aops.c` `xfs_prepare_ioend()`;
  `linux-fdp/fs/iomap/buffered-io.c` `iomap_chain_bio()`.
- **Status**: kept to preserve published behaviour.

### XFS-3: iomap adds mappings with another PID to the open ioend

`iomap_can_add_to_ioend()` does not compare PIDs. A writeback mapping that is
physically contiguous with the open ioend is added to it even if it carries
another PID (a new PISA pick, a new pick for a Flexible P-Directory, or a
bitmap changed by fp-remap), and its blocks are written with the PID of the
ioend. After an fp-remap, up to one ioend of each file under writeback still
goes to the old PIDs; single-bio ioends are picked again (XFS-2).

- **Trigger**: contiguous writeback across mappings with different PIDs, e.g.
  fp-remap during writeback.
- **Experiment path**: yes, in the fp-remap recipes on XFS.
- **Where**: `linux-fdp/fs/iomap/buffered-io.c` `iomap_can_add_to_ioend()`.
- **Status**: kept to preserve published behaviour.

### XFS-4: PID bits are not carried in log records

The PinodeBlock (PID bits in `di_pad2`) is written when an inode is flushed,
but the logged copy of an inode zeroes `di_pad2`, and log recovery zeroes it
again when it replays the inode. After an unclean shutdown, every inode
recovered from the log loses its PID bits; only the Initial P-Directories get
theirs back at mount.

- **Trigger**: a crash or shutdown with inodes in the active log, then mount.
- **Experiment path**: no.
- **Where**: `linux-fdp/fs/xfs/xfs_inode_item.c` `xfs_inode_to_log_dinode()`;
  `linux-fdp/fs/xfs/xfs_inode_item_recover.c` `xfs_log_dinode_to_disk()`;
  `linux-fdp/fs/xfs/libxfs/xfs_inode_buf.c` `xfs_inode_to_disk()`.
- **Status**: not on the experiment path.

### XFS-5: fp-remap changes PID bits in memory only

fp-remap, and the reset of the Initial P-Directories at mount (XFS-14), set
`i_pid_bitmap` and call `mark_inode_dirty()`, which does not log an XFS inode.
The new bits reach the PinodeBlock only when a later transaction logs the
inode, e.g. an allocating write or a new file in the directory. An inode that
no transaction touches comes back with its old bits after it is evicted from
the inode cache or the file system is mounted again, and a Flexible
P-Directory that comes back this way gives its old PIDs to new files.

- **Trigger**: fp-remap, then inode cache eviction or a remount.
- **Experiment path**: fp-remap runs in the XFS recipes; the old bits come back
  only after eviction or a remount.
- **Where**: `linux-fdp/fs/xfs/xfs_cofdp.c` `xfs_ioc_cofdp_remap()`,
  `xfs_cofdp_remap_actor()`, `xfs_cofdp_init_pdir()`.
- **Status**: kept to preserve published behaviour.

### XFS-7: The fp-remap walk recurses once per directory level

`xfs_cofdp_remap_dir()` and `xfs_cofdp_remap_actor()` recurse through
`xfs_readdir()` for every subdirectory, with about 0.4 KiB of kernel stack per
level, so a tree a few dozen levels deep can overflow the kernel stack.

- **Trigger**: fp-remap on a deep tree.
- **Experiment path**: no. The recipes remap shallow trees.
- **Where**: `linux-fdp/fs/xfs/xfs_cofdp.c` `xfs_cofdp_remap_dir()`,
  `xfs_cofdp_remap_actor()`.
- **Status**: not on the experiment path.

### XFS-8: Only `xfs_create()` sets the PID bits of a new inode

`i_pid_bitmap` is not reset when an in-core inode is allocated, and only
`xfs_create()` sets it from the parent directory. O_TMPFILE inodes (also used
for whiteouts) and symlinks keep the value of the inode that last used the
memory, and inode flushes write it to disk; an O_TMPFILE file linked into a
P-Directory is written with an unrelated PID.

- **Trigger**: O_TMPFILE files, symlinks, whiteouts.
- **Experiment path**: no.
- **Where**: `linux-fdp/fs/xfs/xfs_icache.c` `xfs_inode_alloc()`;
  `linux-fdp/fs/xfs/xfs_inode.c` `xfs_create_tmpfile()`;
  `linux-fdp/fs/xfs/xfs_symlink.c` `xfs_symlink()`;
  `linux-fdp/fs/xfs/xfs_inode.h` `xfs_get_initial_pid()`.
- **Status**: not on the experiment path.

### XFS-10: No feature bit marks the PID count

`sb_fdp_pids` is the superblock byte at offset 281 (`XFS_SB_FDP_PIDS_OFF`), in
padding that only file systems with the metadir feature define, and no feature
bit marks a PiDir-FS file system. Tools and kernels without CoFDP treat the byte
as padding: xfs_repair zeroes it (XFSPROGS-1), and a stock kernel mounts the
file system and clears the PID bits of every inode it writes.

- **Trigger**: a PiDir-FS file system handled by xfs_repair or by a kernel
  without CoFDP.
- **Experiment path**: no.
- **Where**: `XFS_SB_FDP_PIDS_OFF` in `linux-fdp/fs/xfs/libxfs/xfs_format.h`;
  `linux-fdp/fs/xfs/libxfs/xfs_sb.c`.
- **Status**: kept to preserve published behaviour.

### XFS-11: Direct I/O uses one PID per mapping and does not pass PID 0

`xfs_direct_write_iomap_begin()` tags each mapping with one PID picked from the
inode's PID bits, so every bio of the mapping carries that PID, and a pick of
PID 0 is not passed to the bio (BLK-4).

- **Trigger**: O_DIRECT writes to files with PID bits.
- **Experiment path**: yes, in the direct-I/O recipes on XFS; their
  P-Directories do not include PID 0.
- **Where**: `linux-fdp/fs/xfs/xfs_iomap.c` `xfs_direct_write_iomap_begin()`;
  `linux-fdp/fs/iomap/direct-io.c` `iomap_dio_submit_bio()`.
- **Status**: kept to preserve published behaviour.

### XFS-12: A delalloc conversion race keeps the PID of the previous mapping

When `xfs_bmapi_convert_delalloc()` finds the extent already converted by a
racing thread, it returns the mapping without setting `iomap->fdp_pid`, so
writeback keeps the PID of the previous mapping of the same writepages call (0
for the first). Already allocated COW-fork mappings behave the same way.

- **Trigger**: fsync or `sync_file_range()` writeback racing with background
  writeback of the same delalloc extent.
- **Experiment path**: possible, rare.
- **Where**: `linux-fdp/fs/xfs/libxfs/xfs_bmap.c`
  `xfs_bmapi_convert_delalloc()`.
- **Status**: kept to preserve published behaviour.

### XFS-14: Initial P-Directories are reset at every mount

At every read-write mount, each existing `p<i>` is mapped back to PID *i*, even
if it was remapped with fp-remap, while its files keep their remapped bits.
The Initial P-Directories are not created on a remount from read-only to
read-write.

- **Trigger**: a mount after fp-remap of an Initial P-Directory; a read-only
  mount followed by `remount,rw`.
- **Experiment path**: no.
- **Where**: `linux-fdp/fs/xfs/xfs_cofdp.c` `xfs_cofdp_init_pdir()`,
  `xfs_cofdp_create_initial_pdirs()`; `linux-fdp/fs/xfs/xfs_super.c`
  `xfs_fs_fill_super()`.
- **Status**: not on the experiment path.

### XFS-16: The fp-remap walk holds the locks of every directory on its path

The walk keeps the shared IOLOCK, and in block and leaf directories the
directory data buffer, of every ancestor while it descends, so lookups and
readdir in those directories wait for the whole walk. Parent and child IOLOCKs
use the same lockdep subclass (a lockdep kernel reports recursive locking), and
`xfs_readdir()` is called with a buffer size of 0, which disables directory
readahead.

- **Trigger**: fp-remap of a large tree in use; lockdep kernels.
- **Experiment path**: fp-remap runs in the XFS recipes on small trees.
- **Where**: `linux-fdp/fs/xfs/xfs_cofdp.c` `xfs_cofdp_remap_dir()`,
  `xfs_cofdp_remap_actor()`.
- **Status**: kept to preserve published behaviour.

## f2fs-tools (mkfs.f2fs)

### MKFS-1: Segments of per-PID logs are not typed in the SIT

With `-O lost_found`, `f2fs_write_check_point_pack()` puts the per-PID logs
into checkpoint slots 3 to 63, but the SIT journal covers only the six segments
of log 0; the SIT entries of the other segments stay zero, i.e. typed HOT_DATA.
This is the on-disk half of F2FS-1.

- **Trigger**: every `mkfs.f2fs -O lost_found`.
- **Experiment path**: yes.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format.c`
  `f2fs_write_check_point_pack()`.
- **Status**: kept to preserve published behaviour.

### MKFS-2: The root's link count leaves out the Initial P-Directories

The root inode gets `i_links` 3 (2 plus lost+found), although it has one
subdirectory more per Initial P-Directory. fsck.f2fs reports the mismatch, and
after three subdirectories of the root are removed, the link count of the root
reaches 0 and the root inode is deleted at the next umount.

- **Trigger**: fsck.f2fs; removing subdirectories of the root.
- **Experiment path**: no.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format.c` `f2fs_write_root_inode()`.
- **Status**: kept to preserve published behaviour.

### MKFS-3: The default quota usage leaves out the P-Directories

With `-O quota`, the default quota records count the root and lost+found only;
the Initial P-Directories are not counted, and fsck.f2fs reports the mismatch.

- **Trigger**: `mkfs.f2fs -O quota,lost_found`.
- **Experiment path**: no.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format.c` `f2fs_write_default_quota()`.
- **Status**: kept to preserve published behaviour.

### MKFS-4: P-Directory inodes point at themselves

The `next_blkaddr` of the node footer of an Initial P-Directory inode is its own
block address (root and lost+found point at the next block). Nothing follows
this chain: recovery walks only the warm node log.

- **Trigger**: every `mkfs.f2fs -O lost_found`.
- **Experiment path**: yes, without effect.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format.c` `f2fs_write_pdir_inode()`.
- **Status**: kept to preserve published behaviour.

### MKFS-5: P-Directory inodes lack the extra attribute fields

Unlike lost+found, the Initial P-Directory inodes are written without the
extra_attr, project quota, crtime and compression fields. With
`-O extra_attr,inode_checksum` the inode checksum is stored where such an inode
keeps `i_addr[2]`, so the directory gets a bogus block address that is used once
it needs a third dentry block.

- **Trigger**: `mkfs.f2fs -O extra_attr,inode_checksum,lost_found`, then many
  entries in a P-Directory.
- **Experiment path**: no.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format.c` `f2fs_write_pdir_inode()`.
- **Status**: kept to preserve published behaviour.

### MKFS-6: PID-tagged writes assume a 4 KiB LBA format on one whole namespace

`f2fs_pidir_write_block()` writes the blocks of the P-Directories with NVMe
passthrough, using the F2FS block address as starting LBA and one LBA per
block, on the first device. With a smaller LBA format, on a partition or with
several devices (`-c`), the blocks land at wrong addresses and the
P-Directories cannot be read.

- **Trigger**: `-O lost_found` on a 512-byte LBA format, a partition or a
  multi-device file system.
- **Experiment path**: no. The recipes use the whole namespace with 4 KiB LBAs.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format_pidir.c`
  `f2fs_pidir_write_block()`.
- **Status**: not on the experiment path.

### MKFS-7: Duplicate PIDs are not rejected *(documentation only)*

`f2fs_pidir_query_pids()` accepts 1 to 16 placement identifiers from the
reclaim unit handle status but does not check them for duplicates; a device
that reported a PID twice would get two Initial P-Directories with the same
name.

- **Trigger**: a device that reports duplicate PIDs.
- **Experiment path**: no. ConfFDP reports each PID once.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format_pidir.c`
  `f2fs_pidir_query_pids()`.
- **Status**: not on the experiment path.

### MKFS-8: A device too small for the layout is rejected after the 4 MiB wipe *(documentation only)*

The check that the per-PID log layout (131 zones) fits in the main area runs
while the superblock is prepared, after `-f` has zeroed the first 4 MiB of a
device that holds a file system. Such a device loses its old file system
although mkfs.f2fs fails. Failures of the PID discovery are detected before
anything is written.

- **Trigger**: `mkfs.f2fs -f -O lost_found` on a device with fewer than 131
  zones in the main area.
- **Experiment path**: no.
- **Where**: `f2fs-tools-fdp/mkfs/f2fs_format_main.c` `main()`;
  `f2fs-tools-fdp/mkfs/f2fs_format.c` `f2fs_pidir_prepare()`.
- **Status**: not on the experiment path.

## xfsprogs (mkfs.xfs)

### XFSPROGS-1: xfs_repair clears the PID count

On file systems without the metadir feature, `secondary_sb_whack()` treats the
superblock bytes after `sb_lsn`, `sb_fdp_pids` (byte 281) included, as unused
and zeroes them in the primary and every secondary superblock. `xfs_repair -n`
reports a fresh PiDir-FS file system as damaged, and after a repair the kernel
mounts it without PiDir-FS. See also XFS-10.

- **Trigger**: xfs_repair on a file system made with `-d fdp_pids=N` and
  `metadir=0`.
- **Experiment path**: no.
- **Where**: `xfsprogs-fdp/repair/agheader.c` `secondary_sb_whack()`;
  `xfsprogs-fdp/libxfs/xfs_sb.c` `__xfs_sb_from_disk()`.
- **Status**: not on the experiment path.

### XFSPROGS-2: libxfs drops the PID bits of the inodes it writes *(documentation only)*

The libxfs of xfsprogs does not know the PinodeBlock: `xfs_inode_to_disk()`
zeroes `di_pad2`, and `xfs_inode_from_disk()` does not read it. Tools that
rewrite inodes through libxfs, such as xfs_repair rebuilding a directory or
correcting a link count, clear the PID bits of those inodes.

- **Trigger**: xfs_repair and other libxfs writers on a PiDir-FS file system.
- **Experiment path**: no.
- **Where**: `xfsprogs-fdp/libxfs/xfs_inode_buf.c` `xfs_inode_to_disk()`,
  `xfs_inode_from_disk()`.
- **Status**: not on the experiment path.

### XFSPROGS-3: The `FDP mode:` line can show a geometry that changes later

mkfs.xfs prints `FDP mode: N PIDs, agcount=..., agsize=... blocks` when it sets
up one AG per PID. `align_ag_geometry()` can still change the AG size or count
afterwards (stripe or atomic-write alignment, dropping a too small last AG), and
then the line, or the one-AG-per-PID layout, no longer holds; mkfs.xfs does not
check it again.

- **Trigger**: `-d fdp_pids=N` with stripe options (`su`/`sw`) or on a device
  that reports a stripe or atomic-write unit.
- **Experiment path**: no.
- **Where**: `xfsprogs-fdp/mkfs/xfs_mkfs.c` `calculate_initial_ag_geometry()`,
  `align_ag_geometry()`.
- **Status**: kept to preserve published behaviour.

### XFSPROGS-4: xfs_db has no field for the PID count *(documentation only)*

xfs_db does not show `sb_fdp_pids`; the superblock `pad` field, shown on metadir
file systems only, starts after it. The kernel reports the PID count at mount
(`CoFDP: PiDir-FS with N PIDs over M AGs`).

- **Trigger**: inspecting a PiDir-FS superblock with xfs_db.
- **Experiment path**: no.
- **Where**: `xfsprogs-fdp/db/sb.c`.
- **Status**: not on the experiment path.

## ConfFDP (FEMU)

### FEMU-1: The FTL scan resumes at ring `rg_number` + 1

`fdpftl_thread()` polls the submission rings 1 to `nr_pollers`, but after
serving a request (and the background GC of every RG) it continues at ring
`rg_number` + 1 instead of the next ring. Under load the higher rings are served
first and rings 2 to `rg_number` can wait; completions still go to the right
ring.

- **Trigger**: several busy I/O queues with `multipoller_enabled=1`
  (`run-fdp.sh`).
- **Experiment path**: yes.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `fdpftl_thread()`.
- **Status**: kept to preserve published behaviour.

### FEMU-2: Trim deallocates one LBA too many

`fdpssd_trim()` deallocates `nlb` + 1 LBAs per Dataset Management range,
although the range length is a count. The page after each range is unmapped
and marked invalid even if it holds live data: garbage collection does not copy
it, so GC copies and the write amplification are undercounted by up to one page
per range, and later reads of it take no NAND time. The data itself stays
readable (the memory backend is not changed).

- **Trigger**: every deallocation (file system discards, TorFS deallocations).
- **Experiment path**: yes.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `fdpssd_trim()`.
- **Status**: kept to preserve published behaviour.

### FEMU-6: Trim ranges are namespace-relative

Reads and writes reach the FTL with device-wide LBAs (`nvme_rw()` adds the start
of the namespace), but `nvme_dsm()` passes Dataset Management ranges with
namespace-relative LBAs, which the FDP and multi-stream FTLs use as device-wide
LPNs. With several namespaces, a trim on namespace 2 or higher deallocates
pages of namespace 1.

- **Trigger**: `namespaces` > 1 and a deallocation on namespace 2 or higher.
- **Experiment path**: no (`namespaces=1`).
- **Where**: `ConfFDP/hw/femu/nvme-io.c` `nvme_dsm()`;
  `ConfFDP/hw/femu/fdp/fdpftl.c` `fdpssd_trim()`;
  `ConfFDP/hw/femu/mstream/msftl.c` `msssd_trim()`.
- **Status**: not on the experiment path.

### FEMU-7: Multi-stream trim keeps the mapping

The multi-stream FTL (`femu_mode=7`) marks trimmed pages invalid but keeps the
LPN-to-page mapping. Once GC has reused the block, a later overwrite or trim of
the LPN invalidates a page that now belongs to another LPN, which corrupts the
valid-page counts.

- **Trigger**: in `femu_mode=7`, trim an LBA, let GC reuse its old block, then
  overwrite or trim the LBA again.
- **Experiment path**: no. The recipes use `femu_mode=6`.
- **Where**: `ConfFDP/hw/femu/mstream/msftl.c` `msssd_trim()`.
- **Status**: not on the experiment path.

### FEMU-8: `sum_written` is always 0, and the start-up counters are uninitialised

The `sum_written` line of the dump printed by the FEMU reset is always 0: the
value is never accumulated. The write amplification is computed from the
`RG r STREAM h : written` lines instead (`scripts/common/femu-waf.py`). When
QEMU starts, `fdpssd_init_write_pointer()` prints these lines for counters that
are not initialised yet; they are not part of a dump.

- **Trigger**: every FEMU reset; every QEMU start.
- **Experiment path**: yes.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `fdpssd_femu_reset()`,
  `fdpssd_init_write_pointer()`.
- **Status**: kept to preserve published behaviour.

### FEMU-11: Device parameters are not validated

An `rg_number` that is not a power-of-two divisor of the number of dies makes
`rg2physical()` and `get_rg_id()` return -1 (out-of-bounds accesses);
`rg_number` or `handle_number` 0 and `gc_thres_pcent` 0 divide by zero; a
`nand_page_size_kb` other than 4 (the property default is 16) breaks the
assumption of the FDP and multi-stream FTLs that one LBA is one page.

- **Trigger**: a FEMU command line with such values.
- **Experiment path**: no. `run-fdp.sh` and `configs/femu/` use valid values.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `ssd_init_params()`,
  `rg2physical()`, `get_rg_id()`, `fdpssd_write()`; `FEMU_LBADS` in
  `ConfFDP/hw/femu/femu.c`.
- **Status**: not on the experiment path.

### FEMU-12: The FDP configurations log page does not describe the device

The FDP configurations log page (Get Log Page 20h) reports
`stream_number` x `rg_number` reclaim unit handles while the FTL has
`handle_number` x `rg_number`, an FDP attribute byte of 0 (RGIF 0) and a reclaim
unit size in blocks instead of bytes; it ignores the log page offset, and the
log identifier is taken from 16 bits instead of 8.

- **Trigger**: a host that reads the FDP configurations log, e.g. nvme-cli.
- **Experiment path**: no. mkfs.f2fs, the tools and the benchmarks read the
  reclaim unit handle status instead; the kernel reads neither.
- **Where**: `ConfFDP/hw/femu/fdp/fdp.c` `fdp_log_confs()`, `fdp_get_log()`.
- **Status**: not on the experiment path.

### FEMU-13: I/O Management errors are returned as latency

In the FDP and multi-stream FTLs, an I/O Management Receive with an unsupported
management operation returns `NVME_INVALID_FIELD | NVME_DNR` as the latency:
the command completes successfully after about 16 us. Commands that the FTL
thread does not handle keep the latency of the previous request.

- **Trigger**: I/O Management Receive with an unsupported management
  operation.
- **Experiment path**: no. fdp-reset and the reclaim unit handle status use
  supported operations.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `fdpssd_io_mgmt_send()`,
  `fdpssd_io_mgmt_recv()`, `fdpftl_thread()`; the same functions in
  `ConfFDP/hw/femu/mstream/msftl.c`.
- **Status**: not on the experiment path.

### FEMU-16: `req->nlb` is 0-based in every mode

`nvme_rw()` stores the 0-based block count of the command in `req->nlb`. The FDP
and multi-stream FTLs process LPNs `slba` to `slba` + `nlb`, but the black-box
FTL (`bbssd`, unchanged upstream code) treats `req->nlb` as a count, so in
`femu_mode=1` a one-block request computes a wrong LPN range.

- **Trigger**: `femu_mode=1` (`run-blackbox.sh`).
- **Experiment path**: no.
- **Where**: `ConfFDP/hw/femu/nvme-io.c` `nvme_rw()`;
  `ConfFDP/hw/femu/bbssd/ftl.c`.
- **Status**: not on the experiment path.

### FEMU-17: I/O Management commands succeed in every mode

I/O Management Send (management operations 0, 1 and 2) and Receive complete
successfully in every `femu_mode`, but only the FDP and multi-stream FTLs act on
them. In the other modes a reclaim unit handle status reply transfers no data
and the FEMU reset does nothing.

- **Trigger**: I/O Management commands in a `femu_mode` other than 6 and 7.
- **Experiment path**: no.
- **Where**: `ConfFDP/hw/femu/nvme-io.c` `nvme_io_mgmt_send()`,
  `nvme_io_cmd()`.
- **Status**: not on the experiment path.

### FEMU-18: NPDG and NPDA are computed from `secsz`

`nvme_ns_init_identify()` computes the preferred write granularity and alignment
as `nand_page_size_kb` x 1024 / `secsz`, although every LBA format has 4 KiB
blocks. With the default `secsz` of 512 it reports 32 LBAs, and with a `secsz`
larger than the NAND page it reports 65536.

- **Trigger**: FEMU started without `secsz=4096` (`run-fdp.sh` sets it).
- **Experiment path**: no.
- **Where**: `ConfFDP/hw/femu/femu.c` `nvme_ns_init_identify()`.
- **Status**: not on the experiment path.

### FEMU-19: Every mode uses a 4 KiB LBA format

All LBA formats have 4 KiB blocks without metadata (`FEMU_LBADS`) in every
`femu_mode`. The black-box FTL still maps LBAs to NAND pages with `secsz` and
`secs_per_pg` (512 B x 8 in `run-blackbox.sh`), so it models only about one
eighth of the NAND work of a request.

- **Trigger**: `femu_mode=1` with a `secsz` other than 4096.
- **Experiment path**: no.
- **Where**: `FEMU_LBADS` and `nvme_ns_init_identify()` in
  `ConfFDP/hw/femu/femu.c`; `ConfFDP/hw/femu/bbssd/ftl.c`.
- **Status**: not on the experiment path.

### FEMU-L2: Spill and redirect targets depend on the wall clock

When an RG is still at the high GC watermark after up to 128 forced garbage
collections, `fdpssd_write()` writes the whole request into another RG, and
forced GC copies the valid pages of a victim with less than 10% invalid pages
into another RG. The target is the current second modulo `rg_number`
(`fdp_spill_rg()`); it is not checked for free reclaim units and can be the
source RG. Placement under GC pressure therefore depends on the time, data of
one PID can end up in other RGs and in reclaim units written by other PIDs,
and if the target has no free reclaim unit left, FEMU aborts (`no free RU`).

- **Trigger**: an RG whose free reclaim units reach the high watermark while its
  victims hold mostly valid data, e.g. live data close to the capacity of the
  RGs that its PIDs map to.
- **Experiment path**: possible under GC pressure.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `fdp_spill_rg()`, `fdpssd_write()`,
  `do_gc()`.
- **Status**: kept to preserve published behaviour.

### FEMU-L3: PIDs beyond the device are folded into the last RG

`fdpssd_write()` computes the RG as PID / `handle_number` in 8 bits: PIDs from
256 x `handle_number` on wrap around into a valid RG, and the other PIDs beyond
the `rg_number` x `handle_number` PIDs of the device go to the last RG. The
first such write is reported once on stderr.

- **Trigger**: a write with a PID of `rg_number` x `handle_number` or more, e.g.
  TORFS-2.
- **Experiment path**: only through TORFS-2.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `fdpssd_write()`.
- **Status**: kept to preserve published behaviour.

### FEMU-L4: Redirected writes are counted for the requested RG

`fdpssd_write()` adds the host pages to the `written` counter of the requested
RG and placement handle before the write may be redirected to another RG
(FEMU-L2), so the dump attributes redirected pages to the RG that was full, not
to the RG that stored them. The totals, and with them the write amplification,
are not affected.

- **Trigger**: a redirected host write (FEMU-L2).
- **Experiment path**: as FEMU-L2.
- **Where**: `ConfFDP/hw/femu/fdp/fdpftl.c` `fdpssd_write()`.
- **Status**: kept to preserve published behaviour.

### FEMU-L5: PRACT without protection information is accepted in every mode

`femu_nvme_rw_check_req()` lets a read or write with PRACT set on a namespace
without protection information run as a plain read or write in every
`femu_mode` (upstream: OCSSD mode only), and records an Invalid Field entry in
the error log each time.

- **Trigger**: passthrough I/O with PRACT (CDW12 bit 29) set.
- **Experiment path**: no. Linux does not set PRACT on these namespaces.
- **Where**: `ConfFDP/hw/femu/nvme-util.c` `femu_nvme_rw_check_req()`.
- **Status**: not on the experiment path.

## RocksDB (CoFDP fork)

The P-Directory placement of the fork is described in
[`FDP_ROCKSDB_F2FS/docs/COFDP.md`](FDP_ROCKSDB_F2FS/docs/COFDP.md); N is the
number of P-Directories of a DB.

### RDB-1: The P-Directory fsync index is not bounds-checked

Placement clamps the P-Directory index of an SST file to the last P-Directory,
but `PDirPlacement::FsyncPDir()` does not: a compaction fsyncs P-Directory
`output_level + 1` and an atomic flush P-Directory 1, and when this index is N
or more, the descriptor is read past the end of the array. What is read there
depends on the heap: a value of 0 or less skips the fsync, an invalid descriptor
makes it fail (the error is ignored, RDB-2), and a valid one syncs whatever it
refers to. The P-Directory that received the new SST files is not synced.

- **Trigger**: compactions into level N - 1 or deeper; atomic flushes with
  N = 1.
- **Experiment path**: yes. The `cofdp` and `kernelfdp-pc` layouts of the
  recipes have three P-Directories, so compactions into level 2 and deeper read
  past the array.
- **Where**: `FDP_ROCKSDB_F2FS/env/pdir_placement.cc`
  `PDirPlacement::FsyncPDir()`; `FDP_ROCKSDB_F2FS/env/io_posix.cc`
  `PosixDirectory::FsyncWithDirOptions()`.
- **Status**: kept to preserve published behaviour.

### RDB-2: P-Directory fsync errors are ignored

`FsyncPDir()` ignores the result of `fsync()` and skips descriptors of 0 or
less. Non-atomic flushes and `SyncWAL()` fsync the DB directory, not the L0 or
WAL P-Directory.

- **Trigger**: flushes, compactions and `SyncWAL()` with P-Directories.
- **Experiment path**: yes.
- **Where**: `FDP_ROCKSDB_F2FS/env/pdir_placement.cc`
  `PDirPlacement::FsyncPDir()`; `FDP_ROCKSDB_F2FS/db/flush_job.cc`;
  `FDP_ROCKSDB_F2FS/db/db_impl/db_impl_write.cc`.
- **Status**: kept to preserve published behaviour.

### RDB-3: Placement state lives in the process

The level of each SST file is kept in memory only: a process that reopens a DB
with N > 0 looks for every SST file in P-Directory 0, and `GetChildren()` does
not list the files inside P-Directories.

- **Trigger**: reopening a DB with P-Directories in another process.
- **Experiment path**: no. Each recipe creates and uses its DB in one process.
- **Where**: `FDP_ROCKSDB_F2FS/env/pdir_placement.cc`;
  `FDP_ROCKSDB_F2FS/env/fs_posix.cc`.
- **Status**: not on the experiment path.

### RDB-4: P-Directories are ordered by the bytes of their names

`Discover()` sorts the P-Directories by the byte order of their names, not
numerically (`p10` before `p2`), and ignores entries whose names do not start
with `p` (e.g. `fp...`).

- **Trigger**: ten or more P-Directories; P-Directories named `fp...`.
- **Experiment path**: no. The layouts use `p0`, `p1` and `p2`.
- **Where**: `FDP_ROCKSDB_F2FS/env/pdir_placement.cc`
  `PDirPlacement::Discover()`.
- **Status**: not on the experiment path.

### RDB-5: Every `p*` entry counts, and the last registration applies to all DBs

`Discover()` registers every entry of the DB directory whose name starts with
`p`, regular files included, and the P-Directories of the most recently opened
DB apply to all DBs of the process.

- **Trigger**: a file named `p...` in the DB directory; several DBs in one
  process.
- **Experiment path**: no.
- **Where**: `FDP_ROCKSDB_F2FS/env/pdir_placement.cc`
  `PDirPlacement::Discover()`.
- **Status**: not on the experiment path.

### RDB-6: Path rewriting is not limited to the DB directory

`Resolve()` rewrites any `*.log` or `*.sst` path into a P-Directory of the
file's own directory, also outside the DB directory, while the operations that
are not rewritten (e.g. `LinkFile()`, `NewRandomRWFile()`) use the original
path.

- **Trigger**: `*.log` or `*.sst` files outside the DB directory; operations
  that are not rewritten.
- **Experiment path**: no.
- **Where**: `FDP_ROCKSDB_F2FS/env/pdir_placement.cc`
  `PDirPlacement::Resolve()`; `FDP_ROCKSDB_F2FS/env/fs_posix.cc`.
- **Status**: not on the experiment path.

### RDB-7: Deleted files stay in the resolution cache

`DeleteFile()` removes the resolved path instead of the logical one from the
resolution cache, so cache entries are never removed and the cache grows by one
entry per file.

- **Trigger**: every file deletion.
- **Experiment path**: yes (memory use only).
- **Where**: `FDP_ROCKSDB_F2FS/env/fs_posix.cc` `DeleteFile()`;
  `FDP_ROCKSDB_F2FS/env/pdir_placement.cc` `PDirPlacement::ForgetPath()`.
- **Status**: kept to preserve published behaviour.

### RDB-8: SCAN latency covers iterator creation only

The `SCAN` line of the ops timeline times `NewIterator()` only, not `Seek()` or
`Next()`.

- **Trigger**: every iterator.
- **Experiment path**: yes.
- **Where**: `FDP_ROCKSDB_F2FS/db/db_impl/db_impl.cc` `DBImpl::NewIterator()`;
  `FDP_ROCKSDB_F2FS/env/cofdp_stats.h`.
- **Status**: kept to preserve published behaviour.

### RDB-9: Timeline percentiles are power-of-two lower bounds

Each percentile of the ops timeline is the lower bound of the power-of-two
nanosecond bucket that contains it and can be up to 2x below the true value;
the average is exact.

- **Trigger**: every timeline row.
- **Experiment path**: yes.
- **Where**: `FDP_ROCKSDB_F2FS/env/cofdp_stats.cc`.
- **Status**: kept to preserve published behaviour.

### RDB-10: MixGraph never throttles

MixGraph does not apply the `--sine_mix_rate` and
`--benchmark_{read,write}_rate_limit` rate limiters.

- **Trigger**: mixgraph with `--sine_mix_rate` or a benchmark rate limit.
- **Experiment path**: no. The recipes set neither.
- **Where**: `FDP_ROCKSDB_F2FS/tools/db_bench_tool.cc` `Benchmark::MixGraph()`.
- **Status**: not on the experiment path.

### RDB-11: `--duration` does not lift the op limit

With `--duration`, a db_bench benchmark ends after the duration or after its op
count (`--num`, `--reads`, ...), whichever comes first; upstream ignores the op
count when `--duration` is set.

- **Trigger**: `--duration` together with an op count.
- **Experiment path**: no. The recipes do not set `--duration`.
- **Where**: `FDP_ROCKSDB_F2FS/tools/db_bench_tool.cc` `Duration::Done()`.
- **Status**: not on the experiment path.

## RocksDB + TorFS

Paths are relative to `FDP_ROCKSDB_TORFS/plugin/torfs/fs/`.

### TORFS-2: Level-separation PIDs are not bounded

With `stream_option=2`, WAL files get PID 1 and the SST files of level L get
PID L + 2 without a bound, so L5 shares PID 7 with the MANIFEST files and L6
uses PID 8, which a device with 8 PIDs does not have (ConfFDP puts it into its
last RG, FEMU-L3).

- **Trigger**: compaction output into L5 or L6.
- **Experiment path**: the UserFDP recipes use `stream_option=2`; whether L5 or
  L6 is reached depends on the amount of data.
- **Where**: `fs_torfs_io.cc` `FDPWritableFile::SetWriteLifeTimeHint()`.
- **Status**: kept to preserve published behaviour.

### TORFS-3: The reclaim unit handle status is parsed with a wrong layout

`struct nvme_fdp_ruh_status` pads the 14 reserved header bytes to 20, so the
descriptor count and the descriptors are read at offsets 20 and 24 instead of
14 and 16. The buffer is not cleared, the completion status is not checked, and
the descriptor loop is not bounded by the 256 descriptors of the buffer. On
ConfFDP the count reads as 0, so the parsed geometry is {0, 0}; on a device
whose first descriptor has a non-zero EARUTR, the loop reads far past the
buffer.

- **Trigger**: every TorFS start.
- **Experiment path**: yes; only `stream_option=4` uses the parsed values
  (TORFS-6).
- **Where**: `xnvme_be.h` `struct nvme_fdp_ruh_status`; `xnvme_be.cc`
  `XNvmeBackend::Init()`.
- **Status**: kept to preserve published behaviour.

### TORFS-5: Directory listings do not show TorFS files

TorFS creates no placeholder files in the host file system, while
`GetChildren()` lists only the host directory. RocksDB scans such as
`DestroyDB()`, the purge of obsolete files and the WAL discovery on reopen do
not see TorFS files: a second `fresh_db` benchmark in the same db_bench process
starts on the SST files of the first one, and old OPTIONS, LOG and WAL files
stay on the device.

- **Trigger**: more than one `fresh_db` benchmark per db_bench process.
- **Experiment path**: no. The recipes run one `fresh_db` benchmark per process.
- **Where**: `fs_torfs.cc` `TorFS::NewWritableFile()`, `TorFS::GetChildren()`.
- **Status**: not on the experiment path.

### TORFS-6: `stream_option=4` sends an uninitialised placement handle

The per-level reclaim group policy sets only the reclaim group byte of the PID;
the placement handle byte is uninitialised, and the largest reclaim group comes
from the wrongly parsed reclaim unit handle status (TORFS-3).

- **Trigger**: `stream_option=4`.
- **Experiment path**: no.
- **Where**: `fs_torfs_io.cc` `FDPWritableFile::SetWriteLifeTimeHint()`.
- **Status**: not on the experiment path.

### TORFS-7: Freed space is deallocated twice, and failures are not reported

`DeallocateWorker()` deallocates each merged free range in its size-dependent
branch and once more unconditionally (as upstream TorFS). The range size is
narrowed to 32 bits, so ranges of 4 GiB or more are deallocated only in part,
and `SubmitDeallocate()` does not report a failed Dataset Management command,
whose result the callers ignore.

- **Trigger**: every file deletion.
- **Experiment path**: yes.
- **Where**: `fs_torfs.cc` `TorFS::DeallocateWorker()`; `io_torfs.cc`
  `IOInterface::Deallocate()`; `xnvme_be.cc` `XNvmeBackend::SubmitDeallocate()`.
- **Status**: kept to preserve published behaviour.

### TORFS-8: Logs on TorFS carry meaningless timestamps

`TorFSLogger::Logv()` sets only the month and year of the `struct tm` it formats
(the other fields are uninitialised) and takes the sub-second part from
`CLOCK_MONOTONIC`, so the timestamps of the RocksDB info LOG and of the TorFS
log are not a date or time.

- **Trigger**: every log line.
- **Experiment path**: yes.
- **Where**: `torfs_logger.h` `TorFSLogger::Logv()`.
- **Status**: kept to preserve published behaviour.

### TORFS-10: TorFS logs its errors into the most recent logger

`NewLogger()` replaces `logger_` without synchronisation and makes the DB's info
LOG the sink of TorFS errors. If a flush of that LOG runs out of space, the
error is logged into the logger that is flushing, which can deadlock on its
mutex.

- **Trigger**: TorFS runs out of free space or buffer memory while the info LOG
  flushes.
- **Experiment path**: no.
- **Where**: `fs_torfs.cc` `TorFS::NewLogger()`, `TorFS::AllocCap()`.
- **Status**: not on the experiment path.

### TORFS-M1: TorFS metadata is not persistent

`FDP_META_ENABLED` is 0: the file map of TorFS lives in memory only, so every
process starts with an empty TorFS. A DB written by one process cannot be
opened by another (e.g. `db_bench --use_existing_db`, or separate YCSB load and
run processes); the new process creates a new DB over the data on the device.
Turning persistence on needs more than the switch, because `NewFDPFS()` creates
the TorFS log before `LoadMetaData()`, which may clear the file map.

- **Trigger**: using a TorFS DB from more than one process.
- **Experiment path**: no. Each recipe loads and runs its TorFS DB in one
  process.
- **Where**: `fs_torfs.h` `FDP_META_ENABLED`; `fs_torfs.cc`
  `TorFS::LoadMetaData()`.
- **Status**: kept to preserve published behaviour.

## CacheLib

Paths are relative to `FDP_CACHELIB/cachelib/` unless they start with
`configs/`. The HybridFDP mode and the UserFDP passthrough are described in
[`FDP_CACHELIB/COFDP.md`](FDP_CACHELIB/COFDP.md).

### CL-6: The MOS of I/O Management Receive is never sent

`FdpNvme::nvmeIOMgmtRecv()` builds CDW10 as
`(op & 0xf) | (op_specific & 0xff << 16)`; by operator precedence the MOS term
is always 0, and the management operation is cut to 4 bits.

- **Trigger**: an I/O Management Receive with a non-zero MOS or a management
  operation above 15.
- **Experiment path**: yes, without effect (the reclaim unit handle status uses
  MOS 0).
- **Where**: `navy/common/FdpNvme.cpp` `FdpNvme::nvmeIOMgmtRecv()`.
- **Status**: kept to preserve published behaviour.

### CL-9: HybridFDP sizes the NVM cache per file

In HybridFDP mode, `nvmCacheSizeMB` is the size of each of the two cache files,
so a HybridFDP configuration has twice the NVM cache of a single-device
configuration with the same value: `configs/cachelib/<workload>/cofdp.json`,
`kernel.json` and `pc.json` have 2 x 6144 MiB, `user.json` 6144 MiB.

- **Trigger**: comparing HybridFDP and single-device configurations.
- **Experiment path**: yes.
- **Where**: `cachebench/cache/Cache-inl.h` (HybridFDP files);
  `configs/cachelib/*/{cofdp,kernel,pc,user}.json`.
- **Status**: kept to preserve published behaviour.

### CL-10: GFL configurations use different key counts

`configs/cachelib/graph_cache_leader_fbobj/user.json` sets `numKeys` to
7,160,557, `cofdp.json` and `kernel.json` to 716,055.

- **Trigger**: comparing the GFL configurations.
- **Experiment path**: yes.
- **Where**: `configs/cachelib/graph_cache_leader_fbobj/`.
- **Status**: kept to preserve published behaviour.

### CL-12: UserFDP writes do not set DTYPE

Passthrough writes carry the PID in DSPEC (CDW13[31:16]) but leave the
directive type in CDW12 at 0, so a device that follows the FDP specification
ignores the placement handle; ConfFDP decodes DSPEC without checking DTYPE (see
also BLK-2).

- **Trigger**: UserFDP on an FDP SSD that follows the specification.
- **Experiment path**: yes; ConfFDP is not affected.
- **Where**: `navy/common/FdpNvme.cpp` `FdpNvme::prepFdpUringCmdSqe()`.
- **Status**: kept to preserve published behaviour.

### CL-13: The HybridFDP mode is not validated

The mode assumes exactly two directories listed in sorted order,
`navyBigHashSizePct` 50 and no cache persistence (`cacheDir`), but other
combinations are accepted: with one directory BigHash overlaps the metadata and
BlockCache in the single file, with another BigHash share an engine runs past
the end of its file, extra directories get files that are never used, and with
`cacheDir` set the metadata I/O divides by a stripe size of 0. BigHash starts at
offset 0 whenever the mode is configured, whatever the device layout.

- **Trigger**: `enableHyBridFDP` with any other combination.
- **Experiment path**: no. The configurations in `configs/cachelib/` use two
  sorted directories, 50% and no `cacheDir`.
- **Where**: `allocator/nvmcache/NavySetup.cpp` `setupBigHash()`,
  `createDevice()`; `cachebench/cache/Cache-inl.h`; `navy/common/Device.cpp`.
- **Status**: not on the experiment path.

### CL-14: A failed chunk of a split I/O can go unnoticed

With an asynchronous I/O engine (UserFDP), each read or write piece is issued
as consecutive 512 KiB I/Os, and only the result of the last one is returned:
an error in an earlier chunk is reported as success and counted as read or
written. The upstream log of failed I/Os in `IOOp::done()` is disabled.

- **Trigger**: a passthrough command that fails in the middle of a piece.
- **Experiment path**: the UserFDP recipes use this path; ConfFDP does not fail
  these commands.
- **Where**: `navy/common/Device.cpp` `Device::writeInternal()`,
  `Device::readInternal()`, `IOOp::done()`; `navy/common/Device.h`.
- **Status**: kept to preserve published behaviour.

### CL-15: The stddev of latency estimates is always 0

`PercentileStats` does not compute the standard deviation but still exports it
(`<prefix>_stddev`) and prints it in the API latency blocks of
`--report_api_latency`.

- **Trigger**: every latency estimate.
- **Experiment path**: yes.
- **Where**: `common/PercentileStats.h`, `common/PercentileStats.cpp`
  `PercentileStats::estimate()`; `cachebench/cache/CacheStats.h`.
- **Status**: kept to preserve published behaviour.

### CL-16: `testDurationSec` is checked every 1024 operations per thread

The stressors stop a thread only when its operation count is a multiple of 1024
and the deadline has passed, so threads run up to 1023 operations past the
duration and stop at different times; the run time and the throughput include
this tail.

- **Trigger**: configurations that set `test_config.testDurationSec`.
- **Experiment path**: yes, with `configs/cachelib/kvcache_l2_wc/config.json`.
- **Where**: `cachebench/runner/CacheStressor.h`,
  `cachebench/runner/AsyncCacheStressor.h`.
- **Status**: kept to preserve published behaviour.

### CL-17: The roles of the cache files and the F2FS pin are chosen differently

In HybridFDP mode the cache files are named `navy_cache<i>` in the order of
`nvmCachePaths`, the first file in sorted path order backs the small object
cache (BigHash), and the F2FS pin is applied to the file named `navy_cache0`. If
`nvmCachePaths` is not sorted, the pinned file is the large object cache.

- **Trigger**: an unsorted `nvmCachePaths` with `enableHyBridFDP`.
- **Experiment path**: no. All configurations list their directories in sorted
  order.
- **Where**: `navy/common/Device.cpp` `createFileDevice()` and the pin in the
  file device setup; `cachebench/cache/Cache-inl.h`.
- **Status**: not on the experiment path.

### CL-M1: UserFDP places each Navy engine on one placement handle

As in upstream CacheLib, BigHash and BlockCache use entries 1 and 2 of the
reclaim unit handle status as placement handles. ConfFDP lists its PIDs handle
by handle (0, 2, 4, 6, 1, 3, 5, 7 with 4 RGs x 2 handles), so the two engines
write PIDs 2 and 4, each within one RG.

- **Trigger**: every UserFDP configuration.
- **Experiment path**: yes.
- **Where**: `navy/common/FdpNvme.cpp` `FdpNvme::allocateFdpHandle()`;
  `navy/Factory.cpp`.
- **Status**: kept to preserve published behaviour.

### CL-N1: UserFDP serialises large I/O

The pieces of a read or write (up to the maximum transfer size of the device)
are issued as consecutive 512 KiB passthrough commands, each waiting for the
previous one, and the UserFDP configurations set `navyQDepth` 1. A region flush
or reclaim read is therefore a sequence of 512 KiB round trips.

- **Trigger**: every large UserFDP read or write.
- **Experiment path**: yes.
- **Where**: `navy/common/Device.cpp` `Device::writeInternal()`,
  `Device::readInternal()`.
- **Status**: kept to preserve published behaviour.

### CL-N2: The fixed workload seed does not make workloads reproducible

The generators of keys, value sizes and popularity are seeded from `srand()`
with a fixed seed (`kWorkloadSeed`), but the per-thread seeds are drawn by
threads that run concurrently, so which generator gets which seed depends on
scheduling; the request stream of the stressors is still seeded from
`folly::Random`.

- **Trigger**: every cachebench run.
- **Experiment path**: yes.
- **Where**: `cachebench/workload/WorkloadGenerator.cpp`.
- **Status**: kept to preserve published behaviour.

### CL-N3: Progress latency lines cover about the last minute

The NVM, SET and GET latency lines printed with `--progress` come from
sliding-window estimators with 1 s windows over about 60 s, so each line
describes roughly the last minute, not the last progress interval.

- **Trigger**: `--progress`.
- **Experiment path**: yes.
- **Where**: `cachebench/runner/ProgressTracker.cpp`;
  `common/PercentileStats.h`.
- **Status**: kept to preserve published behaviour.

## YCSB

### YCSB-2: Latency CSV files are shared and not cleaned up *(documentation only)*

Every status report rewrites `<hdrhistogram.csv_dir>/ycsbhist<OP>` for each
operation type that has samples. Concurrent ycsb processes with the same
directory overwrite each other's files, and the files of operation types that a
later phase does not execute keep the data of an earlier phase.

- **Trigger**: several ycsb processes with one `hdrhistogram.csv_dir`; phases
  with different operation types.
- **Experiment path**: the recipes run one ycsb at a time.
- **Where**: `YCSB-cpp-fdp/core/measurements.cc`
  `HdrHistogramMeasurements::WritePercentileCsv()`.
- **Status**: kept to preserve published behaviour.

### YCSB-3: ycsb always pauses 720 s after the load phase

`main()` sleeps 720 s after the load phase, also when it is skipped and when
only `-load` is given; the upstream `sleepafterload` property is ignored.

- **Trigger**: every ycsb invocation.
- **Experiment path**: yes.
- **Where**: `YCSB-cpp-fdp/core/ycsbc.cc` `main()`.
- **Status**: kept to preserve published behaviour.

### YCSB-4: The write-only workloads load keys from the request distribution

`workloads/workload_zipfian`, `workload_uniform` and `workload_latest` set
`loadkeydistribution=requestdistribution`: every insert of the load phase writes
a key drawn from the request distribution, so keys repeat and the load phase
overwrites records instead of creating `recordcount` distinct ones;
`Load operations(ops)` counts the inserts. The default, `sequential`, is the
YCSB load.

- **Trigger**: the three write-only workloads.
- **Experiment path**: yes.
- **Where**: `YCSB-cpp-fdp/core/core_workload.cc` `CoreWorkload::Init()`,
  `CoreWorkload::DoInsert()`; the three workload files.
- **Status**: resolved: the key source is the workload property
  `loadkeydistribution`.

### YCSB-7: `stdev` is printed in ns

The status lines (`-s`) print `stdev=` in ns, while the other latencies of the
line are in us.

- **Trigger**: every status line.
- **Experiment path**: yes.
- **Where**: `YCSB-cpp-fdp/core/measurements.cc`
  `HdrHistogramMeasurements::GetStatusMsg()`.
- **Status**: kept to preserve published behaviour.

### YCSB-8: Approximate zeta constant; other thetas sum it at startup

The scrambled zipfian generator uses 90.5 as the precomputed zeta constant for
10^10 + 1 items and theta 0.90 (exact value 90.5699), which slightly biases key
popularity. Any other `zipfian_const`, including the YCSB default 0.99, sums
10^10 terms at startup, which takes minutes before the first operation.

- **Trigger**: zipfian and latest request distributions; `zipfian_const` other
  than 0.90.
- **Experiment path**: yes (the constant); the recipes do not set
  `zipfian_const`.
- **Where**: `YCSB-cpp-fdp/core/scrambled_zipfian_generator.h`;
  `YCSB-cpp-fdp/core/zipfian_generator.h`.
- **Status**: kept to preserve published behaviour.

## Filebench

### SCR-1: `psrun $runtime` runs for 60 s

Filebench accepts only literal integers after `psrun`, so the last line of
`fileserver_f2fs.f` and `fileserver_xfs.f`, `psrun $runtime`, is parsed as a
bare `psrun`: statistics every 10 s and Filebench's default run of 60 s;
`$runtime=1200` is ignored.

- **Trigger**: running either fileserver personality as it is.
- **Experiment path**: `scripts/elasticity/fig17-filebench.sh` runs a copy whose
  last line is `psrun 10 4200` (`FILEBENCH_PSRUN`).
- **Where**: `filebench-fdp/fileserver_f2fs.f`, `filebench-fdp/fileserver_xfs.f`
  (last line).
- **Status**: kept to preserve published behaviour.

### SCR-2: `directio=1` in `fileserver_xfs.f` has no effect

Filebench sets O_DIRECT only when it opens a file, with the attributes of the
opening flowop (`createfile1`, which has no `directio`). On the write, append
and read flowops (`wrtfile1`, `appendfilerand1`, `readfile1`), `directio` only
aligns buffers, so the workload issues buffered I/O like `fileserver_f2fs.f`.
With `directio` on `createfile1`, the first write whose size is not a multiple
of the block size fails.

- **Trigger**: every run of `fileserver_xfs.f`.
- **Experiment path**: yes.
- **Where**: `filebench-fdp/fileserver_xfs.f`.
- **Status**: kept to preserve published behaviour.

## Blockchain

The Aptos and Solana changes are patches in `blockchain-fdp/`; the bindings are
in `scripts/blockchain/`.

### BC-2: Aptos latency percentiles miss most samples

Execution-latency samples are buffered per thread, and only the thread that
prints the results flushes its buffer; the BlockSTM worker threads, which
execute the user transactions, never hand their samples over. The printed
percentiles (`<prefix> pXX: N us`, the `pXX us` table columns and the
`pXX_latency_us` JSON fields) therefore describe the one block-epilogue
transaction per block that the pipeline's execution thread runs itself.

- **Trigger**: every Aptos benchmark.
- **Experiment path**: yes.
- **Where**:
  `blockchain-fdp/aptos/0001-executor-benchmark-time-based-mode-and-measurements.patch`:
  `aptos-move/block-executor/src/counters.rs` `get_all_latencies()`.
- **Status**: kept to preserve published behaviour.

### BC-3: Aptos checkpoints copy instead of hard-linking

In the CoFDP binding, `db/` and `cp/` are different mounts, so RocksDB copies
the SST files into the checkpoint (`link(2)` fails with EXDEV), while the
single-PID binding hard-links them.

- **Trigger**: every checkpoint of the Aptos DBs in the CoFDP binding.
- **Experiment path**: yes.
- **Where**: `scripts/blockchain/bind-aptos.sh` (layout `cofdp`).
- **Status**: kept to preserve published behaviour.

### BC-5: The Aptos time-based test always stops after 530 s

`single_node_performance.py` stops block generation of the time-based
`apt-fa-transfer` test after 530 s; the runner's `BENCHMARK_DURATION_SECS` only
switches the account sizing of the time-based mode.

- **Trigger**: every time-based test.
- **Experiment path**: yes.
- **Where**: `blockchain-fdp/aptos/0002-testsuite-cofdp-runner.patch`:
  `testsuite/single_node_performance.py`,
  `testsuite/performance_benchmark_cofdp.sh`.
- **Status**: kept to preserve published behaviour.

### BC-6: A time-limited raw-transfer run reports the requested block count

When a time limit stops a raw-transfer workload early, the benchmark still
counts the requested number of blocks, which misreports the transaction count
and TPS or makes the run panic.

- **Trigger**: a time-limited raw-transfer workload.
- **Experiment path**: no. The CoFDP tests run `apt-fa-transfer`.
- **Where**:
  `blockchain-fdp/aptos/0001-executor-benchmark-time-based-mode-and-measurements.patch`:
  `execution/executor-benchmark/src/transaction_generator.rs`.
- **Status**: not on the experiment path.

### BC-8: Solana accounts directories are one directory, reached through mount propagation

In the CoFDP binding, `accounts/`, `accounts_hash_cache/` and `accounts_index/`
are bind mounts of the same directory (`p2`), and the bind mounts reach the
validator's directories only through mount propagation (shared mounts, as on
systemd systems; the script checks it). Because the three directories are one,
the accounts-hash cache cleanup after every full accounts-hash calculation also
deletes the bucket files and the restart file of the disk index while they are
in use (they stay mapped, so the running validator is not affected); if the
validator crashes and is restarted, the index setup erases the shared
directory, account storage included.

- **Trigger**: every full accounts-hash calculation; a validator restart.
- **Experiment path**: yes (the cleanup); the recipes do not restart the
  validator.
- **Where**: `scripts/blockchain/bind-solana.sh` (layout `cofdp`).
- **Status**: kept to preserve published behaviour.

### BC-11: `HIDE_OUTPUT=0` still hides the benchmark output

`single_node_performance.py` treats any non-empty `HIDE_OUTPUT` as set, so the
runner's `HD=0` still hides the benchmark output.

- **Trigger**: every run of the runner.
- **Experiment path**: yes (console output only).
- **Where**: `blockchain-fdp/aptos/0002-testsuite-cofdp-runner.patch`:
  `testsuite/performance_benchmark_cofdp.sh`,
  `testsuite/single_node_performance.py`.
- **Status**: kept to preserve published behaviour.

### BC-13: A leftover Aptos DB is not detected

The executor benchmark skips the upstream check that the data directory does
not exist yet, because the bindings mount P-Directories into it before the run;
a DB left there by a previous run is not detected.

- **Trigger**: running without formatting the volume first.
- **Experiment path**: no. The recipes format the volume before every run.
- **Where**:
  `blockchain-fdp/aptos/0001-executor-benchmark-time-based-mode-and-measurements.patch`:
  `execution/executor-benchmark/src/db_generator.rs`.
- **Status**: not on the experiment path.

## Tools

No known issues.

## Scripts

### SCR-11: A failed fp-remap is still recorded

The elasticity drivers print a failed fp-remap call but still record it in
`events.csv`.

- **Trigger**: an fp-remap call that fails, e.g. with a wrong `--fp3` path.
- **Experiment path**: only when fp-remap fails.
- **Where**: `scripts/elasticity/run_experiment_cachelib.py`,
  `run_experiment_filebench.py`, `run_experiment_fio.py`
  `schedule_fp_remap()`.
- **Status**: kept to preserve published behaviour.

### SCR-12: `bench_tenant_sep_fs.py` reports all I/O as reads

fio's log suffix `.N` is the job index, not the I/O direction. Every tenant runs
one job, so all records are in the `*.1.log` files, which the driver reads as
reads: the per-second output and the PDFs show all bandwidth and IOPS as reads.
Latencies and totals are not affected.

- **Trigger**: every run.
- **Experiment path**: yes (`scripts/tenants/fig14.sh`).
- **Where**: `scripts/tenants/bench_tenant_sep_fs.py` `print_per_second()`.
- **Status**: kept to preserve published behaviour.

### SCR-M2: The Filebench driver ignores `--interval`

`run_experiment_filebench.py` always samples every 60 s (`SAMPLE_WINDOW`);
`--interval` is accepted and ignored.

- **Trigger**: `--interval`.
- **Experiment path**: yes (60 s samples).
- **Where**: `scripts/elasticity/run_experiment_filebench.py`.
- **Status**: kept to preserve published behaviour.
