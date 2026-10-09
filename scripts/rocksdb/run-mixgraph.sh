#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# run-mixgraph.sh <stack> [db_bench option...]: RocksDB db_bench fillrandom
# followed by mixgraph (Table 2 "fillran." and "mixgraph", Figs. 12 and 13).
#
# fillrandom writes 26,214,400 keys (48-byte keys, 1 KiB values), then
# mixgraph runs 26,214,400 operations of the mixed GET/PUT/SEEK model with the
# parameters of scripts/rocksdb/lib.sh. CoFDP and the KernelFDP baselines run
# the RocksDB F2FS fork with the DB in $COFDP_TENANT0 (layouts:
# scripts/rocksdb/lib.sh); UserFDP runs the RocksDB TorFS fork on the raw
# namespace with its level separation policy stream_option=2 (see
# FDP_ROCKSDB_TORFS). Extra options are appended to the db_bench command line.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"
# shellcheck source=scripts/rocksdb/lib.sh
. "$here/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <stack> [db_bench option...]
  stack: $rocksdb_stacks
EOF
	exit 1
}

[ $# -ge 1 ] || usage
stack=$1
shift
case " $rocksdb_stacks " in
*" $stack "*) ;;
*) usage ;;
esac
mixgraph_cmd "$stack" "$COFDP_TENANT0"
cmd+=("$@")

cofdp_require_root
cofdp_require "${cmd[0]}"
rocksdb_layout "$stack"
cofdp_run_logged "$(cofdp_result_name rocksdb-mixgraph "$stack")" "${cmd[@]}"
cofdp_finish_hint
