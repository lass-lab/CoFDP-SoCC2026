#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# fig15-tenant.sh <tenant> [option...]: run one workload of the co-running
# tenants (Fig. 15) in the directories that fig15-bind.sh prepared; nothing
# is formatted or mounted. Start each tenant at its time from its own
# terminal and stop it with Ctrl-C (SIGINT), which the script treats as the
# normal end of the tenant; the output goes to
# $RESULT_DIR/fig15-<tenant>-<RUN_TAG>.log. Extra options are appended to the
# workload's command line.
#
#   gff       CacheLib GFF in tenant1/p0 and tenant1/p1
#             (configs/cachelib/graph_cache_follower_fbobj/isol.json)
#   ycsbe     YCSB-E on RocksDB with the DB in tenant1
#             (workloads/workloade_isol, rocksdb.properties_tenant1)
#   cdn       CacheLib cdn in tenant1/p0 and tenant1/p1
#             (configs/cachelib/cdn/isol.json)
#   mixgraph  RocksDB db_bench fillrandom + mixgraph with the DB in tenant0,
#             with the flags of scripts/rocksdb/run-mixgraph.sh
#
# Solana (tenant0) runs as a normal user from the patched Solana tree, with
# COFDP_TENANT_ROOT=$COFDP_TENANT0 COFDP_PDIR=p1 and without
# COFDP_SOLANA_WAL_DIR (scripts/README.md, Fig. 15).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"
# shellcheck source=scripts/rocksdb/lib.sh
. "$here/../rocksdb/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <tenant> [option...]
  tenant: gff ycsbe cdn mixgraph
EOF
	exit 1
}

# tenant_run <name> <cmd>...: cofdp_run_logged, except that a tenant stopped
# with SIGINT (Ctrl-C, or exit status 130) has ended normally
tenant_run() {
	local log=$RESULT_DIR/$1.log status=0 interrupted=

	cofdp_start_logged "$@"
	cofdp_dry_run && return 0
	shift
	trap 'interrupted=1' INT
	"$@" >"$log" 2>&1 || status=$?
	trap - INT
	if [ -n "$interrupted" ] || [ "$status" -eq 130 ]; then
		cofdp_log "stopped with SIGINT (status $status)"
	elif [ "$status" -ne 0 ]; then
		cofdp_die "the workload failed with status $status, see $log"
	fi
	cofdp_log "output: $log"
}

[ $# -ge 1 ] || usage
tenant=$1
shift
case $tenant in
gff | cdn)
	if [ "$tenant" = gff ]; then
		config=$CACHELIB_CONFIGS/graph_cache_follower_fbobj/isol.json
	else
		config=$CACHELIB_CONFIGS/cdn/isol.json
	fi
	[ "$COFDP_TENANT1" = /home/femu/tenant1 ] ||
		cofdp_die "$config uses /home/femu/tenant1 (COFDP_TENANT1=$COFDP_TENANT1)"
	cmd=("$CACHEBENCH" --json_test_config "$config" --report_api_latency --progress=100)
	;;
ycsbe)
	cmd=("$YCSB_F2FS" -load -run -db rocksdb -P workloads/workloade_isol
		-P rocksdb/rocksdb.properties_tenant1 -s)
	[ "$COFDP_TENANT1" = /home/femu/tenant1 ] ||
		cmd+=(-p "rocksdb.dbname=$COFDP_TENANT1")
	cofdp_cd "$YCSB_DIR"
	;;
mixgraph)
	mixgraph_cmd cofdp "$COFDP_TENANT0"
	;;
*)
	usage
	;;
esac
cmd+=("$@")

cofdp_require_root
cofdp_require "${cmd[0]}"
cofdp_check_mounted
tenant_run "$(cofdp_result_name fig15 "$tenant")" "${cmd[@]}"
