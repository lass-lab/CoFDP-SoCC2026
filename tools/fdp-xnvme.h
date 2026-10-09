/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fdp-xnvme.h - submit one NVMe I/O Management command through xNVMe
 *
 * Shared by fdp-reset and fdp-ruh-status; only fdp-xnvme.c uses xNVMe.
 */
#ifndef FDP_XNVME_H
#define FDP_XNVME_H

#include <stddef.h>
#include <stdint.h>

/* I/O Management Receive (opcode 12h): Reclaim Unit Handle Status */
#define NVME_IOMR_MO_RUH_STATUS		0x1

/*
 * I/O Management Send (opcode 1Dh): FEMU vendor extension that prints the
 * FEMU counters and resets the emulated SSD (NVME_IOMS_MO_FEMU_RESET in
 * ConfFDP/hw/femu/nvme.h).
 */
#define NVME_IOMS_MO_FEMU_RESET		0x2

struct xnvme_dev;
struct xnvme_queue;

struct fdp_dev {
	const char *uri;		/* device path, for messages */
	struct xnvme_dev *dev;
	struct xnvme_queue *queue;
	uint32_t nsid;			/* namespace of the device */
};

/*
 * On failure, these functions print a message to stderr and return -1
 * (NULL for fdp_buf_alloc()); they return 0 on success.
 */
int fdp_dev_open(struct fdp_dev *fdev, const char *uri);
int fdp_dev_close(struct fdp_dev *fdev);

/* A zero-filled I/O buffer of @nbytes bytes */
void *fdp_buf_alloc(struct fdp_dev *fdev, size_t nbytes);
void fdp_buf_free(struct fdp_dev *fdev, void *buf);

/*
 * Submit an I/O Management Send/Receive with management operation @mo and
 * management operation specific field @mos to the namespace of @fdev,
 * transferring @nbytes bytes from/to @buf, and wait for its completion.
 * Fails if the command cannot be submitted or completes with an error.
 */
int fdp_io_mgmt_send(struct fdp_dev *fdev, uint8_t mo, uint16_t mos,
		     void *buf, uint32_t nbytes);
int fdp_io_mgmt_recv(struct fdp_dev *fdev, uint8_t mo, uint16_t mos,
		     void *buf, uint32_t nbytes);

#endif /* FDP_XNVME_H */
