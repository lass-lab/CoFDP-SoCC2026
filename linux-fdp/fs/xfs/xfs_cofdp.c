// SPDX-License-Identifier: GPL-2.0
/*
 * CoFDP PiDir-FS support for XFS: PID selection, PISA bookkeeping, Initial
 * P-Directories and fp-remap.  See xfs_cofdp.h for an overview.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_ag.h"
#include "xfs_dir2.h"
#include "xfs_dir2_priv.h"
#include "xfs_icache.h"
#include "xfs_cofdp.h"

#include <linux/iomap.h>
#include <linux/mount.h>
#include <linux/namei.h>

/*
 * Resolve the PID of one allocation or bio from the non-zero PID bits of an
 * inode.  Below an Initial P-Directory the PID bits hold a single PID.  Below
 * a Flexible P-Directory one of the mapped PIDs is picked with
 * get_random_bytes(), which approximates round robin without shared state; if
 * no PID is mapped, PID 0 is used.
 */
unsigned int
xfs_cofdp_pick_pid(
	u32			pid_bits)
{
	u32			bitmap = cofdp_pid_bitmap(pid_bits);
	u32			r;

	if (!cofdp_pid_is_flexible(pid_bits))
		return __ffs(bitmap);
	if (!bitmap)
		return 0;

	/* Pick the r-th lowest mapped PID by clearing the r lowest set bits. */
	get_random_bytes(&r, sizeof(r));
	for (r %= hweight32(bitmap); r > 0; r--)
		bitmap &= bitmap - 1;
	return __ffs(bitmap);
}

/*
 * Tag a mapping of @ip with the PID for the bios built from it: an allocated
 * extent found by writeback, or a direct I/O mapping.  Mappings of extents
 * allocated at writeback carry the PID chosen by PISA instead, see
 * xfs_bmapi_convert_delalloc().
 */
void
xfs_cofdp_tag_iomap(
	struct xfs_inode	*ip,
	struct iomap		*iomap)
{
	if (xfs_cofdp_enabled(ip->i_mount) && ip->i_pid_bitmap)
		iomap->fdp_pid = xfs_cofdp_pick_pid(ip->i_pid_bitmap);
}

/*
 * Called just before an ioend is submitted.  An ioend that consists of its
 * inline bio only gets a freshly picked PID, so that the writeback of a file
 * in a Flexible P-Directory spreads over the mapped PIDs.  A larger ioend keeps
 * the PID of the mapping that started it for all of its bios:
 * iomap_chain_bio() has already submitted every bio but the last one, and the
 * last one inherited the same PID.  NOTE: see KNOWN_ISSUES (XFS-2, XFS-3).
 */
void
xfs_cofdp_tag_ioend(
	struct iomap_ioend	*ioend)
{
	struct xfs_inode	*ip = XFS_I(ioend->io_inode);
	u32			pid_bits = ip->i_pid_bitmap;

	if (!xfs_cofdp_enabled(ip->i_mount) || !cofdp_pid_bitmap(pid_bits))
		return;
	if (ioend->io_bio == &ioend->io_inline_bio)
		ioend->io_bio->fdp_pid = xfs_cofdp_pick_pid(pid_bits);
}

/*
 * PISA, the PID-aware space allocator, assigns an AG to each PID in
 * m_pisa_pid_ag so that the extents of one PID are grouped together and
 * different PIDs use different AGs.
 *
 * NOTE: the AG assignment is currently not consumed by the XFS allocator, see
 * xfs_bmap_btalloc_pisa(); data placement follows stock XFS locality.
 */

/*
 * Return the AG of @pid.  If that AG cannot hold an extent of @maxlen blocks,
 * move the PID to the first AG with enough free space that no other PID uses,
 * else to the AG with the most free space, else to the next AG.  An AG whose
 * AGF has not been read yet reports no free space and is left to the
 * allocator.
 */
xfs_agnumber_t
xfs_cofdp_pisa_select_ag(
	struct xfs_mount	*mp,
	unsigned int		pid,
	xfs_extlen_t		maxlen)
{
	xfs_agnumber_t		agno = mp->m_pisa_pid_ag[pid];
	xfs_agnumber_t		new_agno = NULLAGNUMBER;
	xfs_agnumber_t		best_agno = NULLAGNUMBER;
	xfs_extlen_t		best_free = 0;
	xfs_extlen_t		freeblks;
	struct xfs_perag	*pag;
	xfs_agnumber_t		a;

	pag = xfs_perag_get(mp, agno);
	freeblks = pag->pagf_freeblks;
	xfs_perag_put(pag);
	if (freeblks == 0 || freeblks >= maxlen)
		return agno;

	spin_lock(&mp->m_pisa_lock);
	for (a = 0; a < mp->m_sb.sb_agcount; a++) {
		bool		used = false;
		unsigned int	i;

		pag = xfs_perag_get(mp, a);
		freeblks = pag->pagf_freeblks;
		xfs_perag_put(pag);
		if (freeblks < maxlen)
			continue;

		if (freeblks > best_free) {
			best_free = freeblks;
			best_agno = a;
		}

		for (i = 0; i < mp->m_sb.sb_fdp_pids; i++) {
			if (i != pid && mp->m_pisa_pid_ag[i] == a) {
				used = true;
				break;
			}
		}
		if (!used) {
			new_agno = a;
			break;
		}
	}
	if (new_agno == NULLAGNUMBER)
		new_agno = best_agno;
	if (new_agno == NULLAGNUMBER)
		new_agno = (agno + 1) % mp->m_sb.sb_agcount;
	mp->m_pisa_pid_ag[pid] = new_agno;
	spin_unlock(&mp->m_pisa_lock);

	return new_agno;
}

/* Record that the allocator placed the extent of @pid in @new_agno. */
void
xfs_cofdp_pisa_update_ag(
	struct xfs_mount	*mp,
	unsigned int		pid,
	xfs_agnumber_t		agno,
	xfs_agnumber_t		new_agno)
{
	if (new_agno == agno)
		return;

	spin_lock(&mp->m_pisa_lock);
	mp->m_pisa_pid_ag[pid] = new_agno;
	spin_unlock(&mp->m_pisa_lock);
}

/*
 * Check the PID count of a PiDir-FS file system and set up the PISA state.
 * This runs before xfs_mountfs() so that allocations made during log recovery
 * already find initialised state.
 */
int
xfs_cofdp_mount(
	struct xfs_mount	*mp)
{
	unsigned int		pid;

	spin_lock_init(&mp->m_pisa_lock);
	if (!xfs_cofdp_enabled(mp))
		return 0;

	if (mp->m_sb.sb_fdp_pids > XFS_COFDP_MAX_PIDS) {
		xfs_warn(mp,
	"CoFDP: superblock asks for %u PIDs, PiDir-FS supports at most %u.",
			 mp->m_sb.sb_fdp_pids, XFS_COFDP_MAX_PIDS);
		return -EINVAL;
	}

	/* PISA starts out with PID i in AG i (modulo the AG count). */
	for (pid = 0; pid < ARRAY_SIZE(mp->m_pisa_pid_ag); pid++)
		mp->m_pisa_pid_ag[pid] = pid % mp->m_sb.sb_agcount;

	xfs_info(mp, "CoFDP: PiDir-FS with %u PIDs over %u AGs",
		 mp->m_sb.sb_fdp_pids, mp->m_sb.sb_agcount);
	return 0;
}

/*
 * Look up or create the Initial P-Directory @name in the root directory and
 * map it to @pid.
 */
static int
xfs_cofdp_init_pdir(
	struct xfs_mount	*mp,
	const char		*name,
	unsigned int		pid)
{
	struct dentry		*root = mp->m_super->s_root;
	struct inode		*dir = d_inode(root);
	struct dentry		*dentry;
	bool			created = false;
	int			error = 0;

	inode_lock(dir);
	dentry = lookup_one_len(name, root, strlen(name));
	if (IS_ERR(dentry)) {
		error = PTR_ERR(dentry);
		goto out_unlock;
	}

	if (d_is_negative(dentry)) {
		error = vfs_mkdir(&nop_mnt_idmap, dir, dentry, 0755);
		if (error)
			goto out_dput;
		created = true;
	} else if (!d_is_dir(dentry)) {
		error = -ENOTDIR;
		goto out_dput;
	}

	/*
	 * Map the directory to @pid.  An existing P-Directory is reset to @pid
	 * on every mount, even if it was remapped with fp-remap.
	 * NOTE: see KNOWN_ISSUES (XFS-14).
	 */
	if (d_is_positive(dentry)) {
		struct xfs_inode	*ip = XFS_I(d_inode(dentry));

		if (created || ip->i_pid_bitmap != (1U << pid)) {
			ip->i_pid_bitmap = 1U << pid;
			mark_inode_dirty(d_inode(dentry));
		}
	}

out_dput:
	dput(dentry);
out_unlock:
	inode_unlock(dir);
	if (created)
		xfs_notice(mp, "CoFDP: created Initial P-Directory %s (PID %u)",
			   name, pid);
	return error;
}

/*
 * Create the Initial P-Directories /p0 ... /p<N-1> when a PiDir-FS file system
 * is mounted read-write.  This is best effort: a P-Directory that cannot be
 * set up is reported and the mount goes on.
 */
void
xfs_cofdp_create_initial_pdirs(
	struct xfs_mount	*mp)
{
	unsigned int		pid;

	for (pid = 0; pid < mp->m_sb.sb_fdp_pids; pid++) {
		char		name[16];	/* "p" and a PID */
		int		error;

		snprintf(name, sizeof(name), "p%u", pid);
		error = xfs_cofdp_init_pdir(mp, name, pid);
		if (error)
			xfs_warn(mp,
		"CoFDP: cannot set up Initial P-Directory %s, error %d",
				 name, error);
	}
}

/*
 * fp-remap: XFS_IOC_COFDP_REMAP maps a directory and everything below it to
 * the PIDs in a bitmap, which makes the directory a Flexible P-Directory.
 *
 * The walk recurses once per directory level.  It changes the PID bits in
 * memory only: mark_inode_dirty() does not log an XFS inode, so the new PID
 * bits reach the PinodeBlock only once a later transaction logs the inode.
 * NOTE: see KNOWN_ISSUES (XFS-5, XFS-7, XFS-16).
 */
struct xfs_cofdp_remap_ctx {
	struct dir_context	ctx;
	struct xfs_mount	*mp;
	u32			pid_bits;
	int			error;		/* first error seen */
};

static int xfs_cofdp_remap_dir(struct xfs_mount *mp, struct xfs_inode *dp,
		u32 pid_bits);

static bool
xfs_cofdp_remap_actor(
	struct dir_context	*ctx,
	const char		*name,
	int			namlen,
	loff_t			offset,
	u64			ino,
	unsigned int		d_type)
{
	struct xfs_cofdp_remap_ctx *rctx =
		container_of(ctx, struct xfs_cofdp_remap_ctx, ctx);
	struct xfs_inode	*ip;
	int			error;

	/* Skip "." and "..". */
	if (name[0] == '.' && (namlen == 1 || (namlen == 2 && name[1] == '.')))
		return true;

	error = xfs_iget(rctx->mp, NULL, ino, 0, 0, &ip);
	if (!error) {
		ip->i_pid_bitmap = rctx->pid_bits;
		mark_inode_dirty(VFS_I(ip));

		if (d_type == DT_DIR)
			error = xfs_cofdp_remap_dir(rctx->mp, ip,
					rctx->pid_bits);
		xfs_irele(ip);
	}

	/* Remember the first error but remap as much of the tree as we can. */
	if (error && !rctx->error)
		rctx->error = error;
	return true;
}

static int
xfs_cofdp_remap_dir(
	struct xfs_mount	*mp,
	struct xfs_inode	*dp,
	u32			pid_bits)
{
	struct xfs_cofdp_remap_ctx rctx = {
		.ctx.actor	= xfs_cofdp_remap_actor,
		.ctx.pos	= 0,
		.mp		= mp,
		.pid_bits	= pid_bits,
	};
	int			error;

	xfs_ilock(dp, XFS_IOLOCK_SHARED);
	error = xfs_readdir(NULL, dp, &rctx.ctx, 0);
	xfs_iunlock(dp, XFS_IOLOCK_SHARED);

	return rctx.error ? rctx.error : error;
}

int
xfs_ioc_cofdp_remap(
	struct file		*filp,
	void __user		*arg)
{
	struct inode		*inode = file_inode(filp);
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_cofdp_remap_arg remap;
	unsigned int		nr_pids;
	u32			pid_bits;
	int			error;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;
	if (!xfs_cofdp_enabled(mp))
		return -EOPNOTSUPP;
	if (copy_from_user(&remap, arg, sizeof(remap)))
		return -EFAULT;
	if (remap.flags != 0)
		return -EINVAL;

	/* Accept a non-empty set of the PIDs the file system was made with. */
	nr_pids = min_t(unsigned int, mp->m_sb.sb_fdp_pids, XFS_COFDP_MAX_PIDS);
	if (!remap.new_pid_bitmap || remap.new_pid_bitmap >= (1U << nr_pids))
		return -EINVAL;

	error = mnt_want_write_file(filp);
	if (error)
		return error;

	pid_bits = COFDP_PID_FLEXIBLE | remap.new_pid_bitmap;
	ip->i_pid_bitmap = pid_bits;
	mark_inode_dirty(inode);
	error = xfs_cofdp_remap_dir(mp, ip, pid_bits);

	mnt_drop_write_file(filp);
	return error;
}
