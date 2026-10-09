#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# setup-fs.sh <fs>: reset the FDP SSD and create and mount a fresh file
# system on it, as every recipe does before its workload (for interactive
# sessions). See usage() for the file systems.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <fs>
  pidirfs-f2fs    PiDir-FS on F2FS: mkfs.f2fs -O lost_found creates the Initial
                  P-Directories p0 ... p<N-1>; mounted with fdp_log_n=\$COFDP_NR_PIDS
  kernelfdp-f2fs  F2FS without PiDir-FS (KernelFDP-F2FS), fdp_log_n=1
  pidirfs-xfs     PiDir-FS on XFS: mkfs.xfs -d fdp_pids=\$COFDP_NR_PIDS, mounted
                  with -o \$XFS_MOUNT_OPTS (default: discard)
The file system is mounted on \$COFDP_MNT ($COFDP_MNT).
EOF
	exit 1
}

[ $# -eq 1 ] || usage
cofdp_require_root
case $1 in
pidirfs-f2fs) cofdp_setup_pidirfs_f2fs ;;
kernelfdp-f2fs) cofdp_setup_kernelfdp_f2fs ;;
pidirfs-xfs) cofdp_setup_pidirfs_xfs "${XFS_MOUNT_OPTS:-discard}" ;;
*) usage ;;
esac
cofdp_log "$1 is mounted on $COFDP_MNT"
