#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# umount-all.sh: unmount every mount of the FDP namespace ($COFDP_DEV), i.e.
# its file system and all bind mounts of its directories that PiDir-Binding
# created (a plain 'umount /dev/nvme0n1' removes only one mount at a time).
# Every recipe does this itself before mkfs.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
cofdp_require_root
cofdp_umount_all
cofdp_log "$COFDP_DEV is not mounted"
