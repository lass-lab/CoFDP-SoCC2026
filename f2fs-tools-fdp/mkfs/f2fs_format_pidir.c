/**
 * f2fs_format_pidir.c
 *
 * CoFDP PiDir-FS support for mkfs.f2fs: access to the FDP device through
 * xNVMe NVMe passthrough.
 *  - f2fs_pidir_query_pids() reads the placement identifiers (PIDs) from the
 *    Reclaim Unit Handle Status; mkfs creates one Initial P-Directory per PID.
 *  - f2fs_pidir_write_block() writes one block with a PID attached; it is used
 *    for the inode and dentry blocks of the Initial P-Directories.
 *
 * Without libxnvme (configure --without-xnvme) both fail, so mkfs.f2fs
 * rejects -O lost_found.
 *
 * Dual licensed under the GPL or LGPL version 2 licenses.
 */
#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <string.h>

#include "config.h"
#ifdef HAVE_LIBXNVME
#include <libxnvme.h>
#endif

#include "f2fs_fs.h"
#include "f2fs_format_utils.h"

#ifdef HAVE_LIBXNVME

/* I/O Management Receive, management operation: Reclaim Unit Handle Status */
#define PIDIR_IOMR_MO_RUH_STATUS	0x1
/* Management Operation Specific: reserved for this operation, sent as 1 */
#define PIDIR_IOMR_MOS			0x1
/* directive type of a write that carries its PID in DSPEC */
#define PIDIR_DTYPE_DATA_PLACEMENT	0x2

/* Reclaim Unit Handle Status Descriptor */
struct pidir_ruhs_desc {
	__le16 pid;		/* Placement Identifier */
	__le16 ruhid;		/* Reclaim Unit Handle Identifier */
	__le32 earutr;		/* Estimated Active RU Time Remaining */
	__le64 ruamw;		/* Reclaim Unit Available Media Writes */
	__u8 rsvd16[16];
};

/*
 * Reclaim Unit Handle Status, read with room for 256 descriptors. The size of
 * this buffer (8208 bytes) sets NUMD of the command, so it must not change.
 */
#define PIDIR_RUHS_NR_DESC	256

struct pidir_ruhs {
	__u8 rsvd0[14];
	__le16 nruhsd;		/* Number of RUH Status Descriptors */
	struct pidir_ruhs_desc desc[PIDIR_RUHS_NR_DESC];
};

static_assert(sizeof(struct pidir_ruhs_desc) == 32, "");
static_assert(offsetof(struct pidir_ruhs, nruhsd) == 14, "");
static_assert(offsetof(struct pidir_ruhs, desc) == 16, "");
static_assert(sizeof(struct pidir_ruhs) == 8208, "");

/*
 * Every command opens the device with a single-entry queue, passes the
 * command through, waits for it and releases the device again.
 */
struct pidir_xnvme {
	struct xnvme_dev *dev;
	struct xnvme_queue *queue;
	struct xnvme_cmd_ctx *ctx;
	uint32_t nsid;
	int err;		/* completion status, set by pidir_xnvme_cb() */
};

static void pidir_xnvme_cb(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct pidir_xnvme *x = cb_arg;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		xnvme_cmd_ctx_pr(ctx, XNVME_PR_DEF);
		x->err = -EIO;
	}
	xnvme_queue_put_cmd_ctx(x->queue, ctx);
}

static void pidir_xnvme_close(struct pidir_xnvme *x)
{
	xnvme_queue_term(x->queue);
	xnvme_dev_close(x->dev);
}

static int pidir_xnvme_open(struct pidir_xnvme *x)
{
	struct xnvme_opts opts = xnvme_opts_default();
	int err;

	x->dev = xnvme_dev_open(c.devices[0].path, &opts);
	if (!x->dev)
		return -ENODEV;

	err = xnvme_queue_init(x->dev, 1, 0, &x->queue);
	if (err) {
		xnvme_dev_close(x->dev);
		return err;
	}

	x->ctx = xnvme_queue_get_cmd_ctx(x->queue);
	if (!x->ctx) {
		pidir_xnvme_close(x);
		return -ENOMEM;
	}
	x->ctx->async.cb = pidir_xnvme_cb;
	x->ctx->async.cb_arg = x;
	x->ctx->dev = x->dev;
	x->nsid = xnvme_dev_get_nsid(x->dev);
	x->err = 0;
	return 0;
}

/* Wait for the command whose submission returned @err, then close @x. */
static int pidir_xnvme_complete(struct pidir_xnvme *x, int err)
{
	if (!err) {
		err = xnvme_queue_drain(x->queue);
		if (err >= 0)
			err = x->err;
	}
	pidir_xnvme_close(x);
	return err;
}

/*
 * Store the PIDs of the device in @pids and return their number (1 to @max).
 * Return -1 with an error message if xNVMe cannot open the device, if the
 * Reclaim Unit Handle Status command fails or if the device reports no PID or
 * more than @max PIDs.
 */
int f2fs_pidir_query_pids(uint16_t *pids, unsigned int max)
{
	struct pidir_ruhs ruhs;
	struct pidir_xnvme x;
	unsigned int i, nr;
	int err;

	memset(&ruhs, 0, sizeof(ruhs));

	err = pidir_xnvme_open(&x);
	if (err) {
		MSG(0, "\tError: PiDir-FS (-O lost_found): xNVMe cannot open "
			"%s (%d)\n", c.devices[0].path, err);
		return -1;
	}

	err = xnvme_nvm_mgmt_recv(x.ctx, x.nsid, PIDIR_IOMR_MO_RUH_STATUS,
				PIDIR_IOMR_MOS, &ruhs, sizeof(ruhs));
	err = pidir_xnvme_complete(&x, err);
	if (err) {
		MSG(0, "\tError: PiDir-FS (-O lost_found): cannot read the FDP "
			"reclaim unit handle status of %s (%d)\n",
			c.devices[0].path, err);
		return -1;
	}

	nr = le16_to_cpu(ruhs.nruhsd);
	if (!nr || nr > max) {
		MSG(0, "\tError: PiDir-FS (-O lost_found): %s reports %u "
			"placement identifiers, mkfs.f2fs supports 1 to %u\n",
			c.devices[0].path, nr, max);
		return -1;
	}

	for (i = 0; i < nr; i++)
		pids[i] = le16_to_cpu(ruhs.desc[i].pid);
	return nr;
}

/* Write one F2FS block at @blkaddr with @pid attached. */
int f2fs_pidir_write_block(void *buf, uint64_t blkaddr, uint16_t pid)
{
	struct pidir_xnvme x;
	int err;

	err = pidir_xnvme_open(&x);
	if (!err) {
		/*
		 * SLBA is the F2FS block address and NLB is 0: this assumes a
		 * 4 KiB LBA format and a file system that starts at LBA 0 of a
		 * single namespace. NOTE: see KNOWN_ISSUES (MKFS-6).
		 */
		xnvme_prep_nvm(x.ctx, XNVME_SPEC_NVM_OPC_WRITE, x.nsid,
				blkaddr, 0);
		x.ctx->cmd.nvm.dtype = PIDIR_DTYPE_DATA_PLACEMENT;
		x.ctx->cmd.nvm.cdw13.dspec = pid;
		err = xnvme_cmd_pass(x.ctx, buf, F2FS_BLKSIZE, NULL, 0);
		err = pidir_xnvme_complete(&x, err);
	}
	if (err) {
		MSG(0, "\tError: PID-tagged write of block 0x%08"PRIx64
			" (PID %u) failed (%d)\n", blkaddr, pid, err);
		return -1;
	}
	return 0;
}

#else /* !HAVE_LIBXNVME */

int f2fs_pidir_query_pids(uint16_t *UNUSED(pids), unsigned int UNUSED(max))
{
	MSG(0, "\tError: PiDir-FS (-O lost_found) is not available: "
		"mkfs.f2fs was built without libxnvme (--without-xnvme)\n");
	return -1;
}

/* not reached: mkfs aborts when the PIDs cannot be read */
int f2fs_pidir_write_block(void *UNUSED(buf), uint64_t UNUSED(blkaddr),
				uint16_t UNUSED(pid))
{
	return -1;
}

#endif /* HAVE_LIBXNVME */
