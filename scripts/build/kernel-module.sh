#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# kernel-module.sh <f2fs|xfs>: rebuild, install and reload only the F2FS or
# XFS module of linux-fdp, e.g. after changing fs/f2fs.
#
# Valid only on a kernel built from this tree with scripts/build/kernel.sh:
# the CoFDP block-layer, iomap and NVMe changes are in the kernel image and
# are not rebuilt here, and the module must match the running kernel's
# struct bio. No file system of that type may be mounted.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 1 ] || { [ "$1" != f2fs ] && [ "$1" != xfs ]; }; then
	echo "usage: $0 <f2fs|xfs>" >&2
	exit 1
fi
fs=$1

cofdp_cd "$COFDP_ROOT/linux-fdp"
cofdp_dry_run || [ -f .config ] ||
	cofdp_die "linux-fdp is not configured; build the kernel with scripts/build/kernel.sh"
if ! cofdp_dry_run && findmnt -rn -t "$fs" >/dev/null; then
	cofdp_die "a $fs file system is mounted; unmount it first (scripts/common/umount-all.sh)"
fi
cofdp_run make modules M="fs/$fs" -j"$JOBS"
cofdp_run sudo make modules_install M="fs/$fs"
if cofdp_dry_run || [ -d "/sys/module/$fs" ]; then
	cofdp_run sudo rmmod "$fs"
fi
cofdp_run sudo insmod "./fs/$fs/$fs.ko"

# the module now loaded must be the one just built
cofdp_dry_run && exit 0
built=$(modinfo -F srcversion "fs/$fs/$fs.ko" 2>/dev/null || :)
loaded=$(cat "/sys/module/$fs/srcversion" 2>/dev/null || :)
if [ -n "$built" ] && [ "$built" != "$loaded" ]; then
	cofdp_die "the loaded $fs module is not fs/$fs/$fs.ko"
fi
cofdp_log "fs/$fs/$fs.ko is loaded"
