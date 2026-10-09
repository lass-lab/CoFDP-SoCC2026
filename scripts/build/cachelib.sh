#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# cachelib.sh: build FDP_CACHELIB with its contrib/build.sh (which installs
# the dependencies and builds them, then CacheLib, into FDP_CACHELIB/opt);
# the recipes run FDP_CACHELIB/opt/cachelib/bin/cachebench. The fork needs
# liburing >= 2.2, installed before the build. build.sh options:
# CACHELIB_BUILD_OPTS (default -j: all CPUs, release build with debug info).
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
read -r -a opts <<<"${CACHELIB_BUILD_OPTS:--j}"

cofdp_dry_run || pkg-config --atleast-version=2.2 liburing ||
	cofdp_die "liburing >= 2.2 is required (pkg-config liburing)"
cofdp_cd "$COFDP_ROOT/FDP_CACHELIB"
cofdp_run ./contrib/build.sh "${opts[@]}"
cofdp_require "$CACHEBENCH"
cofdp_dry_run || cofdp_log "built $CACHEBENCH"
