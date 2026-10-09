#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# ycsb.sh <f2fs|torfs|all>: build YCSB-cpp-fdp once per RocksDB fork, with the
# headers and the static library of that fork (scripts/build/rocksdb.sh
# first), and keep the binaries as YCSB-cpp-fdp/ycsb-f2fs (CoFDP and the
# KernelFDP baselines) and YCSB-cpp-fdp/ycsb-torfs (UserFDP).
#
# -L selects the fork's own librocksdb.a, so nothing needs to be installed.
# -lzstd is linked because RocksDB compiles ZSTD support into librocksdb.a
# when libzstd-dev is installed (scripts/build/deps.sh); the RocksDB options
# files (rocksdb_*_options.ini) select Snappy. The HdrHistogram_c submodule is
# cloned at the pinned commit if it is missing.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

usage() {
	echo "usage: $0 <f2fs|torfs|all>" >&2
	exit 1
}

hdrhistogram_commit=733b470eced028fac82a3cbfd064f2dcf545c6bc

# build_ycsb <variant> <RocksDB fork directory> <libraries>
build_ycsb() {
	local variant=$1 rocksdb=$COFDP_ROOT/$2 libs=$3

	cofdp_require_files "$rocksdb/librocksdb.a"
	cofdp_run make clean
	cofdp_run make BIND_ROCKSDB=1 -j"$JOBS" EXTRA_CXXFLAGS="-I$rocksdb/include" \
		EXTRA_LDFLAGS="-L$rocksdb $libs"
	cofdp_run cp ycsb "ycsb-$variant"
	cofdp_dry_run || cofdp_log "built $YCSB_DIR/ycsb-$variant"
}

[ $# -eq 1 ] || usage
case $1 in
f2fs | torfs | all) ;;
*) usage ;;
esac

cofdp_cd "$YCSB_DIR"
if [ ! -f HdrHistogram_c/CMakeLists.txt ]; then
	[ ! -d HdrHistogram_c ] || cofdp_run rmdir HdrHistogram_c
	cofdp_run git clone https://github.com/HdrHistogram/HdrHistogram_c.git HdrHistogram_c
	cofdp_run git -C HdrHistogram_c checkout "$hdrhistogram_commit"
fi
if [ "$1" != torfs ]; then
	build_ycsb f2fs FDP_ROCKSDB_F2FS \
		"-ldl -lsnappy -lgflags -lbz2 -ldl -llz4 -lz -luring -lzstd"
fi
if [ "$1" != f2fs ]; then
	build_ycsb torfs FDP_ROCKSDB_TORFS \
		"-ldl -lsnappy -lgflags -lbz2 -ldl -lbz2 -llz4 -lxnvme -lz -luring -u torfs_filesystem_reg -lzstd"
fi
