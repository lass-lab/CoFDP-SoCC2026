# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
#
# Default settings of the CoFDP experiment scripts (sourced, not executed).
#
# The defaults are the paper's setup: a FEMU guest whose user is femu, the
# emulated FDP SSD at /dev/nvme0n1 with 8 PIDs (4 RGs x 2 placement handles,
# see configs/femu/) and PiDir-FS mounted on /home/femu/f2fs_fdp_mount.
# Every variable can be overridden from the environment, e.g.
#   sudo COFDP_DEV=/dev/nvme1n1 scripts/ycsb/run.sh cofdp workloada
# The CacheLib configurations (configs/cachelib/) and the Filebench
# personalities (filebench-fdp/) contain the default mount point literally.

# Repository root (this file is scripts/common/env.sh)
: "${COFDP_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# Guest home directory, FDP namespace and PiDir-FS mount point
: "${COFDP_HOME:=/home/femu}"
: "${COFDP_DEV:=/dev/nvme0n1}"
: "${COFDP_MNT:=$COFDP_HOME/f2fs_fdp_mount}"
# Number of PIDs of the device: fdp_log_n of PiDir-FS on F2FS and
# mkfs.xfs -d fdp_pids of PiDir-FS on XFS
: "${COFDP_NR_PIDS:=8}"
# femu: reset with tools/fdp-reset (FEMU prints its counters, see finish-run.sh)
# nvme-format: real FDP SSD, reset with 'nvme format --force' (no counters)
: "${COFDP_DEVICE:=femu}"

# Directories into which the recipes bind-mount P-Directories (PiDir-Binding);
# YCSB-cpp-fdp/rocksdb/rocksdb.properties_tenant{0,1} and the blockchain
# launchers use them as data directories
: "${COFDP_TENANT0:=$COFDP_HOME/tenant0}"
: "${COFDP_TENANT1:=$COFDP_HOME/tenant1}"

# Results: <recipe>-<stack>-<workload>-<RUN_TAG>.log plus a .cmd file with
# the command line, kernel and start time
: "${RESULT_DIR:=$COFDP_HOME/results}"
: "${RUN_TAG:=$(date +%Y%m%d-%H%M%S)}"

# CoFDP tools (tools/, scripts/build/tools.sh)
: "${FDP_RESET:=$COFDP_ROOT/tools/fdp-reset}"
: "${FDP_RUH_STATUS:=$COFDP_ROOT/tools/fdp-ruh-status}"
: "${FP_REMAP:=$COFDP_ROOT/tools/fp-remap}"
: "${PIDIRFS_MOUNT:=$COFDP_ROOT/tools/pidirfs-mount}"

# File-system tools (scripts/build/f2fs-tools.sh, scripts/build/xfsprogs.sh)
: "${MKFS_F2FS:=$COFDP_ROOT/f2fs-tools-fdp/mkfs/mkfs.f2fs}"
: "${MKFS_XFS:=$COFDP_ROOT/xfsprogs-fdp/mkfs/mkfs.xfs}"

# Workloads (scripts/build/)
: "${DB_BENCH_F2FS:=$COFDP_ROOT/FDP_ROCKSDB_F2FS/db_bench}"
: "${DB_BENCH_TORFS:=$COFDP_ROOT/FDP_ROCKSDB_TORFS/db_bench}"
: "${YCSB_DIR:=$COFDP_ROOT/YCSB-cpp-fdp}"
: "${YCSB_F2FS:=$YCSB_DIR/ycsb-f2fs}"
: "${YCSB_TORFS:=$YCSB_DIR/ycsb-torfs}"
: "${CACHEBENCH:=$COFDP_ROOT/FDP_CACHELIB/opt/cachelib/bin/cachebench}"
: "${CACHELIB_CONFIGS:=$COFDP_ROOT/configs/cachelib}"
: "${FILEBENCH:=filebench}"
: "${FILEBENCH_WORKLOADS:=$COFDP_ROOT/filebench-fdp}"
: "${SOLANA_SRC:=$COFDP_HOME/solana}"
: "${APTOS_SRC:=$COFDP_HOME/aptos-core}"
: "${PYTHON:=python3}"

# Parallel build jobs
: "${JOBS:=$(nproc)}"

export COFDP_ROOT COFDP_HOME COFDP_DEV COFDP_MNT COFDP_NR_PIDS COFDP_DEVICE
export COFDP_TENANT0 COFDP_TENANT1 RESULT_DIR RUN_TAG
export FDP_RESET FDP_RUH_STATUS FP_REMAP PIDIRFS_MOUNT MKFS_F2FS MKFS_XFS
export DB_BENCH_F2FS DB_BENCH_TORFS YCSB_DIR YCSB_F2FS YCSB_TORFS CACHEBENCH
export CACHELIB_CONFIGS FILEBENCH FILEBENCH_WORKLOADS SOLANA_SRC APTOS_SRC
export PYTHON JOBS
