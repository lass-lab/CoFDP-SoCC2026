#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# fig16a-cachelib.sh: CacheLib with its RGs changed at run time by fp-remap
# (Fig. 16(a)).
#
# Resets the FDP SSD, creates PiDir-FS on F2FS, creates the P-Directory fp3
# with the subdirectories small/ and large/ that hold the cache files, gives
# fp3 the PIDs 1, 3, 5 and 7 (one placement handle in each of the four RGs)
# and runs run_experiment_cachelib.py: cachebench with
# configs/cachelib/kvcache_l2_wc/config.json (override with CACHELIB_CONFIG),
# fp-remap of fp3 at the times of FP_REMAP_EVENTS in the driver, the
# throughput every 15 s, and a PDF.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
config=${CACHELIB_CONFIG:-$CACHELIB_CONFIGS/kvcache_l2_wc/config.json}
if [ -z "${CACHELIB_CONFIG:-}" ] && [ "$COFDP_MNT" != /home/femu/f2fs_fdp_mount ]; then
	cofdp_die "$config uses /home/femu/f2fs_fdp_mount; pass an edited copy as CACHELIB_CONFIG"
fi
name=$(cofdp_result_name fig16a cachelib)

cofdp_require_root
cofdp_require "$CACHEBENCH" "$FP_REMAP" "$PYTHON"
cofdp_aslr_off
cofdp_setup_pidirfs_f2fs
cofdp_run mkdir "$COFDP_MNT/fp3"
cofdp_run mkdir "$COFDP_MNT/fp3/small"
cofdp_run mkdir "$COFDP_MNT/fp3/large"
cofdp_fp_remap "$COFDP_MNT/fp3" 1 3 5 7
cofdp_run_logged "$name" "$PYTHON" "$here/run_experiment_cachelib.py" \
	--cachebench "$CACHEBENCH" --config "$config" --fp-remap "$FP_REMAP" \
	--fp3 "$COFDP_MNT/fp3" --result-dir "$RESULT_DIR/$name"
cofdp_log "result.csv, events.csv and result.pdf: $RESULT_DIR/$name/"
cofdp_finish_hint
