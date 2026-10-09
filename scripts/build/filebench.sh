#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# filebench.sh: build and install upstream Filebench (master at 22620e6,
# reporting version 1.5-alpha3; see filebench-fdp/README.md) from
# $FILEBENCH_SRC (default $COFDP_HOME/filebench). 'make install' is needed:
# the fileserver personalities load the cvar-gamma library from the install
# directory.
#
# Filebench runs only with ASLR off ('sysctl -w kernel.randomize_va_space=0',
# until the next reboot), see filebench-fdp/README.md; the Filebench recipes
# (scripts/filebench/run.sh, scripts/elasticity/fig17-filebench.sh) set it.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
src=${FILEBENCH_SRC:-$COFDP_HOME/filebench}

[ -d "$src/.git" ] || cofdp_run git clone https://github.com/filebench/filebench.git "$src"
cofdp_cd "$src"
cofdp_run git checkout 22620e6
cofdp_run libtoolize
cofdp_run aclocal
cofdp_run autoheader
cofdp_run automake --add-missing
cofdp_run autoconf
cofdp_run ./configure
cofdp_run make -j"$JOBS"
cofdp_run sudo make install
cofdp_dry_run || cofdp_log "installed filebench; it needs ASLR off (sysctl -w kernel.randomize_va_space=0)"
