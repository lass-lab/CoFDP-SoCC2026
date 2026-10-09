#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# xfsprogs.sh: build xfsprogs-fdp in place; the recipes run
# xfsprogs-fdp/mkfs/mkfs.xfs from there. It adds mkfs.xfs -d fdp_pids=N
# (PiDir-FS on XFS: one allocation group per PID). XFSPROGS_INSTALL=1 also
# installs it (sudo make install), replacing the system's xfsprogs.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi

cofdp_cd "$COFDP_ROOT/xfsprogs-fdp"
cofdp_run make -j"$JOBS"
if [ "${XFSPROGS_INSTALL:-0}" = 1 ]; then
	cofdp_run sudo make install
fi
cofdp_require "$MKFS_XFS"
cofdp_dry_run || cofdp_log "built $MKFS_XFS"
