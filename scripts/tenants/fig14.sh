#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# fig14.sh <fs>: three fio tenants on a file system (Fig. 14).
#
# Resets the FDP SSD and creates PiDir-FS (fs = f2fs or xfs), creates the
# P-Directories fp1, fp2 and fp3 and sets their PIDs to $FIG14_PIDS (default
# "3 5 7") with fp-remap, then runs bench_tenant_sep_fs.py with
# tenant_desc_fs: staggered random 4 MiB I/O (direct I/O, io_uring) of tenant
# A (5 % reads) on fp1, B (60 % reads) on fp2 and C (30 % reads) on fp3.
#
#   f2fs  mkfs.f2fs -O lost_found, mounted with fdp_log_n=$COFDP_NR_PIDS
#   xfs   mkfs.xfs -d fdp_pids=$COFDP_NR_PIDS, mounted with
#         -o $XFS_MOUNT_OPTS (default: discard,allocsize=64k)
#
# The rows of Fig. 14 differ in the device only:
#   FEMU, 4 RGs     the default preset (configs/femu/): PIDs 3, 5 and 7 are
#                   in RGs 1, 2 and 3, one RG per tenant
#   FEMU, 1 RG      FEMU started with configs/femu/cofdp-1rg.env: the three
#                   PIDs share one RG
#   real FDP SSD    COFDP_DEVICE=nvme-format COFDP_DEV=<namespace>
#                   COFDP_NR_PIDS=<PIDs of the device> FIG14_PIDS="1 2 3"
#
# With --plot-only, only the PDFs of an earlier run are drawn again:
#   fig14.sh --plot-only <log dir>
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <f2fs|xfs>
       $0 --plot-only <log dir>
EOF
	exit 1
}

driver=$here/bench_tenant_sep_fs.py
desc=$here/tenant_desc_fs

[ $# -ge 1 ] || usage
if [ "$1" = --plot-only ]; then
	[ $# -eq 2 ] || usage
	exec "$PYTHON" "$driver" --plot-only --desc "$desc" --log-dir "$2"
fi
[ $# -eq 1 ] || usage
fs=$1
case $fs in
f2fs | xfs) ;;
*) usage ;;
esac
read -r -a pids <<<"${FIG14_PIDS:-3 5 7}"
[ ${#pids[@]} -eq 3 ] || cofdp_die "FIG14_PIDS must name three PIDs"
name=$(cofdp_result_name fig14 "$fs")

cofdp_require_root
cofdp_require fio "$PYTHON"
cofdp_aslr_off
if [ "$fs" = f2fs ]; then
	cofdp_setup_pidirfs_f2fs
else
	cofdp_setup_pidirfs_xfs "${XFS_MOUNT_OPTS:-discard,allocsize=64k}"
fi
for n in 1 2 3; do
	cofdp_run mkdir "$COFDP_MNT/fp$n"
done
for n in 1 2 3; do
	cofdp_fp_remap "$COFDP_MNT/fp$n" "${pids[n - 1]}"
done

# The tenant description names /home/femu/f2fs_fdp_mount literally
if [ "$COFDP_MNT" != /home/femu/f2fs_fdp_mount ]; then
	cofdp_edited_copy "$desc" "$RESULT_DIR/$name.desc" \
		-e "s#/home/femu/f2fs_fdp_mount#$COFDP_MNT#g"
	desc=$RESULT_DIR/$name.desc
fi
cofdp_run_logged "$name" "$PYTHON" "$driver" --desc "$desc" \
	--log-dir "$RESULT_DIR/$name"
cofdp_log "fio logs and PDFs: $RESULT_DIR/$name/"
