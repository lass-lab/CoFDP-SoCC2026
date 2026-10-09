// SPDX-License-Identifier: GPL-2.0-only
/*
 * pidirfs-mount - mount the CoFDP F2FS (PiDir-FS) with N per-PID logs
 *
 * Usage: pidirfs-mount [--discard-unit=segment|block | --nodiscard]
 *                      [--dev DEVICE] [--dir DIRECTORY] <nr_pid_logs>
 *
 * Mounts DEVICE (default /dev/nvme0n1) on DIRECTORY (default
 * /home/femu/f2fs_fdp_mount) as f2fs, without mount flags and with the
 * options:
 *
 *	fdp_log_n=<nr_pid_logs>,discard,nobarrier,discard_unit=segment
 *
 * fdp_log_n is the number of per-PID logs of every log type (PISA). The log
 * index is also the PID written to the device, so use the number of PIDs of
 * the device: 8 with the ConfFDP preset in configs/femu/. The kernel accepts
 * 1 to 21. --discard-unit=block and --nodiscard select:
 *
 *	fdp_log_n=<nr_pid_logs>,discard,nobarrier,discard_unit=block
 *	fdp_log_n=<nr_pid_logs>,nodiscard,nobarrier
 *
 * Exit status: 0 if the file system was mounted, 1 otherwise (including
 * usage errors).
 */
#define _GNU_SOURCE		/* program_invocation_short_name */
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#define DEFAULT_DEV		"/dev/nvme0n1"
#define DEFAULT_DIR		"/home/femu/f2fs_fdp_mount"

/* F2FS_MAX_PID_LOGS of the CoFDP kernel (linux-fdp/fs/f2fs/f2fs.h) */
#define F2FS_MAX_PID_LOGS	21

enum discard_mode {
	DISCARD_SEGMENT,
	DISCARD_BLOCK,
	NODISCARD,
};

/* The mount options that follow fdp_log_n=N */
static const char *const discard_opts[] = {
	[DISCARD_SEGMENT]	= "discard,nobarrier,discard_unit=segment",
	[DISCARD_BLOCK]		= "discard,nobarrier,discard_unit=block",
	[NODISCARD]		= "nodiscard,nobarrier",
};

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s [--discard-unit=segment|block | --nodiscard]\n"
		"       [--dev DEVICE] [--dir DIRECTORY] NR_PID_LOGS\n"
		"Mount the CoFDP F2FS on DEVICE with NR_PID_LOGS (1-%d) per-PID logs:\n"
		"  mount -t f2fs -o fdp_log_n=NR_PID_LOGS,%s DEVICE DIRECTORY\n"
		"\n"
		"  --discard-unit=segment  discard in units of segments (default)\n"
		"  --discard-unit=block    discard in units of blocks\n"
		"  --nodiscard             mount with nodiscard\n"
		"  --dev DEVICE            device to mount (default " DEFAULT_DEV ")\n"
		"  --dir DIRECTORY         mount point (default " DEFAULT_DIR ")\n"
		"  -h, --help              print this help\n",
		program_invocation_short_name, F2FS_MAX_PID_LOGS,
		discard_opts[DISCARD_SEGMENT]);
}

/* NR_PID_LOGS is a decimal number from 1 to F2FS_MAX_PID_LOGS. */
static int parse_nr_pid_logs(const char *s, int *nr)
{
	unsigned long val;
	char *end;

	if (*s < '0' || *s > '9')
		return -1;
	errno = 0;
	val = strtoul(s, &end, 10);
	if (errno || *end || val < 1 || val > F2FS_MAX_PID_LOGS)
		return -1;
	*nr = (int)val;
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "discard-unit",	required_argument,	NULL, 'u' },
		{ "nodiscard",		no_argument,		NULL, 'n' },
		{ "dev",		required_argument,	NULL, 'd' },
		{ "dir",		required_argument,	NULL, 'm' },
		{ "help",		no_argument,		NULL, 'h' },
		{ NULL,			0,			NULL, 0 }
	};
	enum discard_mode mode = DISCARD_SEGMENT;
	bool discard_unit = false, nodiscard = false;
	const char *dev = DEFAULT_DEV;
	const char *dir = DEFAULT_DIR;
	char data[64];
	int nr_pid_logs, c;

	while ((c = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
		switch (c) {
		case 'u':
			if (!strcmp(optarg, "segment")) {
				mode = DISCARD_SEGMENT;
			} else if (!strcmp(optarg, "block")) {
				mode = DISCARD_BLOCK;
			} else {
				fprintf(stderr, "%s: invalid discard unit '%s'\n",
					program_invocation_short_name, optarg);
				usage(stderr);
				return 1;
			}
			discard_unit = true;
			break;
		case 'n':
			nodiscard = true;
			break;
		case 'd':
			dev = optarg;
			break;
		case 'm':
			dir = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:	/* getopt_long() printed the error */
			usage(stderr);
			return 1;
		}
	}

	if (discard_unit && nodiscard) {
		fprintf(stderr,
			"%s: --discard-unit and --nodiscard cannot be combined\n",
			program_invocation_short_name);
		usage(stderr);
		return 1;
	}
	if (nodiscard)
		mode = NODISCARD;

	if (optind != argc - 1) {
		usage(stderr);
		return 1;
	}
	if (parse_nr_pid_logs(argv[optind], &nr_pid_logs)) {
		fprintf(stderr,
			"%s: invalid number of per-PID logs '%s' (must be 1-%d)\n",
			program_invocation_short_name, argv[optind],
			F2FS_MAX_PID_LOGS);
		return 1;
	}

	snprintf(data, sizeof(data), "fdp_log_n=%d,%s", nr_pid_logs,
		 discard_opts[mode]);

	if (mount(dev, dir, "f2fs", 0, data)) {
		fprintf(stderr, "%s: cannot mount %s on %s with options %s: %s\n",
			program_invocation_short_name, dev, dir, data,
			strerror(errno));
		return 1;
	}

	printf("Successfully mounted %s on %s with options: %s\n",
	       dev, dir, data);
	return 0;
}
