#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# run.sh <stack> <workload>: Filebench video server and OLTP (Table 2 "video"
# and "oltp", Figs. 12 and 13).
#
#   workload  personality                   directories (below $COFDP_MNT)
#   video     filebench-fdp/video_churn.f   p4 active videos, p5 passive videos
#   oltp      filebench-fdp/oltp.f          p4 log file, p5 data files
#
#   stack           file system             p4, p5
#   cofdp           PiDir-FS, fdp_log_n=8   Initial P-Directories (PIDs 4 and 5)
#   kernelfdp-f2fs  F2FS, fdp_log_n=1       plain directories (mkdir)
#
# video ends by itself. oltp does not (filebench-fdp/README.md): the script
# stops it with SIGINT FILEBENCH_OLTP_SECONDS seconds (default 600) after
# Filebench prints 'Running...' or, if FILEBENCH_OLTP_OPS=<n> is set, as soon
# as an IO Summary reports n or more operations (Table 2 lists oltp with 18M
# operations: FILEBENCH_OLTP_OPS=18000000). Ctrl-C stops either workload the
# same way. After SIGINT, Filebench prints 'Aborting...' and exits with status
# 1 without a final IO Summary. The script treats this as the normal end: it
# removes the shared-memory file and the semaphore set that Filebench leaves
# behind and prints the last IO Summary.
#
# Filebench is the upstream build of scripts/build/filebench.sh. It needs
# ASLR off, which the script turns off first (cofdp_aslr_off in lib.sh).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

usage() {
	cat >&2 <<EOF
usage: $0 <stack> <workload>
  stack:    cofdp kernelfdp-f2fs
  workload: video oltp
EOF
	exit 1
}

# The shared-memory files of Filebench, /tmp/filebench-shm-XXXXXX
filebench_shm_files() {
	find /tmp -maxdepth 1 -type f -name 'filebench-shm-*'
}

# Operations of the last IO Summary in a Filebench log (0 before the first one)
filebench_ops() {
	awk '$2 == "IO" && $3 == "Summary:" { ops = $4 } END { print (ops == "" ? 0 : ops) }' "$1"
}

# filebench_remove_shm <file>: remove a shared-memory file that Filebench left
# behind, and its System V semaphore set, whose key Filebench derives from the
# file (ftok(<file>, 1) in ipc_init())
filebench_remove_shm() {
	local shm=$1 dev ino key semid

	read -r dev ino < <(stat -c '%d %i' "$shm")
	key=$(printf '0x%08x' $(( (1 << 24) | ((dev & 0xff) << 16) | (ino & 0xffff) )))
	semid=$(ipcs -s | awk -v key="$key" '$1 == key { print $2 }')
	[ -z "$semid" ] || cofdp_run ipcrm -s "$semid"
	cofdp_run rm -f -- "$shm"
}

# filebench_run <name> <command>...: run Filebench with its output in
# $RESULT_DIR/<name>.log (as cofdp_run_logged does) and stop it after
# $stop_secs seconds or $stop_ops operations, as described at the top
filebench_run() {
	local log=$RESULT_DIR/$1.log pid status=0 stop='' running='' ops old_shm shm last

	cofdp_start_logged "$@"
	shift
	if cofdp_dry_run; then
		if [ -n "$stop_ops" ]; then
			cofdp_trace "# SIGINT to Filebench once an IO Summary reports $stop_ops ops"
		elif [ -n "$stop_secs" ]; then
			cofdp_trace "# SIGINT to Filebench $stop_secs s after 'Running...'"
		fi
		cofdp_trace "# then remove the shared-memory file and the semaphore set if Filebench left them"
		return 0
	fi

	old_shm=$(filebench_shm_files)
	: >"$log"
	"$@" >"$log" 2>&1 &
	pid=$!
	# Ctrl-C reaches Filebench directly; SIGTERM is passed on as SIGINT
	trap 'kill -INT "$pid" 2>/dev/null || :' INT TERM
	while kill -0 "$pid" 2>/dev/null; do
		if [ -z "$stop" ]; then
			if [ -z "$running" ] && grep -q 'Running\.\.\.' "$log"; then
				running=$SECONDS
			fi
			if [ -n "$stop_ops" ]; then
				ops=$(filebench_ops "$log") || ops=0
				[ "${ops:-0}" -lt "$stop_ops" ] || stop=ops
			elif [ -n "$stop_secs" ] && [ -n "$running" ] &&
			     [ $((SECONDS - running)) -ge "$stop_secs" ]; then
				stop=seconds
			fi
			if [ -n "$stop" ]; then
				cofdp_show kill -INT "$pid"
				kill -INT "$pid" 2>/dev/null || :
			fi
		fi
		sleep 1 || :
	done
	wait "$pid" || status=$?
	trap - INT TERM

	while IFS= read -r shm; do
		filebench_remove_shm "$shm"
	done < <(filebench_shm_files | grep -vxF -e "$old_shm" || :)

	last=$(awk '$2 == "IO" && $3 == "Summary:" { line = $0 } END { print line }' "$log")
	# A worker process that died (e.g. with ASLR on) ends the run early, but
	# Filebench may still exit with status 0
	if grep -q -E 'Unexpected Process termination|NO VALID RESULTS' "$log"; then
		cofdp_die "Filebench reported a failed run (status $status), see $log"
	fi
	if grep -q 'Aborting\.\.\.' "$log"; then
		case $stop in
		ops) cofdp_log "stopped Filebench with SIGINT: an IO Summary reported $stop_ops ops or more" ;;
		seconds) cofdp_log "stopped Filebench with SIGINT $stop_secs s after 'Running...'" ;;
		*) cofdp_log "Filebench was stopped with SIGINT" ;;
		esac
	elif [ "$status" -ne 0 ]; then
		cofdp_die "Filebench failed with status $status, see $log"
	fi
	cofdp_log "last IO Summary: ${last:-none}"
	cofdp_log "output: $log"
}

[ $# -eq 2 ] || usage
stack=$1
workload=$2
case $workload in
video) personality=video_churn.f ;;
oltp) personality=oltp.f ;;
*) usage ;;
esac
case $stack in
cofdp | kernelfdp-f2fs) ;;
*) usage ;;
esac
name=$(cofdp_result_name filebench "$stack" "$workload")
stop_ops=
stop_secs=
if [ "$workload" = oltp ]; then
	stop_ops=${FILEBENCH_OLTP_OPS:-}
	[ -n "$stop_ops" ] || stop_secs=${FILEBENCH_OLTP_SECONDS:-600}
	case $stop_secs$stop_ops in
	*[!0-9]*) cofdp_die "FILEBENCH_OLTP_SECONDS and FILEBENCH_OLTP_OPS must be numbers" ;;
	esac
fi

cofdp_require_root
cofdp_require "$FILEBENCH" ipcs ipcrm
cofdp_aslr_off
if [ "$stack" = cofdp ]; then
	cofdp_setup_pidirfs_f2fs
	cofdp_check_dirs "$COFDP_MNT/p4" "$COFDP_MNT/p5"
else
	cofdp_setup_kernelfdp_f2fs
	cofdp_run mkdir "$COFDP_MNT/p4" "$COFDP_MNT/p5"
fi

# The personalities name /home/femu/f2fs_fdp_mount literally
wml=$FILEBENCH_WORKLOADS/$personality
if [ "$COFDP_MNT" != /home/femu/f2fs_fdp_mount ]; then
	cofdp_edited_copy "$wml" "$RESULT_DIR/$name.f" \
		-e "s#/home/femu/f2fs_fdp_mount#$COFDP_MNT#g"
	wml=$RESULT_DIR/$name.f
fi

filebench_run "$name" "$FILEBENCH" -f "$wml"
cofdp_finish_hint
