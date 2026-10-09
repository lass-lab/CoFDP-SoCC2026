#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# bind-aptos.sh <layout>: reset the FDP SSD, create PiDir-FS and bind
# P-Directories into the directories of the Aptos executor benchmark (Table 2
# "aptos", Figs. 12 and 13). Run the benchmark afterwards as printed at the
# end (runner of blockchain-fdp/aptos, see blockchain-fdp/README.md).
#
# The benchmark creates its DB (db/) and the DB checkpoint (cp/) in
# $COFDP_TENANT0/p1 (runner variable COFDP_APTOS_DIR), each with ledger_db,
# state_kv_db and state_merkle_db. Each $COFDP_TENANT0/pN is a bind mount:
#
#   cofdp       tenant0/p1 is p1 (db/ledger_db); db/state_kv_db is p2,
#               db/state_merkle_db p3, and cp/ledger_db, cp/state_kv_db and
#               cp/state_merkle_db are p4, p5 and p6 (bind mounts of the
#               Initial P-Directories). db/ and cp/ are different mounts, so
#               the checkpoint copies the SST files (KNOWN_ISSUES BC-3).
#   single-pid  every tenant0/pN is the P-Directory $SINGLE_PDIR (default p0),
#               so the node writes all its data with one PID.
#   write-set   cofdp, plus db/ledger_db/write_set_db on p7
#   ledger      no checkpoint directories; db/state_kv_db p2,
#               db/state_merkle_db p3, and in db/ledger_db: transaction_db p5,
#               event_db p6, write_set_db p7
#   hot-shard   cofdp, plus db/state_kv_db/hot_shard_0 ... hot_shard_15 on
#               subdirectories of p7
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

layouts="cofdp single-pid write-set ledger hot-shard"

usage() {
	cat >&2 <<EOF
usage: $0 <layout>
  layout: $layouts
EOF
	exit 1
}

[ $# -eq 1 ] || usage
layout=$1
case " $layouts " in
*" $layout "*) ;;
*) usage ;;
esac
t=$COFDP_TENANT0
m=$COFDP_MNT
db=$t/p1/db
cp=$t/p1/cp

# db/ and cp/ with the three DBs each, in the bind mount of p1
make_db_dirs() {
	cofdp_bind "$m/p1" "$t/p1"
	cofdp_run mkdir -p "$db" "$cp"
	cofdp_run mkdir -p "$db/ledger_db" "$db/state_kv_db" "$db/state_merkle_db"
	if [ "$layout" != ledger ]; then
		cofdp_run mkdir -p "$cp/ledger_db" "$cp/state_kv_db" "$cp/state_merkle_db"
	fi
}

chmod_dirs() {
	cofdp_run chmod 777 "$t"
	cofdp_chmod_entries "$t" "$t/p1" "$db" "$cp"
}

bind_db_and_checkpoint() {
	cofdp_bind "$m/p6" "$cp/state_merkle_db"
	cofdp_bind "$m/p5" "$cp/state_kv_db"
	cofdp_bind "$m/p4" "$cp/ledger_db"
	cofdp_bind "$m/p3" "$db/state_merkle_db"
	cofdp_bind "$m/p2" "$db/state_kv_db"
}

cofdp_require_root
cofdp_setup_pidirfs_f2fs
cofdp_remove_tree "$t" "$COFDP_TENANT1"
cofdp_run mkdir -p "$t" "$COFDP_TENANT1"
cofdp_run mkdir -p "$t/p0" "$t/p1" "$t/p2" "$t/p3" "$t/p4" "$t/p5" "$t/p6" "$t/p7"

case $layout in
cofdp)
	make_db_dirs
	chmod_dirs
	bind_db_and_checkpoint
	cofdp_bind "$m/p7" "$t/p7"
	;;
single-pid)
	pdir=${SINGLE_PDIR:-p0}
	for n in 7 6 5 4 3 2 1 0; do
		cofdp_bind "$m/$pdir" "$t/p$n"
	done
	cofdp_run chmod 777 "$t"
	cofdp_chmod_entries "$t"
	;;
write-set)
	make_db_dirs
	cofdp_bind "$m/p7" "$t/p7"
	chmod_dirs
	bind_db_and_checkpoint
	cofdp_run mkdir "$db/ledger_db/write_set_db"
	cofdp_bind "$m/p7" "$db/ledger_db/write_set_db"
	cofdp_chmod_entries "$db/state_kv_db"
	;;
ledger)
	make_db_dirs
	cofdp_bind "$m/p7" "$t/p7"
	chmod_dirs
	cofdp_bind "$m/p3" "$db/state_merkle_db"
	cofdp_bind "$m/p2" "$db/state_kv_db"
	cofdp_run mkdir "$db/ledger_db/transaction_db" "$db/ledger_db/event_db" \
		"$db/ledger_db/write_set_db"
	cofdp_bind "$m/p5" "$db/ledger_db/transaction_db"
	cofdp_bind "$m/p6" "$db/ledger_db/event_db"
	cofdp_bind "$m/p7" "$db/ledger_db/write_set_db"
	cofdp_chmod_entries "$db/state_kv_db"
	;;
hot-shard)
	make_db_dirs
	cofdp_bind "$m/p7" "$t/p7"
	chmod_dirs
	bind_db_and_checkpoint
	for i in {0..15}; do
		cofdp_run mkdir -p "$db/state_kv_db/hot_shard_$i"
	done
	for i in {0..15}; do
		cofdp_run mkdir -p "$t/p7/hot_shard_$i"
	done
	for i in {0..15}; do
		cofdp_bind "$t/p7/hot_shard_$i" "$db/state_kv_db/hot_shard_$i"
	done
	cofdp_chmod_entries "$db/state_kv_db"
	;;
esac

cat >&2 <<EOF
[cofdp] PiDir-Binding done ($layout). Run the benchmark as the user that runs
[cofdp] it (not root), in the patched aptos-core tree $APTOS_SRC:
  COFDP_APTOS_DIR=$t/p1 ./testsuite/performance_benchmark_cofdp.sh --cofdp 2>&1 | tee $(cofdp_result_name aptos "$layout").log
[cofdp] (--cofdp-time for the time-based test). Afterwards run
[cofdp] scripts/common/finish-run.sh as root (FEMU counters of the run).
EOF
