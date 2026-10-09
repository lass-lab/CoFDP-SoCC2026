#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# solana.sh: check out Solana v1.18.26 into $SOLANA_SRC (default
# $COFDP_HOME/solana), apply the CoFDP patches (blockchain-fdp/solana/) and
# build the release binaries the launchers use (blockchain-fdp/README.md).
# Install rustup (https://rustup.rs) first: the tree pins its Rust toolchain,
# which rustup installs on the first cargo call.
set -euo pipefail
# shellcheck source=scripts/common/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
patches=("$COFDP_ROOT"/blockchain-fdp/solana/*.patch)

# rustup installs cargo into ~/.cargo
if [ -f "$HOME/.cargo/env" ]; then
	# shellcheck source=/dev/null
	. "$HOME/.cargo/env"
fi
cofdp_require git cargo
[ -d "$SOLANA_SRC/.git" ] ||
	cofdp_run git clone https://github.com/solana-labs/solana.git "$SOLANA_SRC"
cofdp_cd "$SOLANA_SRC"
if [ -d "$SOLANA_SRC/.git" ] &&
   git -C "$SOLANA_SRC" apply --reverse --check "${patches[@]}" 2>/dev/null; then
	cofdp_log "the CoFDP patches are already applied"
else
	cofdp_run git checkout v1.18.26
	cofdp_run git apply "${patches[@]}"
fi
cofdp_run cargo build --release --bin solana-genesis --bin solana-keygen \
	--bin solana-faucet --bin solana-validator --bin solana-bench-tps
