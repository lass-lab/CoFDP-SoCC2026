# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
#
# RocksDB helpers (sourced after scripts/common/lib.sh by scripts/ycsb/run.sh,
# scripts/rocksdb/run-mixgraph.sh and scripts/tenants/fig15-tenant.sh): the
# file system and DB-directory layout of the RocksDB runs, and the db_bench
# flags of the mixgraph runs.
#
# The DB directory is $COFDP_TENANT0 (YCSB: rocksdb.properties_tenant0,
# db_bench: -db). The RocksDB F2FS fork puts WAL files into the first and the
# SST files of level L into the (L+2)-th of the DB entries whose names start
# with 'p' (sorted, capped at the last one; FDP_ROCKSDB_F2FS/docs/COFDP.md).
# PiDir-Binding mounts P-Directories onto the entries p0, p1 and p2, so the WAL
# goes to p0, L0 SSTs to p1 and the deeper levels to p2:
#
#   stack           file system              tenant0/p0     tenant0/p1     tenant0/p2
#   cofdp           PiDir-FS, fdp_log_n=8    pp0pp2pp4pp6   pp0pp2pp4pp6   pp1pp3pp5pp7
#   kernelfdp-pc    PiDir-FS, fdp_log_n=8    p1             p2             p3
#   kernelfdp-f2fs  F2FS, fdp_log_n=1        tenant0 is a bind mount of the directory
#                                            dirdir (no p* entries, no placement shim)
#   userfdp         none: the RocksDB TorFS fork on the raw namespace
#
# pp0pp2pp4pp6 and pp1pp3pp5pp7 are Flexible P-Directories (legacy prefix pp;
# fp is accepted as well) over the PIDs 0,2,4,6 and 1,3,5,7, i.e. one
# placement handle in each of the four RGs. p1, p2 and p3 are Initial
# P-Directories, one PID each.

rocksdb_stacks="cofdp kernelfdp-pc kernelfdp-f2fs userfdp"

# rocksdb_layout <stack>: reset the device and create the layout
rocksdb_layout() {
	local t=$COFDP_TENANT0 m=$COFDP_MNT

	case $1 in
	cofdp)
		cofdp_setup_pidirfs_f2fs
		cofdp_run mkdir "$m/pp0pp2pp4pp6"
		cofdp_run mkdir "$m/pp1pp3pp5pp7"
		cofdp_remove_tree "$t"
		cofdp_run mkdir "$t" "$t/p0" "$t/p1" "$t/p2"
		cofdp_bind "$m/pp0pp2pp4pp6" "$t/p0"
		cofdp_bind "$m/pp0pp2pp4pp6" "$t/p1"
		cofdp_bind "$m/pp1pp3pp5pp7" "$t/p2"
		;;
	kernelfdp-pc)
		cofdp_setup_pidirfs_f2fs
		cofdp_remove_tree "$t"
		cofdp_run mkdir "$t" "$t/p0" "$t/p1" "$t/p2"
		cofdp_bind "$m/p1" "$t/p0"
		cofdp_bind "$m/p2" "$t/p1"
		cofdp_bind "$m/p3" "$t/p2"
		;;
	kernelfdp-f2fs)
		cofdp_setup_kernelfdp_f2fs
		cofdp_remove_tree "$t"
		cofdp_run mkdir "$t"
		cofdp_run mkdir "$m/dirdir"
		cofdp_bind "$m/dirdir" "$t"
		;;
	userfdp)
		cofdp_prepare_device
		;;
	*)
		cofdp_die "unknown stack '$1' (one of: $rocksdb_stacks)"
		;;
	esac
}

# db_bench flags of the mixgraph runs (Table 2 "fillran." and "mixgraph"):
# fillrandom writes 26,214,400 keys (48-byte keys, 1 KiB values), then
# mixgraph runs 26,214,400 operations of the mixed GET/PUT/SEEK model.
# mixgraph_cmd adds the placement flags (-db, or TorFS' -stream_option and
# --fs_uri) and the seed between the two parts.
mixgraph_flags_head=("--benchmarks=fillrandom,mixgraph,stats" --histogram
	-subcompactions=8 -max_background_flushes=4 -max_background_compactions=4)
mixgraph_flags_tail=(-cache_size=268435456 -keyrange_dist_a=14.18
	-keyrange_dist_b=-2.917 -keyrange_dist_c=0.0164 -keyrange_dist_d=-0.08082
	-keyrange_num=30 -value_k=0.2615 -value_sigma=25.45 -iter_k=2.517
	-iter_sigma=14.236 -value_theta=2048 -mix_max_value_size=3000
	-mix_get_ratio=0.83 -mix_put_ratio=0.14 -mix_seek_ratio=0.03
	-sine_mix_rate_interval_milliseconds=0 -sine_a=1000 -sine_b=0.000073
	-sine_d=4500 --perf_level=2 -reads=26214400 -num=26214400 -value_size=1024
	-key_size=48 -use_existing_db=0)

# mixgraph_cmd <stack> <DB directory>: set the caller's array cmd to the
# db_bench command line of a mixgraph run (UserFDP has no DB directory)
# shellcheck disable=SC2034
mixgraph_cmd() {
	if [ "$1" = userfdp ]; then
		cmd=("$DB_BENCH_TORFS" "${mixgraph_flags_head[@]}" -stream_option=2
			"--fs_uri=torfs:xnvme:$COFDP_DEV?be=thrpool" -seed=1739254486043241
			"${mixgraph_flags_tail[@]}")
	else
		cmd=("$DB_BENCH_F2FS" "${mixgraph_flags_head[@]}" -seed=1739254486043241
			"-db=$2" "${mixgraph_flags_tail[@]}")
	fi
}
