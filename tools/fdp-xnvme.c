// SPDX-License-Identifier: GPL-2.0-only
/*
 * fdp-xnvme.c - submit one NVMe I/O Management command through xNVMe
 *
 * The device is opened with default xNVMe options, the thread-pool
 * asynchronous interface ("thrpool") and without O_DIRECT. The command is
 * submitted on a queue of depth one, which is then drained to wait for its
 * completion.
 */
#define _GNU_SOURCE		/* program_invocation_short_name */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libxnvme.h>

#include "fdp-xnvme.h"

/* "<program>: <device>: <message>[: <strerror(err)>]" */
__attribute__((format(printf, 3, 4)))
static void fdp_err(const struct fdp_dev *fdev, int err, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: %s: ", program_invocation_short_name, fdev->uri);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (err)
		fprintf(stderr, ": %s", strerror(err));
	fputc('\n', stderr);
}

int fdp_dev_open(struct fdp_dev *fdev, const char *uri)
{
	struct xnvme_opts opts = xnvme_opts_default();
	int err;

	opts.async = "thrpool";
	opts.direct = 0;

	fdev->uri = uri;
	fdev->queue = NULL;
	fdev->dev = xnvme_dev_open(uri, &opts);
	if (!fdev->dev) {
		fdp_err(fdev, errno ? errno : ENODEV,
			"cannot open the device with xNVMe");
		return -1;
	}

	err = xnvme_queue_init(fdev->dev, 1, 0, &fdev->queue);
	if (err) {
		fdp_err(fdev, -err, "cannot create an xNVMe queue");
		xnvme_dev_close(fdev->dev);
		return -1;
	}

	fdev->nsid = xnvme_dev_get_nsid(fdev->dev);
	return 0;
}

int fdp_dev_close(struct fdp_dev *fdev)
{
	int err = xnvme_queue_term(fdev->queue);

	if (err)
		fdp_err(fdev, -err, "cannot tear down the xNVMe queue");
	xnvme_dev_close(fdev->dev);
	return err ? -1 : 0;
}

void *fdp_buf_alloc(struct fdp_dev *fdev, size_t nbytes)
{
	void *buf = xnvme_buf_alloc(fdev->dev, nbytes);

	if (!buf) {
		fdp_err(fdev, ENOMEM, "cannot allocate a %zu-byte I/O buffer",
			nbytes);
		return NULL;
	}
	memset(buf, 0, nbytes);
	return buf;
}

void fdp_buf_free(struct fdp_dev *fdev, void *buf)
{
	xnvme_buf_free(fdev->dev, buf);
}

/* Completion of the command, filled in by fdp_cmd_done() */
struct fdp_cpl {
	struct xnvme_queue *queue;
	bool done;
	bool failed;
	unsigned int sct;	/* status code type */
	unsigned int sc;	/* status code */
};

/*
 * Completion callback, called from xnvme_queue_drain(). A command that the
 * backend could not pass to the device also completes here, with an error
 * status.
 */
static void fdp_cmd_done(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct fdp_cpl *cpl = cb_arg;

	cpl->done = true;
	cpl->failed = xnvme_cmd_ctx_cpl_status(ctx);
	cpl->sct = ctx->cpl.status.sct;
	cpl->sc = ctx->cpl.status.sc;
	xnvme_queue_put_cmd_ctx(cpl->queue, ctx);
}

static int fdp_io_mgmt(struct fdp_dev *fdev, bool send, uint8_t mo,
		       uint16_t mos, void *buf, uint32_t nbytes)
{
	const char *cmd = send ? "I/O Management Send" :
				 "I/O Management Receive";
	struct fdp_cpl cpl = { .queue = fdev->queue };
	struct xnvme_cmd_ctx *ctx;
	int err;

	ctx = xnvme_queue_get_cmd_ctx(fdev->queue);
	if (!ctx) {
		fdp_err(fdev, ENOMEM, "no free xNVMe command context");
		return -1;
	}
	ctx->async.cb = fdp_cmd_done;
	ctx->async.cb_arg = &cpl;

	if (send)
		err = xnvme_nvm_mgmt_send(ctx, fdev->nsid, mo, mos, buf, nbytes);
	else
		err = xnvme_nvm_mgmt_recv(ctx, fdev->nsid, mo, mos, buf, nbytes);
	if (err) {
		xnvme_queue_put_cmd_ctx(fdev->queue, ctx);
		fdp_err(fdev, -err, "cannot submit %s (MO 0x%x)", cmd, mo);
		return -1;
	}

	err = xnvme_queue_drain(fdev->queue);
	if (err < 0 || !cpl.done) {
		fdp_err(fdev, err < 0 ? -err : 0,
			"%s (MO 0x%x) did not complete", cmd, mo);
		return -1;
	}
	if (cpl.failed) {
		fdp_err(fdev, 0,
			"%s (MO 0x%x) failed: status code type 0x%x, status code 0x%x",
			cmd, mo, cpl.sct, cpl.sc);
		return -1;
	}
	return 0;
}

int fdp_io_mgmt_send(struct fdp_dev *fdev, uint8_t mo, uint16_t mos,
		     void *buf, uint32_t nbytes)
{
	return fdp_io_mgmt(fdev, true, mo, mos, buf, nbytes);
}

int fdp_io_mgmt_recv(struct fdp_dev *fdev, uint8_t mo, uint16_t mos,
		     void *buf, uint32_t nbytes)
{
	return fdp_io_mgmt(fdev, false, mo, mos, buf, nbytes);
}
