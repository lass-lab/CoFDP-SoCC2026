#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# rocksdb.sh <f2fs|torfs|all>: build db_bench and the static library of the
# RocksDB forks in place, with DEBUG_LEVEL=0 (release build):
#   f2fs   FDP_ROCKSDB_F2FS (CoFDP and the KernelFDP baselines)
#   torfs  FDP_ROCKSDB_TORFS with the TorFS plugin (UserFDP; needs xNVMe)
#
# scripts/build/ycsb.sh links YCSB against these in-tree libraries.
# ROCKSDB_INSTALL=1 also runs 'make install' (into /usr/local); the two forks
# install the same file names, so the fork that is installed last wins.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

usage() {
	echo "usage: $0 <f2fs|torfs|all>" >&2
	exit 1
}

# build_fork <directory> [make variable=value...]
build_fork() {
	local dir=$COFDP_ROOT/$1

	shift
	cofdp_run make -C "$dir" -j"$JOBS" DEBUG_LEVEL=0 "$@" db_bench static_lib
	if [ "${ROCKSDB_INSTALL:-0}" = 1 ]; then
		cofdp_run sudo make -C "$dir" -j"$JOBS" DEBUG_LEVEL=0 "$@" install
	fi
	cofdp_require "$dir/db_bench"
	cofdp_require_files "$dir/librocksdb.a"
	cofdp_dry_run || cofdp_log "built $dir/db_bench and $dir/librocksdb.a"
}

[ $# -eq 1 ] || usage
case $1 in
f2fs) build_fork FDP_ROCKSDB_F2FS ;;
torfs) build_fork FDP_ROCKSDB_TORFS ROCKSDB_PLUGINS=torfs ;;
all)
	build_fork FDP_ROCKSDB_F2FS
	build_fork FDP_ROCKSDB_TORFS ROCKSDB_PLUGINS=torfs
	;;
*) usage ;;
esac
