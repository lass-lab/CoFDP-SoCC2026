#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# run-raw-tenants.sh: three fio tenants on the raw FDP namespace, each with its
# own LBA range and placement ID (Figs. 6 and 7; no file system).
#
# Resets the FDP SSD (any file system on it is unmounted) and runs
# bench_tenant_fdp_sep.py with tenant_desc_fdp_femu (override with
# TENANT_DESC=<file>): fio with io_uring_cmd passthrough on the namespace's
# character device (/dev/ngXnY), FDP placement through --fdp_pli. A tenant's
# pid= is an index into the device's placement-ID list, which ConfFDP reports
# handle by handle: with the default 4-RG preset the indices 0, 1 and 2 are
# PIDs 0, 2 and 4, one RG per tenant (Fig. 7); with configs/femu/cofdp-1rg.env
# they are PIDs 0, 1 and 2 of the single RG (Fig. 6). Then draw the figures with
#   python3 scripts/fio/bench_tenant_fdp_sep_plot.py --desc <desc> --log-dir <dir>
# (fio >= 3.34 and nvme-cli are needed).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/common/lib.sh
. "$here/../common/lib.sh"

if [ $# -ne 0 ]; then
	echo "usage: $0" >&2
	exit 1
fi
desc=${TENANT_DESC:-$here/tenant_desc_fdp_femu}
name=$(cofdp_result_name fio-raw-tenants)

cofdp_require_root
cofdp_require fio nvme "$PYTHON"
cofdp_prepare_device
cofdp_run_logged "$name" "$PYTHON" "$here/bench_tenant_fdp_sep.py" \
	--device "$COFDP_DEV" --desc "$desc" --log-dir "$RESULT_DIR/$name"
cofdp_log "fio logs and PDFs: $RESULT_DIR/$name/"
cofdp_finish_hint
