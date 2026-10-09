// SPDX-License-Identifier: GPL-2.0-only
/*
 * fdp-ruh-status - print the Reclaim Unit Handle Status of an FDP device
 *
 * Usage: fdp-ruh-status <device>	e.g. sudo fdp-ruh-status /dev/nvme0n1
 *
 * Reads the Reclaim Unit Handle Status (I/O Management Receive, management
 * operation 1) of the namespace of <device>, which lists the placement IDs
 * (PIDs) that writes can carry, and prints
 *
 *	nruhsd <number of descriptors>
 *	pid ruhid earutr ruamw
 *	<one line per descriptor, in the order of the device, in decimal>
 *
 * mkfs.f2fs -O lost_found reads the same data to create one Initial
 * P-Directory per PID. ConfFDP in FDP mode reports PIDs 0 to
 * rg_number * handle_number - 1 with RUHID == PID.
 *
 * Exit status: 0 on success, 1 on any failure.
 */
#define _GNU_SOURCE		/* program_invocation_short_name */
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "fdp-xnvme.h"

/* Reclaim Unit Handle Status Descriptor (little endian) */
struct ruh_status_desc {
	uint16_t pid;		/* Placement Identifier */
	uint16_t ruhid;		/* Reclaim Unit Handle Identifier */
	uint32_t earutr;	/* Estimated Active Reclaim Unit Time Remaining */
	uint64_t ruamw;		/* Reclaim Unit Available Media Writes */
	uint8_t rsvd16[16];
};

/*
 * The status is read into a buffer with room for 256 descriptors (8208
 * bytes, the same size mkfs.f2fs uses), which sets NUMD of the command;
 * descriptors beyond the first 256 are not transferred.
 */
#define RUH_STATUS_MAX_DESC	256

struct ruh_status {
	uint8_t rsvd0[14];
	uint16_t nruhsd;	/* Number of RUH Status Descriptors */
	struct ruh_status_desc desc[RUH_STATUS_MAX_DESC];
};

_Static_assert(sizeof(struct ruh_status_desc) == 32, "descriptor size");
_Static_assert(offsetof(struct ruh_status, desc) == 16, "header size");
_Static_assert(sizeof(struct ruh_status) == 8208, "buffer size");

/* MOS is reserved for this operation and sent as 1. */
#define RUH_STATUS_MOS		0x1

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s <device>\n"
		"Print the Reclaim Unit Handle Status (the placement IDs) of the\n"
		"FDP namespace <device> (e.g. /dev/nvme0n1).\n",
		program_invocation_short_name);
}

static void print_ruh_status(const char *uri, const struct ruh_status *rs)
{
	unsigned int nr = le16toh(rs->nruhsd);
	unsigned int i;

	printf("nruhsd %u\n", nr);
	if (nr > RUH_STATUS_MAX_DESC) {
		fprintf(stderr,
			"%s: %s: printing only the first %d of %u descriptors\n",
			program_invocation_short_name, uri,
			RUH_STATUS_MAX_DESC, nr);
		nr = RUH_STATUS_MAX_DESC;
	}

	printf("pid ruhid earutr ruamw\n");
	for (i = 0; i < nr; i++) {
		const struct ruh_status_desc *d = &rs->desc[i];

		printf("%u %u %" PRIu32 " %" PRIu64 "\n",
		       le16toh(d->pid), le16toh(d->ruhid),
		       le32toh(d->earutr), le64toh(d->ruamw));
	}
}

int main(int argc, char **argv)
{
	struct fdp_dev fdev;
	struct ruh_status *rs;
	int ret = 1;

	if (argc == 2 &&
	    (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(stdout);
		return 0;
	}
	if (argc != 2 || argv[1][0] == '-') {
		usage(stderr);
		return 1;
	}

	if (fdp_dev_open(&fdev, argv[1]))
		return 1;

	rs = fdp_buf_alloc(&fdev, sizeof(*rs));
	if (rs) {
		if (!fdp_io_mgmt_recv(&fdev, NVME_IOMR_MO_RUH_STATUS,
				      RUH_STATUS_MOS, rs, sizeof(*rs))) {
			print_ruh_status(argv[1], rs);
			ret = 0;
		}
		fdp_buf_free(&fdev, rs);
	}
	if (fdp_dev_close(&fdev))
		ret = 1;
	return ret;
}
