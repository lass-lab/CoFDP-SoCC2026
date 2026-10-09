#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# deps.sh: install the packages the guest builds and recipes need (Ubuntu
# 20.04 or later). Run as a user with sudo rights.
#
# Not covered here:
# * xNVMe 0.7.x (scripts/build/xnvme.sh) and Filebench
#   (scripts/build/filebench.sh)
# * CacheLib's own dependencies (its contrib/build.sh installs them)
# * liburing >= 2.2: CacheLib needs it, and RocksDB and YCSB link it as well.
#   Ubuntu 20.04 has no liburing package and 22.04 ships 2.1, so install it
#   from source there (https://github.com/axboe/liburing). liburing-dev is
#   installed below where the distribution has it.
# * fio >= 3.34 for Figs. 6 and 7 (scripts/fio/: io_uring_cmd with FDP
#   placement). Ubuntu 20.04 ships fio 3.16; build a newer fio
#   (https://github.com/axboe/fio) for those figures.
# * rustup (https://rustup.rs), which scripts/build/solana.sh needs;
#   scripts/build/aptos.sh installs it through Aptos' dev_setup.sh
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi

packages=(
	# kernel
	build-essential bc bison flex libelf-dev libssl-dev dwarves cpio kmod rsync zstd
	# f2fs-tools, xfsprogs, tools, Filebench
	autoconf automake libtool pkg-config uuid-dev libblkid-dev libinih-dev liburcu-dev
	# xNVMe
	ninja-build libaio-dev libnuma-dev python3-pip
	# RocksDB, YCSB
	cmake libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev
	# recipes
	git fio nvme-cli python3-matplotlib util-linux
)
cofdp_run sudo apt-get update
if apt-cache show liburing-dev >/dev/null 2>&1; then
	packages+=(liburing-dev)
else
	cofdp_log "the distribution has no liburing-dev: install liburing >= 2.2 from source"
fi
cofdp_run sudo apt-get install -y "${packages[@]}"
# xNVMe needs a newer meson than Ubuntu 20.04 ships
cofdp_run sudo pip3 install "meson==${MESON_VERSION:-1.10.0}"
