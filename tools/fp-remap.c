// SPDX-License-Identifier: GPL-2.0-only
/*
 * fp-remap - map a directory tree to a new set of placement IDs (CoFDP)
 *
 * Usage: fp-remap <directory_path> <pid> [pid ...]
 *
 * Makes <directory_path> a Flexible P-Directory: the kernel maps the
 * directory and everything below it to the given PIDs, and later writes of
 * the files in it are spread pseudo-randomly over these PIDs. The file
 * system type is detected with fstatfs():
 *   F2FS (PiDir-FS, mkfs.f2fs -O lost_found)  F2FS_IOC_FP_REMAP
 *   XFS (mkfs.xfs -d fdp_pids=N)               XFS_IOC_COFDP_REMAP
 * Both need CAP_SYS_ADMIN. XFS rejects PIDs at or above the fdp_pids the
 * file system was made with; F2FS uses PID p as p % fdp_log_n.
 *
 * Each <pid> is a decimal PID from 0 to 30; together they form the bitmap
 * passed to the kernel (bit n = PID n).
 *
 * Exit status: 0 on success, 1 on any failure.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <linux/types.h>

/* PID bits (linux-fdp/include/linux/cofdp.h): bits 0..30 are PIDs */
#define COFDP_PID_FLEXIBLE	(1U << 31)
#define COFDP_MAX_PID		30

/* F2FS: linux-fdp/include/uapi/linux/f2fs.h */
#ifndef F2FS_SUPER_MAGIC
#define F2FS_SUPER_MAGIC	0xF2F52010
#endif
#define F2FS_IOCTL_MAGIC	0xf5
#define F2FS_IOC_FP_REMAP	_IOW(F2FS_IOCTL_MAGIC, 16, __u32)

/* XFS: linux-fdp/fs/xfs/libxfs/xfs_fs.h */
#ifndef XFS_SUPER_MAGIC
#define XFS_SUPER_MAGIC		0x58465342
#endif
struct xfs_cofdp_remap_arg {
	__u32	new_pid_bitmap;		/* PIDs to map, at least one */
	__u32	flags;			/* reserved, must be 0 */
};
#define XFS_IOC_COFDP_REMAP	_IOW('X', 62, struct xfs_cofdp_remap_arg)

/* The ioctl numbers are kernel ABI. */
_Static_assert(F2FS_IOC_FP_REMAP == 0x4004f510, "F2FS_IOC_FP_REMAP");
_Static_assert(XFS_IOC_COFDP_REMAP == 0x4008583e, "XFS_IOC_COFDP_REMAP");

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <directory_path> <pid> [pid ...]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "  directory_path  Path to a directory on F2FS or XFS (CoFDP)\n");
	fprintf(stderr, "  pid             One or more PID bit positions (0-30)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Example:\n");
	fprintf(stderr, "  %s /mnt/mydir 2 3 5\n", prog);
	fprintf(stderr, "  -> bitmap = (1<<2)|(1<<3)|(1<<5) = 0x0000002c\n");
}

/* A PID is a decimal number from 0 to COFDP_MAX_PID, without sign or blanks. */
static int parse_pid(const char *s, unsigned int *pid)
{
	unsigned long val;
	char *end;

	if (*s < '0' || *s > '9')
		return -1;
	errno = 0;
	val = strtoul(s, &end, 10);
	if (errno || *end || val > COFDP_MAX_PID)
		return -1;
	*pid = (unsigned int)val;
	return 0;
}

int main(int argc, char *argv[])
{
	int fd, i, ret = 0;
	__u32 bitmap = 0;
	unsigned int pid;
	struct statfs st;

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	for (i = 2; i < argc; i++) {
		if (parse_pid(argv[i], &pid)) {
			fprintf(stderr, "Error: invalid pid '%s' (must be 0-30)\n",
				argv[i]);
			return 1;
		}
		bitmap |= 1U << pid;
	}

	printf("PID bitmap: 0x%08x (bits:", bitmap);
	for (i = 0; i <= COFDP_MAX_PID; i++)
		if (bitmap & (1U << i))
			printf(" %d", i);
	printf(")\n");

	fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "Error: cannot open '%s': %s\n",
			argv[1], strerror(errno));
		return 1;
	}

	if (fstatfs(fd, &st) < 0) {
		fprintf(stderr, "Error: fstatfs failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	switch ((unsigned long)st.f_type) {
	case F2FS_SUPER_MAGIC:
		printf("Detected: F2FS\n");
		if (ioctl(fd, F2FS_IOC_FP_REMAP, &bitmap) < 0) {
			fprintf(stderr, "Error: F2FS ioctl failed: %s\n",
				strerror(errno));
			ret = 1;
		}
		break;

	case XFS_SUPER_MAGIC: {
		struct xfs_cofdp_remap_arg arg = {
			.new_pid_bitmap = bitmap,
			.flags = 0,
		};

		printf("Detected: XFS\n");
		if (ioctl(fd, XFS_IOC_COFDP_REMAP, &arg) < 0) {
			fprintf(stderr, "Error: XFS ioctl failed: %s\n",
				strerror(errno));
			ret = 1;
		}
		break;
	}

	default:
		fprintf(stderr, "Error: unsupported filesystem (magic=0x%lx)\n",
			(unsigned long)st.f_type);
		ret = 1;
		break;
	}

	if (!ret)
		printf("Remapped '%s' -> 0x%08x\n", argv[1],
		       bitmap | COFDP_PID_FLEXIBLE);

	close(fd);
	return ret;
}
