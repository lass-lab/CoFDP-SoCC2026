/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CoFDP PiDir-FS support for XFS.
 *
 * A file system made with "mkfs.xfs -d fdp_pids=N" (sb_fdp_pids = N) tags its
 * data writes with FDP placement IDs (PIDs):
 *
 *  - Every inode carries PID bits (see <linux/cofdp.h>) in i_pid_bitmap.  They
 *    are persisted in the first four bytes of di_pad2 (PinodeBlock) and are
 *    inherited from the parent directory when an inode is created.
 *  - The Initial P-Directories /p0 ... /p<N-1> are created at mount time and
 *    map to PIDs 0 ... N-1.
 *  - XFS_IOC_COFDP_REMAP (fp-remap) maps a directory tree to a set of PIDs and
 *    turns it into a Flexible P-Directory.
 *  - The PID is resolved when blocks are allocated (PISA), when writeback maps
 *    already allocated blocks or submits a single-bio ioend, and for direct
 *    I/O.  It reaches the NVMe driver through iomap->fdp_pid and
 *    bio->fdp_pid.  Metadata buffers and the log are written with PID 0;
 *    log recovery writes set no PID, see KNOWN_ISSUES (BLK-1).
 */
#ifndef __XFS_COFDP_H__
#define __XFS_COFDP_H__

#include <linux/cofdp.h>

struct file;
struct iomap;
struct iomap_ioend;
struct xfs_inode;
struct xfs_mount;

/* Bit 31 of the PID bits is COFDP_PID_FLEXIBLE, leaving room for 31 PIDs. */
#define XFS_COFDP_MAX_PIDS	31

/* PiDir-FS is enabled when mkfs.xfs stored a PID count in the superblock. */
static inline bool
xfs_cofdp_enabled(
	struct xfs_mount	*mp)
{
	return mp->m_sb.sb_fdp_pids > 0;
}

unsigned int xfs_cofdp_pick_pid(u32 pid_bits);
void xfs_cofdp_tag_iomap(struct xfs_inode *ip, struct iomap *iomap);
void xfs_cofdp_tag_ioend(struct iomap_ioend *ioend);

xfs_agnumber_t xfs_cofdp_pisa_select_ag(struct xfs_mount *mp,
		unsigned int pid, xfs_extlen_t maxlen);
void xfs_cofdp_pisa_update_ag(struct xfs_mount *mp, unsigned int pid,
		xfs_agnumber_t agno, xfs_agnumber_t new_agno);

int xfs_cofdp_mount(struct xfs_mount *mp);
void xfs_cofdp_create_initial_pdirs(struct xfs_mount *mp);
int xfs_ioc_cofdp_remap(struct file *filp, void __user *arg);

#endif /* __XFS_COFDP_H__ */
