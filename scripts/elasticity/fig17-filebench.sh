#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# fig17-filebench.sh <fs>: Filebench fileserver with its RGs changed at run
# time by fp-remap (Fig. 17(a) F2FS, Fig. 17(b) XFS).
#
# Resets the FDP SSD, creates PiDir-FS (fs = f2fs or xfs), creates the
# P-Directory fp3 with PID 3 (fp-remap) and runs run_experiment_filebench.py
# with filebench-fdp/fileserver_<fs>.f: fp-remap of fp3 at the times of
# FP_REMAP_EVENTS in the driver (600 s ... 3600 s), the throughput every
# 60 s, and a PDF.
#
#   f2fs  mkfs.f2fs -O lost_found, mounted with fdp_log_n=$COFDP_NR_PIDS
#   xfs   mkfs.xfs -d fdp_pids=$COFDP_NR_PIDS, mounted with -o $XFS_MOUNT_OPTS
#         (default: discard)
#
# The personality ends with 'psrun $runtime', which Filebench runs for 60 s
# only (KNOWN_ISSUES SCR-1). The driver therefore gets a copy in the result
# directory whose last line is $FILEBENCH_PSRUN (default 'psrun 10 4200':
# cumulative statistics every 10 s for 4200 s); set FILEBENCH_PSRUN= (empty)
# to keep the line. The copy's $dir is $COFDP_MNT/fp3.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

if [ $# -ne 1 ] || { [ "$1" != f2fs ] && [ "$1" != xfs ]; }; then
	echo "usage: $0 <f2fs|xfs>" >&2
	exit 1
fi
fs=$1
name=$(cofdp_result_name fig17 filebench "$fs")
psrun=${FILEBENCH_PSRUN-psrun 10 4200}
# the driver runs 'filebench' from PATH
case $FILEBENCH in
*/*) PATH=$(dirname "$FILEBENCH"):$PATH ;;
esac

cofdp_require_root
cofdp_require "$FILEBENCH" "$FP_REMAP" "$PYTHON"
cofdp_aslr_off
if [ "$fs" = f2fs ]; then
	cofdp_setup_pidirfs_f2fs
else
	cofdp_setup_pidirfs_xfs "${XFS_MOUNT_OPTS:-discard}"
fi
cofdp_run mkdir "$COFDP_MNT/fp3"
cofdp_fp_remap "$COFDP_MNT/fp3" 3

edits=(-e "s#^set \\\$dir=.*#set \$dir=$COFDP_MNT/fp3#")
[ -z "$psrun" ] || edits+=(-e "s#^psrun .*#$psrun#")
cofdp_edited_copy "$FILEBENCH_WORKLOADS/fileserver_$fs.f" "$RESULT_DIR/$name.f" "${edits[@]}"
cofdp_run_logged "$name" "$PYTHON" "$here/run_experiment_filebench.py" \
	--workload "$RESULT_DIR/$name.f" --fp-remap "$FP_REMAP" \
	--fp3 "$COFDP_MNT/fp3" --result-dir "$RESULT_DIR/$name"
cofdp_log "result.csv, events.csv and result.pdf: $RESULT_DIR/$name/"
cofdp_finish_hint
