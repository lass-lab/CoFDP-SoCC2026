#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# xnvme.sh: build and install xNVMe (default v0.7.4) into /usr/local.
# mkfs.f2fs -O lost_found (f2fs-tools-fdp), fdp-reset and fdp-ruh-status
# (tools/) and the TorFS plugin (FDP_ROCKSDB_TORFS) use it to send NVMe I/O
# Management commands. The source is cloned into $XNVME_SRC (default
# $COFDP_HOME/xnvme).
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
version=${XNVME_VERSION:-v0.7.4}
src=${XNVME_SRC:-$COFDP_HOME/xnvme}

cofdp_require git meson ninja
[ -d "$src/.git" ] || cofdp_run git clone https://github.com/xnvme/xnvme.git "$src"
cofdp_cd "$src"
cofdp_run git checkout "$version"
# Linux backends only: thrpool, libaio, NVMe passthrough, and io_uring if
# liburing >= 2.2 is installed (the CoFDP tools and TorFS use thrpool)
cofdp_run meson setup builddir --prefix=/usr/local -Dwith-spdk=false \
	-Dwith-libvfn=disabled -Dwith-isal=disabled -Dexamples=false -Dtests=false
cofdp_run ninja -C builddir -j "$JOBS"
cofdp_run sudo ninja -C builddir install
cofdp_run sudo ldconfig
