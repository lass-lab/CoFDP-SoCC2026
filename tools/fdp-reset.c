// SPDX-License-Identifier: GPL-2.0-only
/*
 * fdp-reset - make FEMU print its counters and reset the emulated SSD
 *
 * Usage: fdp-reset <device>		e.g. sudo fdp-reset /dev/nvme0n1
 *
 * Sends I/O Management Send with the FEMU vendor management operation 0x2
 * (NVME_IOMS_MO_FEMU_RESET in ConfFDP/hw/femu/nvme.h) to the namespace of
 * <device>. In its FDP and multi-stream modes, FEMU then prints the counters
 * accumulated since the previous reset to its standard output (the log of
 * femu-scripts/run-fdp.sh): the pages written per PID and the pages copied
 * by garbage collection, from which the write amplification factor is
 * computed (see ConfFDP/README.md). It then resets its FTL to the state of
 * an empty SSD and clears the counters. The data in FEMU's memory backend
 * is not erased, but the FTL no longer knows that it is valid: run fdp-reset
 * before formatting the device for a run, and again after the measured
 * workload to get its counters.
 *
 * Exit status: 0 if the device completed the command successfully, 1 on any
 * failure.
 */
#define _GNU_SOURCE		/* program_invocation_short_name */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fdp-xnvme.h"

/*
 * The command carries MOS 1 and a zero-filled 528-byte data buffer (the size
 * of a Reclaim Unit Handle Status with 16 descriptors). FEMU reads neither.
 */
#define FEMU_RESET_MOS		0x1
#define FEMU_RESET_NBYTES	528

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s <device>\n"
		"Make FEMU print the counters of the emulated SSD of <device>\n"
		"(e.g. /dev/nvme0n1) to its log and reset the SSD to empty.\n",
		program_invocation_short_name);
}

int main(int argc, char **argv)
{
	struct fdp_dev fdev;
	void *buf;
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

	buf = fdp_buf_alloc(&fdev, FEMU_RESET_NBYTES);
	if (buf) {
		if (!fdp_io_mgmt_send(&fdev, NVME_IOMS_MO_FEMU_RESET,
				      FEMU_RESET_MOS, buf, FEMU_RESET_NBYTES))
			ret = 0;
		fdp_buf_free(&fdev, buf);
	}
	if (fdp_dev_close(&fdev))
		ret = 1;

	if (!ret)
		printf("%s: FEMU reset command completed\n", argv[1]);
	return ret;
}
