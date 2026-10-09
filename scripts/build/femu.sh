#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# femu.sh: build ConfFDP (FEMU) on the HOST, in ConfFDP/build-femu, the
# FEMU way: the run scripts are copied into build-femu/ and
# femu-compile.sh configures and builds qemu-system-x86_64 there.
# FEMU_INSTALL_DEPS=1 first installs the build dependencies (FEMU's
# pkgdep.sh, plus liburing-dev where the distribution has it).
#
# run-fdp.sh opens the guest image with aio=io_uring, which QEMU supports only
# when it is built with liburing, so the build stops if liburing is missing.
# Ubuntu 20.04 has no liburing package: install liburing from source there
# (https://github.com/axboe/liburing).
#
# Then start the guest from build-femu/ with the default preset (configs/femu/):
#   cd ConfFDP/build-femu && ./run-fdp.sh
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi

build=$COFDP_ROOT/ConfFDP/build-femu
cofdp_run mkdir -p "$build"
cofdp_cd "$build"
cofdp_run cp ../femu-scripts/femu-copy-scripts.sh .
cofdp_run ./femu-copy-scripts.sh .
if [ "${FEMU_INSTALL_DEPS:-0}" = 1 ]; then
	cofdp_run sudo ./pkgdep.sh
	if apt-cache show liburing-dev >/dev/null 2>&1; then
		cofdp_run sudo apt-get install -y liburing-dev
	fi
fi
cofdp_dry_run || pkg-config --exists liburing ||
	cofdp_die "liburing is not installed (pkg-config liburing); see the top of $0"
cofdp_run ./femu-compile.sh
# femu-compile.sh does not report failures in its exit status
cofdp_require "$build/qemu-system-x86_64"
cofdp_dry_run || cofdp_log "built $build/qemu-system-x86_64"
