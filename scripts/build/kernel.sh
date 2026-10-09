#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# kernel.sh: build and install the CoFDP kernel (linux-fdp) in the guest,
# then reboot into it.
#
# CoFDP adds a PID field to struct bio and struct request, which every module
# that handles bios sees. The kernel image and ALL modules must therefore come
# from this tree and be installed together (make modules_install install); a
# module of another build must not be loaded. scripts/build/kernel-module.sh
# rebuilds just F2FS or XFS on top of a kernel built here.
#
# The configuration starts from $KERNEL_CONFIG (default: the configuration of
# the running kernel) unless linux-fdp/.config exists; F2FS, XFS and the NVMe
# driver are enabled (as modules) if the configuration lacks them.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi

cofdp_cd "$COFDP_ROOT/linux-fdp"
if [ ! -f .config ]; then
	cofdp_run cp "${KERNEL_CONFIG:-/boot/config-$(uname -r)}" .config
	# distribution configurations name the distribution's signing keys
	cofdp_run scripts/config --set-str SYSTEM_TRUSTED_KEYS "" \
		--set-str SYSTEM_REVOCATION_KEYS ""
fi
cofdp_run make olddefconfig
for opt in F2FS_FS XFS_FS BLK_DEV_NVME; do
	if cofdp_dry_run; then
		cofdp_trace "# enable CONFIG_$opt as a module unless it is y or m"
		continue
	fi
	case $(scripts/config --state "$opt") in
	y | m) ;;
	*) cofdp_run scripts/config --module "$opt" ;;
	esac
done
cofdp_run make olddefconfig
cofdp_run make -j"$JOBS"
cofdp_run sudo make modules_install
cofdp_run sudo make install
cofdp_dry_run || cofdp_log "installed $(make -s kernelrelease); reboot into it"
