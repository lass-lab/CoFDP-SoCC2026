#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# tools.sh: build the CoFDP tools in tools/ (fp-remap, fdp-reset,
# fdp-ruh-status, pidirfs-mount; see tools/README.md). fdp-reset and
# fdp-ruh-status need xNVMe (scripts/build/xnvme.sh).
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi

cofdp_run make -C "$COFDP_ROOT/tools" -j"$JOBS"
cofdp_require "$FP_REMAP" "$FDP_RESET" "$FDP_RUH_STATUS" "$PIDIRFS_MOUNT"
cofdp_dry_run || cofdp_log "built the tools in $COFDP_ROOT/tools"
