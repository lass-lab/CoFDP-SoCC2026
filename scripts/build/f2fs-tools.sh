#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# f2fs-tools.sh: build f2fs-tools-fdp in place; the recipes run
# f2fs-tools-fdp/mkfs/mkfs.f2fs from there (nothing is installed).
#
# mkfs.f2fs -O lost_found (PiDir-FS) discovers the placement IDs of the device
# through xNVMe, so configure fails without libxnvme (scripts/build/xnvme.sh).
# Extra configure options: F2FS_TOOLS_CONFIGURE_OPTS.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
read -r -a opts <<<"${F2FS_TOOLS_CONFIGURE_OPTS:-}"

cofdp_cd "$COFDP_ROOT/f2fs-tools-fdp"
cofdp_run ./autogen.sh
cofdp_run ./configure ${opts[@]+"${opts[@]}"}
cofdp_run make -j"$JOBS"
cofdp_require "$MKFS_F2FS"
cofdp_dry_run || cofdp_log "built $MKFS_F2FS"
