#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# run.sh <stack> <workload>: CacheLib cachebench (Table 2, Figs. 12 and 13).
#
# The configuration is configs/cachelib/<workload>/<file> (override with
# CACHELIB_CONFIG=<path>). In the file-system stacks, the small and the large
# object cache each have a file in one of two directories below $COFDP_MNT
# (configs/cachelib/README.md):
#
#   stack           file system             <file>       directories
#   cofdp           PiDir-FS, fdp_log_n=8   cofdp.json   pp0pp2pp4pp6, pp1pp3pp5pp7
#   kernelfdp-pc    PiDir-FS, fdp_log_n=8   pc.json      p4, p5 (Initial P-Directories)
#   kernelfdp-f2fs  F2FS, fdp_log_n=1       kernel.json  p4, p5 (made with mkdir)
#   userfdp         none                    user.json    none (NVMe FDP passthrough)
#
# The graph workloads have no pc.json; KernelFDP-PC uses their kernel.json,
# which has the same settings.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

stacks="cofdp kernelfdp-pc kernelfdp-f2fs userfdp"

usage() {
	cat >&2 <<EOF
usage: $0 <stack> <workload>
  stack:    $stacks
  workload: cdn, kvc, gfl, gal, gff, gaf (or a directory of configs/cachelib/)
EOF
	exit 1
}

[ $# -eq 2 ] || usage
stack=$1
case $2 in
kvc) workload=kvcache ;;
gfl) workload=graph_cache_leader_fbobj ;;
gal) workload=graph_cache_leader_assocs ;;
gff) workload=graph_cache_follower_fbobj ;;
gaf) workload=graph_cache_follower_assocs ;;
*) workload=$2 ;;
esac
dir=$CACHELIB_CONFIGS/$workload
[ -d "$dir" ] || cofdp_die "no configurations in $dir"

case $stack in
cofdp) file=cofdp.json ;;
kernelfdp-pc) file=pc.json; [ -f "$dir/$file" ] || file=kernel.json ;;
kernelfdp-f2fs) file=kernel.json ;;
userfdp) file=user.json ;;
*) usage ;;
esac
config=${CACHELIB_CONFIG:-$dir/$file}
[ -f "$config" ] || cofdp_die "no configuration $config"
# The configurations name the device and the mount point literally
if [ -z "${CACHELIB_CONFIG:-}" ]; then
	if [ "$stack" = userfdp ]; then
		literal=/dev/nvme0n1 value=$COFDP_DEV
	else
		literal=/home/femu/f2fs_fdp_mount value=$COFDP_MNT
	fi
	[ "$value" = "$literal" ] ||
		cofdp_die "$config uses $literal; pass an edited copy as CACHELIB_CONFIG"
fi

cofdp_require_root
cofdp_require "$CACHEBENCH"
case $stack in
cofdp)
	cofdp_setup_pidirfs_f2fs
	cofdp_run mkdir "$COFDP_MNT/pp0pp2pp4pp6" "$COFDP_MNT/pp1pp3pp5pp7"
	;;
kernelfdp-pc)
	cofdp_setup_pidirfs_f2fs
	cofdp_check_dirs "$COFDP_MNT/p4" "$COFDP_MNT/p5"
	;;
kernelfdp-f2fs)
	cofdp_setup_kernelfdp_f2fs
	cofdp_run mkdir "$COFDP_MNT/p4" "$COFDP_MNT/p5"
	;;
userfdp)
	cofdp_prepare_device
	;;
esac
cofdp_run_logged "$(cofdp_result_name cachelib "$stack" "$workload")" \
	"$CACHEBENCH" --json_test_config "$config" --progress=100
cofdp_finish_hint
