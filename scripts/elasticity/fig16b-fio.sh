#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# fig16b-fio.sh <fs>: a fio tenant with its RGs changed at run time by
# fp-remap (Fig. 16(b)).
#
# Resets the FDP SSD, creates PiDir-FS (fs = xfs or f2fs), creates the
# P-Directory fp3 with PID 3 (fp-remap) and runs run_experiment_fio.py: fio
# randrw (1 MiB, queue depth 32, direct I/O) on an 8 GiB file in fp3 for
# 1200 s, fp-remap of fp3 at the times of FP_REMAP_EVENTS in the driver, the
# throughput every 10 s, and a PDF.
#
#   xfs   mkfs.xfs -d fdp_pids=$COFDP_NR_PIDS, mounted with -o $XFS_MOUNT_OPTS
#         (default: discard)
#   f2fs  mkfs.f2fs -O lost_found, mounted with fdp_log_n=$COFDP_NR_PIDS
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

if [ $# -ne 1 ] || { [ "$1" != xfs ] && [ "$1" != f2fs ]; }; then
	echo "usage: $0 <xfs|f2fs>" >&2
	exit 1
fi
fs=$1
name=$(cofdp_result_name fig16b fio "$fs")

cofdp_require_root
cofdp_require fio "$FP_REMAP" "$PYTHON"
cofdp_aslr_off
if [ "$fs" = xfs ]; then
	cofdp_setup_pidirfs_xfs "${XFS_MOUNT_OPTS:-discard}"
else
	cofdp_setup_pidirfs_f2fs
fi
cofdp_run mkdir "$COFDP_MNT/fp3"
cofdp_fp_remap "$COFDP_MNT/fp3" 3
cofdp_run_logged "$name" "$PYTHON" "$here/run_experiment_fio.py" \
	--filename "$COFDP_MNT/fp3/fiotestfile" --fp-remap "$FP_REMAP" \
	--fp3 "$COFDP_MNT/fp3" --result-dir "$RESULT_DIR/$name"
cofdp_log "result.csv, events.csv and result.pdf: $RESULT_DIR/$name/"
cofdp_finish_hint
