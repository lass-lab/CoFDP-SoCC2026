#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# aptos.sh: check out aptos-core 2f99b2fbeb into $APTOS_SRC (default
# $COFDP_HOME/aptos-core), apply the CoFDP patches (blockchain-fdp/aptos/),
# install the build dependencies and build aptos-executor-benchmark
# (blockchain-fdp/README.md). The tree pins its Rust toolchain.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
patches=("$COFDP_ROOT"/blockchain-fdp/aptos/*.patch)

cofdp_require git
[ -d "$APTOS_SRC/.git" ] ||
	cofdp_run git clone https://github.com/aptos-labs/aptos-core.git "$APTOS_SRC"
cofdp_cd "$APTOS_SRC"
if [ -d "$APTOS_SRC/.git" ] &&
   git -C "$APTOS_SRC" apply --reverse --check "${patches[@]}" 2>/dev/null; then
	cofdp_log "the CoFDP patches are already applied"
else
	cofdp_run git checkout 2f99b2fbeb
	cofdp_run git apply "${patches[@]}"
fi
cofdp_run ./scripts/dev_setup.sh -b
# dev_setup.sh installs rustup into ~/.cargo
if [ -f "$HOME/.cargo/env" ]; then
	# shellcheck source=/dev/null
	. "$HOME/.cargo/env"
fi
cofdp_run pip3 install tabulate
cofdp_run cargo build --release -p aptos-executor-benchmark
