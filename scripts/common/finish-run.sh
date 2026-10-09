#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# finish-run.sh: make FEMU print the counters of the run that just ended.
#
# Sends the FEMU reset (tools/fdp-reset) with the file system of the run still
# mounted, exactly like the first step of the next recipe would. FEMU prints
# the pages written per PID ("RG r STREAM h : written : N") and the pages
# copied by garbage collection since the previous reset to its log
# (build-femu/log of ConfFDP on the host) and then resets the emulated SSD to
# empty, so the data of the run is gone. Compute the write amplification from
# the log with scripts/common/femu-waf.py.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
[ "$COFDP_DEVICE" = femu ] || cofdp_die "only FEMU keeps counters (COFDP_DEVICE=femu)"
cofdp_require_root
cofdp_require "$FDP_RESET"
cofdp_device_reset
cofdp_log "the counters of the run are in the FEMU log on the host"
