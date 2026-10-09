#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# fig15-bind.sh <stack> <tenants>: reset the FDP SSD, create PiDir-FS and bind
# P-Directories into the directories of two tenants, $COFDP_TENANT0 and
# $COFDP_TENANT1 (co-running tenants, Fig. 15). Start the tenants' workloads
# afterwards (scripts/README.md, "Fig. 15"); they find their data
# directories below the two tenant directories.
#
# <tenants> selects which workloads run in the two tenant directories:
#   solana-gff-ycsbe  tenant0: Solana (ledger in tenant0/p1); tenant1:
#                     CacheLib GFF, then YCSB-E (RocksDB)
#   mixgraph-cdn      tenant0: RocksDB mixgraph (DB tenant0); tenant1:
#                     CacheLib cdn
# CacheLib keeps its cache files in tenant1/p0 and tenant1/p1
# (configs/cachelib/*/isol.json); a RocksDB DB in a tenant directory uses its
# p* entries (scripts/rocksdb/lib.sh).
#
# <stack>:
#   cofdp         each tenant gets the Initial P-Directories of two RGs:
#                 tenant0/p0, p1, p2 and p3...p7 are p0, p1, p2 and p3 (RGs 0
#                 and 1), tenant1/p0, p1, p2 and p3...p7 are p4, p5, p6 and p7
#                 (RGs 2 and 3). With Solana, its banking trace, accounts-hash
#                 cache and blockstore are bound onto tenant0/p0, p2 and p3.
#   kernelfdp-pc  the tenants' directories get PIDs without regard to RGs:
#                 solana-gff-ycsbe: tenant0/p0, p1 and p2...p7 are p0, p1 and
#                 p2; tenant1/p0, p1, p2 and p3...p7 are p0, p5, p6 and p7;
#                 Solana's banking trace, accounts-hash cache and accounts
#                 index are p7, p3 and p2.
#                 mixgraph-cdn: tenant0 as above; tenant1/p0 and p1 are p4
#                 and p3.
# (With the 4-RG preset, PID p is in RG p / 2.)
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <stack> <tenants>
  stack:   cofdp kernelfdp-pc
  tenants: solana-gff-ycsbe mixgraph-cdn
EOF
	exit 1
}

[ $# -eq 2 ] || usage
stack=$1
tenants=$2
case $stack in
cofdp | kernelfdp-pc) ;;
*) usage ;;
esac
case $tenants in
solana-gff-ycsbe | mixgraph-cdn) ;;
*) usage ;;
esac
t0=$COFDP_TENANT0
t1=$COFDP_TENANT1
m=$COFDP_MNT

# bind_tenant <tenant dir> <P-Directory for p0> ... <P-Directory for p7>
bind_tenant() {
	local dir=$1 n
	local -a pdirs

	shift
	pdirs=("$@")
	for n in 7 6 5 4 3 2 1 0; do
		cofdp_bind "$m/${pdirs[n]}" "$dir/p$n"
	done
}

# Solana's subdirectories of tenant0/p1: the binds target the PiDir-FS mount
# and reach $t0/p1 by mount propagation (shared mounts, as on systemd systems)
solana_dirs() {
	cofdp_run chmod 777 "$t0"
	cofdp_chmod_entries "$t0"
	cofdp_run mkdir -p "$t0/p1/banking_trace" "$t0/p1/accounts_hash_cache" \
		"$t0/p1/accounts_index" "$t0/p1/rocksdb"
	cofdp_chmod_entries "$t0/p1"
}

cofdp_require_root
cofdp_setup_pidirfs_f2fs
cofdp_remove_tree "$t0" "$t1"
cofdp_run mkdir -p "$t0" "$t1"
cofdp_run mkdir -p "$t0/p0" "$t0/p1" "$t0/p2" "$t0/p3" "$t0/p4" "$t0/p5" "$t0/p6" "$t0/p7"
cofdp_run mkdir -p "$t1/p0" "$t1/p1" "$t1/p2" "$t1/p3" "$t1/p4" "$t1/p5" "$t1/p6" "$t1/p7"

case $stack/$tenants in
cofdp/*)
	bind_tenant "$t0" p0 p1 p2 p3 p3 p3 p3 p3
	bind_tenant "$t1" p4 p5 p6 p7 p7 p7 p7 p7
	if [ "$tenants" = solana-gff-ycsbe ]; then
		solana_dirs
		cofdp_bind "$t0/p0" "$m/p1/banking_trace"
		cofdp_bind "$t0/p2" "$m/p1/accounts_hash_cache"
		cofdp_bind "$t0/p3" "$m/p1/rocksdb"
		cofdp_check_propagated "$t0/p1/banking_trace" \
			"$t0/p1/accounts_hash_cache" "$t0/p1/rocksdb"
	fi
	;;
kernelfdp-pc/solana-gff-ycsbe)
	bind_tenant "$t1" p0 p5 p6 p7 p7 p7 p7 p7
	bind_tenant "$t0" p0 p1 p2 p2 p2 p2 p2 p2
	solana_dirs
	cofdp_bind "$m/p7" "$m/p1/banking_trace"
	cofdp_bind "$m/p3" "$m/p1/accounts_hash_cache"
	cofdp_bind "$m/p2" "$m/p1/accounts_index"
	cofdp_check_propagated "$t0/p1/banking_trace" \
		"$t0/p1/accounts_hash_cache" "$t0/p1/accounts_index"
	;;
kernelfdp-pc/mixgraph-cdn)
	bind_tenant "$t0" p0 p1 p2 p2 p2 p2 p2 p2
	cofdp_bind "$m/p4" "$t1/p0"
	cofdp_bind "$m/p3" "$t1/p1"
	;;
esac

cofdp_log "PiDir-Binding done ($stack, $tenants); start the tenants (scripts/README.md, Fig. 15)"
