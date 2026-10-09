#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# bind-solana.sh <layout>: reset the FDP SSD, create PiDir-FS and bind
# P-Directories into the directories of the Solana validator (Table 2
# "solana", Figs. 12 and 13). Start the node afterwards as printed at the end
# (launchers of blockchain-fdp/solana, see blockchain-fdp/README.md).
#
# The validator keeps its ledger, accounts and snapshots below
# $COFDP_TENANT0/p1 (launcher variables COFDP_TENANT_ROOT=$COFDP_TENANT0 and
# COFDP_PDIR=p1). Each $COFDP_TENANT0/pN is a bind mount of a P-Directory:
#
#   cofdp       tenant0/pN is the Initial P-Directory pN (PID N). The account
#               files, the accounts-hash cache and the accounts index (p2), the
#               blockstore RocksDB (p4) and the bank snapshots (p5) get their
#               own PIDs through bind mounts onto subdirectories of p1; the
#               rest of the ledger stays in p1. The write-ahead log of the
#               blockstore goes to tenant0/p3 (p3): the printed launcher
#               commands set COFDP_SOLANA_WAL_DIR=$COFDP_TENANT0/p3
#               (blockchain-fdp/README.md).
#   single-pid  every tenant0/pN is the P-Directory $SINGLE_PDIR (default fp3,
#               a Flexible P-Directory over PID 3), so the node writes all its
#               data with one PID. COFDP_SOLANA_WAL_DIR stays unset, so the
#               write-ahead log stays in the blockstore directory.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <layout>
  layout: cofdp single-pid
EOF
	exit 1
}

[ $# -eq 1 ] || usage
layout=$1
case $layout in
cofdp | single-pid) ;;
*) usage ;;
esac
t=$COFDP_TENANT0
m=$COFDP_MNT

cofdp_require_root
cofdp_setup_pidirfs_f2fs
cofdp_remove_tree "$t" "$COFDP_TENANT1"
cofdp_run mkdir -p "$t" "$COFDP_TENANT1"
cofdp_run mkdir -p "$t/p0" "$t/p1" "$t/p2" "$t/p3" "$t/p4" "$t/p5" "$t/p6" "$t/p7"

if [ "$layout" = cofdp ]; then
	for n in 7 6 5 4 3 2 1 0; do
		cofdp_bind "$m/p$n" "$t/p$n"
	done
	cofdp_run chmod 777 "$t"
	cofdp_chmod_entries "$t"
	cofdp_run mkdir -p "$t/p1/snapshot" "$t/p1/accounts" "$t/p1/banking_trace" \
		"$t/p1/accounts_hash_cache" "$t/p1/accounts_index" "$t/p1/rocksdb"
	cofdp_chmod_entries "$t/p1"
	# The targets are the subdirectories of p1 in the PiDir-FS mount; the
	# bind mounts reach $t/p1/<dir> by mount propagation (shared mounts, as on
	# systemd systems). accounts, accounts_hash_cache and accounts_index are
	# the same directory (KNOWN_ISSUES BC-8).
	for dir in accounts accounts_hash_cache accounts_index; do
		cofdp_bind "$t/p2" "$m/p1/$dir"
	done
	cofdp_bind "$t/p4" "$m/p1/rocksdb"
	cofdp_bind "$t/p5" "$m/p1/snapshot"
	cofdp_check_propagated "$t/p1/accounts" "$t/p1/accounts_hash_cache" \
		"$t/p1/accounts_index" "$t/p1/rocksdb" "$t/p1/snapshot"
else
	pdir=${SINGLE_PDIR:-fp3}
	cofdp_run mkdir -p "$m/$pdir"
	cofdp_run chmod 777 "$m/$pdir"
	for n in 7 6 5 4 3 2 1 0; do
		cofdp_bind "$m/$pdir" "$t/p$n"
	done
	cofdp_run chmod 777 "$t"
	cofdp_chmod_entries "$t" "$t/p0"
fi

# Environment of the launchers: setup.sh and bootstrap-validator.sh must see
# the same values
if [ "$layout" = cofdp ]; then
	launcher_env="export COFDP_TENANT_ROOT=$t COFDP_PDIR=p1 COFDP_SOLANA_WAL_DIR=$t/p3"
else
	launcher_env="export COFDP_TENANT_ROOT=$t COFDP_PDIR=p1; unset COFDP_SOLANA_WAL_DIR"
fi

cat >&2 <<EOF
[cofdp] PiDir-Binding done ($layout). Start the node as the user that runs it
[cofdp] (not root), in the patched Solana tree $SOLANA_SRC:
  $launcher_env
  NDEBUG=1 multinode-demo/cofdp/setup.sh
  NDEBUG=1 multinode-demo/faucet.sh &
  NDEBUG=1 multinode-demo/cofdp/bootstrap-validator.sh &
  NDEBUG=1 multinode-demo/cofdp/bench-tps.sh 2>&1 | tee $(cofdp_result_name solana "$layout").log
[cofdp] When bench-tps has finished, stop the validator and the faucet and run
[cofdp] scripts/common/finish-run.sh as root (FEMU counters of the run).
EOF
