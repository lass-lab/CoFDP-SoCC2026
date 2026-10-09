#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# run.sh <stack> <workload> [ycsb option...]: YCSB on RocksDB (Table 2,
# Figs. 12 and 13).
#
# <workload> is a spec in YCSB-cpp-fdp/workloads/: workloada ... workloadf,
# workload_zipfian, workload_uniform, workload_latest or workload_nutanix.
# The phases are $YCSB_PHASES, by default:
#   workload_{zipfian,uniform,latest}   -load (write-only: the load phase is
#                                        the workload)
#   workload_nutanix                    -run for kernelfdp-pc and userfdp,
#                                        -load -run for cofdp and kernelfdp-f2fs
#   all others                          -load -run
# Every ycsb invocation sleeps 720 s after the (possibly skipped) load phase,
# also with -load or -run alone (KNOWN_ISSUES YCSB-3).
#
# CoFDP and the KernelFDP baselines run ycsb-f2fs (built against the RocksDB
# F2FS fork) with rocksdb.properties_tenant0, i.e. the DB in $COFDP_TENANT0
# (layouts: scripts/rocksdb/lib.sh); UserFDP runs ycsb-torfs with
# rocksdb.properties_torfs (TorFS on the raw namespace). scripts/build/ycsb.sh
# builds both binaries. ycsb runs in YCSB-cpp-fdp/, to which the
# rocksdb.optionsfile paths are relative. Extra options (e.g.
# -p threadcount=4) are appended to the ycsb command line.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"
# shellcheck source=scripts/rocksdb/lib.sh
. "$here/../rocksdb/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <stack> <workload> [ycsb option...]
  stack:    $rocksdb_stacks
  workload: a file in $YCSB_DIR/workloads/
EOF
	exit 1
}

[ $# -ge 2 ] || usage
stack=$1
workload=${2#workloads/}
shift 2
case " $rocksdb_stacks " in
*" $stack "*) ;;
*) usage ;;
esac
[ -f "$YCSB_DIR/workloads/$workload" ] || cofdp_die "no workload $YCSB_DIR/workloads/$workload"

case $workload/$stack in
workload_zipfian/* | workload_uniform/* | workload_latest/*) default_phases=-load ;;
workload_nutanix/kernelfdp-pc | workload_nutanix/userfdp) default_phases=-run ;;
*) default_phases="-load -run" ;;
esac
read -r -a phases <<<"${YCSB_PHASES:-$default_phases}"

# The properties files name /dev/nvme0n1 and /home/femu/tenant0 literally
extra=()
if [ "$stack" = userfdp ]; then
	ycsb=$YCSB_TORFS
	props=rocksdb/rocksdb.properties_torfs
	[ "$COFDP_DEV" = /dev/nvme0n1 ] ||
		extra=(-p "rocksdb.fs_uri=torfs:xnvme:$COFDP_DEV?be=thrpool")
else
	ycsb=$YCSB_F2FS
	props=rocksdb/rocksdb.properties_tenant0
	[ "$COFDP_TENANT0" = /home/femu/tenant0 ] ||
		extra=(-p "rocksdb.dbname=$COFDP_TENANT0")
fi

cofdp_require_root
cofdp_require "$ycsb"
rocksdb_layout "$stack"
cofdp_cd "$YCSB_DIR"
cofdp_run_logged "$(cofdp_result_name ycsb "$stack" "$workload")" \
	"$ycsb" "${phases[@]}" -db rocksdb -P "workloads/$workload" -P "$props" -s \
	${extra[@]+"${extra[@]}"} "$@"
cofdp_finish_hint
