# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
#
# Helpers of the CoFDP experiment recipes (sourced, not executed; sources
# env.sh). The recipes run as root in the FEMU guest.
#
# Every recipe follows this per-run sequence:
#   1. fdp-reset   FEMU prints the counters accumulated since the previous
#                  reset (the write-amplification dump of the previous run)
#                  and resets the emulated SSD to empty
#   2. umount      every mount of the namespace, including bind mounts
#   3. fdp-reset   discards what the unmount wrote
#   4. mkfs and mount of PiDir-FS, or of the baseline file system
#   5. P-Directories: mkdir, fp-remap, mount --bind (PiDir-Binding)
#   6. the workload, with its output in $RESULT_DIR
# The dump of a run is printed by the next fdp-reset: run finish-run.sh (or
# the next recipe) after the workload.
#
# Commands that change the system are printed with a leading '+' before they
# run, preceded by '+ cd <dir>' whenever a command runs in another directory
# than the one printed before it. With COFDP_DRY_RUN=1 they are only printed
# (no root needed).

# shellcheck source=scripts/common/env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

cofdp_log() { printf '[cofdp] %s\n' "$*" >&2; }
cofdp_die() { printf '[cofdp] error: %s\n' "$*" >&2; exit 1; }
cofdp_dry_run() { [ "${COFDP_DRY_RUN:-0}" = 1 ]; }

# The arguments as one shell-quoted command line
cofdp_quote() {
	local arg line=

	for arg in "$@"; do
		case $arg in
		"" | *[!A-Za-z0-9_./:=,@%+-]*) arg="'${arg//\'/\'\\\'\'}'" ;;
		esac
		line+=" $arg"
	done
	printf '%s' "${line# }"
}

# Print one line of the command trace, '+ <text>'. If the working directory
# is not the one of the previous line, '+ cd <dir>' comes first, so that the
# trace shows where every command runs.
cofdp_trace() {
	local cwd=${cofdp_dry_cwd:-$PWD}

	if [ "$cwd" != "${cofdp_traced_cwd:-}" ]; then
		cofdp_traced_cwd=$cwd
		printf '+ cd %s\n' "$(cofdp_quote "$cwd")" >&2
	fi
	printf '+ %s\n' "$*" >&2
}

# Print a command as '+ cmd arg...'
cofdp_show() {
	cofdp_trace "$(cofdp_quote "$@")"
}

# Run a command that changes the system (print it only with COFDP_DRY_RUN=1)
cofdp_run() {
	cofdp_show "$@"
	cofdp_dry_run || "$@"
}

cofdp_require_root() {
	cofdp_dry_run && return 0
	[ "$(id -u)" -eq 0 ] || cofdp_die "run this script as root (sudo)"
}

# Programs a recipe needs: paths must be executable, names must be in PATH
cofdp_require() {
	local cmd
	cofdp_dry_run && return 0
	for cmd in "$@"; do
		case $cmd in
		*/*) [ -x "$cmd" ] || cofdp_die "$cmd is missing; build it first (scripts/build/)" ;;
		*) command -v "$cmd" >/dev/null 2>&1 || cofdp_die "$cmd is not in PATH" ;;
		esac
	done
}

# Files a step needs that are not programs, e.g. a static library
cofdp_require_files() {
	local file
	cofdp_dry_run && return 0
	for file in "$@"; do
		[ -f "$file" ] || cofdp_die "$file is missing; build it first (scripts/build/)"
	done
}

# cd; the trace shows the new directory before the next command. With
# COFDP_DRY_RUN=1 a directory that an earlier, only printed step would create
# (git clone, mkdir) is not entered, but the trace shows it all the same.
cofdp_cd() {
	if cofdp_dry_run && [ ! -d "$1" ]; then
		case $1 in
		/*) cofdp_dry_cwd=$1 ;;
		*) cofdp_dry_cwd=${cofdp_dry_cwd:-$PWD}/$1 ;;
		esac
		return 0
	fi
	cd "$1" || cofdp_die "cannot enter $1"
	cofdp_dry_cwd=
}

cofdp_check_dirs() {
	local dir
	cofdp_dry_run && return 0
	for dir in "$@"; do
		[ -d "$dir" ] || cofdp_die "$dir does not exist"
	done
}

cofdp_check_mounted() {
	cofdp_dry_run && return 0
	mountpoint -q "$COFDP_MNT" || cofdp_die "$COFDP_MNT is not mounted"
}

# Reset the device. FEMU (tools/fdp-reset) first prints the counters it
# accumulated since the previous reset to its log: the pages written per PID
# and the pages copied by garbage collection.
cofdp_device_reset() {
	case $COFDP_DEVICE in
	femu) cofdp_run "$FDP_RESET" "$COFDP_DEV" ;;
	nvme-format) cofdp_run nvme format "$COFDP_DEV" --force ;;
	*) cofdp_die "COFDP_DEVICE must be femu or nvme-format, not '$COFDP_DEVICE'" ;;
	esac
}

# Mount points of $COFDP_DEV, the most recent first: its file system and every
# bind mount of one of its directories
cofdp_dev_mounts() {
	local majmin

	majmin=$(lsblk -dno MAJ:MIN "$COFDP_DEV") || return 1
	majmin=${majmin//[[:space:]]/}
	awk -v mm="$majmin" '$3 == mm { print $5 }' /proc/self/mountinfo | tac
}

# Unmount every mount of $COFDP_DEV. A previous run leaves bind mounts behind
# (the blockchain recipes nest them), and mkfs refuses a mounted device.
cofdp_umount_all() {
	local mounts mnt

	if cofdp_dry_run; then
		cofdp_trace "# unmount every mount of $COFDP_DEV"
		return 0
	fi
	[ -b "$COFDP_DEV" ] || cofdp_die "$COFDP_DEV is not a block device (set COFDP_DEV)"
	for _ in 1 2 3 4 5 6 7 8; do
		mounts=$(cofdp_dev_mounts)
		[ -n "$mounts" ] || return 0
		while IFS= read -r mnt; do
			# a mount propagated to a peer disappears with its source
			mountpoint -q "$mnt" || continue
			cofdp_show umount "$mnt"
			umount "$mnt" || :
		done <<<"$mounts"
	done
	mounts=$(cofdp_dev_mounts)
	[ -z "$mounts" ] || cofdp_die "$COFDP_DEV is still mounted on: ${mounts//$'\n'/ }"
}

# Steps 1-3 of every run. On a real FDP SSD (COFDP_DEVICE=nvme-format) the
# namespace is unmounted and then formatted.
cofdp_prepare_device() {
	cofdp_dry_run || [ -b "$COFDP_DEV" ] ||
		cofdp_die "$COFDP_DEV is not a block device (set COFDP_DEV)"
	if [ "$COFDP_DEVICE" = femu ]; then
		cofdp_require "$FDP_RESET"
		cofdp_device_reset
	else
		cofdp_require nvme
	fi
	cofdp_umount_all
	cofdp_device_reset
}

# mount -t f2fs -o fdp_log_n=<n>,discard,nobarrier,discard_unit=segment, the
# options of every F2FS recipe
cofdp_mount_f2fs() {
	cofdp_run mkdir -p "$COFDP_MNT"
	cofdp_run "$PIDIRFS_MOUNT" --dev "$COFDP_DEV" --dir "$COFDP_MNT" "$1"
	cofdp_check_mounted
}

# PiDir-FS on F2FS (CoFDP, KernelFDP-PC). mkfs.f2fs -O lost_found discovers
# the PIDs of the device and creates one Initial P-Directory p<PID> per PID;
# the mount gives PiDir-FS one log per PID (fdp_log_n = number of PIDs).
cofdp_setup_pidirfs_f2fs() {
	cofdp_require "$MKFS_F2FS" "$PIDIRFS_MOUNT"
	cofdp_prepare_device
	cofdp_run "$MKFS_F2FS" -f -O lost_found "$COFDP_DEV"
	cofdp_mount_f2fs "$COFDP_NR_PIDS"
	cofdp_check_dirs "$COFDP_MNT/p0"
}

# KernelFDP-F2FS baseline: a regular F2FS (no lost_found) mounted with
# fdp_log_n=1; the kernel chooses the PIDs from the F2FS log types.
cofdp_setup_kernelfdp_f2fs() {
	cofdp_require "$MKFS_F2FS" "$PIDIRFS_MOUNT"
	cofdp_prepare_device
	cofdp_run "$MKFS_F2FS" -f "$COFDP_DEV"
	cofdp_mount_f2fs 1
}

# PiDir-FS on XFS: one allocation group per PID; the mount creates the
# Initial P-Directories p0 ... p<N-1>. XFS does not parse P-Directory names,
# so the recipes set the PIDs of a new directory with fp-remap.
cofdp_setup_pidirfs_xfs() {
	local mount_opts=$1

	cofdp_require "$MKFS_XFS"
	cofdp_prepare_device
	cofdp_run "$MKFS_XFS" -d "fdp_pids=$COFDP_NR_PIDS" \
		-m crc=1,reflink=0,rmapbt=0,metadir=0 -i nrext64=0,exchange=0 \
		-n parent=0 "$COFDP_DEV" -f
	cofdp_run mkdir -p "$COFDP_MNT"
	cofdp_run mount -t xfs -o "$mount_opts" "$COFDP_DEV" "$COFDP_MNT"
	cofdp_check_mounted
	cofdp_check_dirs "$COFDP_MNT/p0"
}

# PiDir-Binding: mount a P-Directory onto a directory of an application
cofdp_bind() {
	cofdp_run mount --bind "$1" "$2"
}

# The blockchain and co-running-tenant recipes bind P-Directories onto
# subdirectories in the PiDir-FS mount; an application sees them below its
# own bind mount of the parent only through mount propagation, which is shared
# by default on systemd systems. Fail if it did not happen.
cofdp_check_propagated() {
	local dir

	cofdp_dry_run && return 0
	for dir in "$@"; do
		mountpoint -q "$dir" ||
			cofdp_die "the bind mount did not propagate to $dir (mount propagation is not shared)"
	done
}

# fp-remap <dir> <pid>...: set the PIDs of a directory and its subtree
cofdp_fp_remap() {
	cofdp_require "$FP_REMAP"
	cofdp_run "$FP_REMAP" "$@"
}

# rm -rf of a tenant directory before it is created again; refuses a tree with
# a file system mounted below it
cofdp_remove_tree() {
	local dir

	for dir in "$@"; do
		if ! cofdp_dry_run &&
		   awk -v d="$dir" '$5 == d || index($5, d "/") == 1 { f = 1 } END { exit !f }' \
			/proc/self/mountinfo; then
			cofdp_die "refusing to remove $dir: a file system is mounted below it"
		fi
		cofdp_run rm -rf --one-file-system -- "$dir"
	done
}

# chmod 777 <dir>/* for each argument (nothing for an empty directory)
cofdp_chmod_entries() {
	local dir entries

	for dir in "$@"; do
		if cofdp_dry_run; then
			cofdp_trace "chmod 777 $(cofdp_quote "$dir")/*"
			continue
		fi
		shopt -s nullglob
		entries=("$dir"/*)
		shopt -u nullglob
		[ ${#entries[@]} -eq 0 ] || cofdp_run chmod 777 "${entries[@]}"
	done
}

# ASLR off for the whole system, until the next reboot. Filebench needs it: it
# maps its shared memory at the same address in every worker process, and its
# own per-process switch does nothing (linux_disable_aslr() passes
# 0xffffffff | ADDR_NO_RANDOMIZE, i.e. 0xffffffff, to personality(), which
# only queries the persona). With ASLR on, a run fails with 'Unexpected
# Process termination' and 'NO VALID RESULTS!'. The Filebench recipes
# (filebench/run.sh, elasticity/fig17-filebench.sh) therefore turn it off;
# tenants/fig14.sh and the Fig. 16 recipes turn it off as well.
cofdp_aslr_off() {
	cofdp_run sysctl -w kernel.randomize_va_space=0
}

# cofdp_edited_copy <src> <dst> <sed option>...: write an edited copy of a
# workload file (the original is not changed)
cofdp_edited_copy() {
	local src=$1 dst=$2

	shift 2
	cofdp_show sed "$@" "$src"
	printf '  > %s\n' "$dst" >&2
	cofdp_dry_run && return 0
	mkdir -p "$(dirname "$dst")"
	sed "$@" "$src" >"$dst"
}

# Result name <part>-<part>-...-<RUN_TAG>
cofdp_result_name() {
	local IFS=-

	printf '%s-%s\n' "$*" "$RUN_TAG"
}

# cofdp_start_logged <name> <cmd>...: print a workload's command line and its
# log file $RESULT_DIR/<name>.log, and record the command line, working
# directory, kernel and start time in $RESULT_DIR/<name>.cmd
cofdp_start_logged() {
	local name=$1

	shift
	cofdp_show "$@"
	printf '  > %s\n' "$RESULT_DIR/$name.log" >&2
	cofdp_dry_run && return 0
	mkdir -p "$RESULT_DIR"
	printf 'command: %s\ncwd: %s\nkernel: %s\nstart: %s\n' "$(cofdp_quote "$@")" \
		"$PWD" "$(uname -r)" "$(date '+%F %T %z')" >"$RESULT_DIR/$name.cmd"
}

# cofdp_run_logged <name> <cmd>...: run a workload with stdout and stderr in
# $RESULT_DIR/<name>.log (and <name>.cmd, see cofdp_start_logged)
cofdp_run_logged() {
	local log=$RESULT_DIR/$1.log

	cofdp_start_logged "$@"
	cofdp_dry_run && return 0
	shift
	"$@" >"$log" 2>&1 || cofdp_die "the workload failed with status $?, see $log"
	cofdp_log "output: $log"
}

# Final message of a recipe
cofdp_finish_hint() {
	[ "$COFDP_DEVICE" = femu ] || return 0
	cofdp_log "for the WAF, run scripts/common/finish-run.sh now (FEMU prints the"
	cofdp_log "counters of this run to its log), then scripts/common/femu-waf.py <FEMU log>"
}
