# blockchain-fdp

Patches for the two blockchain workloads of the CoFDP evaluation: a Solana
bootstrap validator driven by `solana-bench-tps`, and the Aptos single-node
executor benchmark. PiDir-Binding bind-mounts P-Directories onto the data
directories of a node, and every file created below a P-Directory is written
with its PID. The patches add launchers that point the nodes at the P-Directory
tree, benchmark options and measurements, and an option that moves the
write-ahead log of the Solana blockstore into a directory of its own
(`COFDP_SOLANA_WAL_DIR`). Apart from that option, they do not change how the
nodes store data.

| Workload | Upstream base | Toolchain | Patches |
|---|---|---|---|
| Solana | [solana-labs/solana](https://github.com/solana-labs/solana) tag `v1.18.26` (commit `d9f20e951`) | Rust 1.75.0 | [`solana/`](solana) |
| Aptos | [aptos-labs/aptos-core](https://github.com/aptos-labs/aptos-core) `main` at `2f99b2fbeb` (2025-12-29) | Rust 1.90.0 | [`aptos/`](aptos) |

Both repositories pin their toolchain in `rust-toolchain.toml`, so `rustup`
installs the right compiler on the first `cargo` call. Use the solana-labs
repository: the `v1.18.26` tag of its successor, anza-xyz/agave, is a different
commit. The binding recipes (device reset, mkfs, mount, bind mounts, run) are in
[`scripts/blockchain/`](../scripts/blockchain).

## Patches

### Solana

- `0001-bench-tps-per-batch-send-statistics.patch`: every sender thread of
  `solana-bench-tps` records, for each batch it sends, how long the batch took
  (milliseconds, measured like the `Tx send done` log line) and the batch's send
  rate (transactions per second). A batch is `--tx-count / --threads` transfers
  (1250 with the defaults below). After upstream's summary (`Average TPS`, used
  for throughput) the run prints:

  ```
  time elapsed <seconds>
  latency stats (n = <batches>):
  mean    : ...
  stdev   : ...
  min     : ...
  p5     : ...
  ...
  p99.999 : ...
  max     : ...

  Sorted latencies for cumulative graph:
  tps stats (n = <batches>):
  ...
  ```

  `latency stats` summarise the per-batch send times in milliseconds and
  `tps stats` the per-batch send rates. Percentile pX is the sample at index
  ceil(X/100 * (n-1)) of the sorted samples. `time elapsed` is the wall time
  since the start of the run.
- `0002-multinode-demo-cofdp-launchers.patch`: `multinode-demo/cofdp/setup.sh`,
  `bootstrap-validator.sh` and `bench-tps.sh`. They are the upstream
  `multinode-demo` scripts, except that the genesis ledger, the accounts and the
  snapshots of the bootstrap validator live in the P-Directory tree
  `$COFDP_TENANT_ROOT/$COFDP_PDIR` (default `/home/femu/tenant0/p1`), and that
  bench-tps defaults to `--duration 1200 --tx-count 10000 --threads 8`. Any
  bench-tps option can be overridden on the command line, e.g.
  `cofdp/bench-tps.sh --duration 900 --thread-batch-sleep-ms 2000`. The upstream
  scripts are not modified.
- `0003-ledger-optional-blockstore-wal-directory.patch`: if
  `COFDP_SOLANA_WAL_DIR` is set and not empty, the blockstore keeps the
  write-ahead log (WAL) of its RocksDB in that directory instead of in the
  ledger's `rocksdb/`, and the validator log shows
  `Blockstore WAL directory: "<dir>" (COFDP_SOLANA_WAL_DIR)`. Unset or empty:
  upstream behaviour.
  - Every process that opens the ledger reads the variable (`solana-genesis` in
    `setup.sh`, `solana-validator`, `solana-ledger-tool`). Export the same value
    for all of them: a process started without it does not see the data that is
    still in the WAL.
  - RocksDB creates the directory if its parent exists.
  - The directory must be empty when `setup.sh` creates the ledger;
    `solana-genesis` fails (`wal_dir contains existing log file`) while it holds
    the WAL of an earlier ledger.

### Aptos

- `0001-executor-benchmark-time-based-mode-and-measurements.patch`
  - `BENCHMARK_DURATION_SECS` makes `aptos-executor-benchmark` stop generating
    blocks after that many seconds (time-based runs).
  - The `Overall` results also report the elapsed time (`HH:MM:SS`), the
    number of generated blocks and blocks/s.
  - `aptos-block-executor` records the execution time of each transaction
    incarnation, and the benchmark prints percentiles of it
    (`<prefix> p99: <N> us`); see BC-2 below.
  - `create-db` accepts an existing data directory, because the P-Directories
    are bind-mounted onto its subdirectories before the run.
- `0002-testsuite-cofdp-runner.patch`
  - `testsuite/performance_benchmark_cofdp.sh --cofdp` runs 300 blocks of
    `apt-fa-transfer` (`FLOW=MAINNET`).
  - `--cofdp-time` runs the time-based test `apt-fa-transfer-sj`
    (`apt-fa-transfer`, block generation stops after 530 s, `FLOW=ADHOC`).
  - Both modes use 8 execution threads and enable the pruners.
  - `testsuite/single_node_performance.py` creates the benchmark DB (`db/`)
    and its checkpoint (`cp/`) under `$COFDP_APTOS_DIR`, which the runner sets
    (default `/home/femu/tenant0/p1`). It parses the new measurements into the
    result table and into the `grep_json_single_node_perf` JSON line
    (`num_blocks`, `blocks_per_second`, `elapsed_time_secs`, `p10_latency_us` ...
    `p99_999_latency_us`). `FLOW=MAINNET` and `FLOW=ADHOC` contain only the two
    tests above; the other flows are unchanged from upstream.

## Build

Build inside the FEMU guest that runs the experiments.

Solana:

```sh
git clone https://github.com/solana-labs/solana.git && cd solana
git checkout v1.18.26              # d9f20e951a06b61e4505da0955228020b96a8915
git apply /path/to/CoFDP/blockchain-fdp/solana/*.patch
cargo build --release --bin solana-genesis --bin solana-keygen --bin solana-faucet \
    --bin solana-validator --bin solana-bench-tps
```

With `NDEBUG=1` the `multinode-demo` scripts run these release binaries through
`cargo run --release`. `setup.sh` calls `fetch-spl.sh`, which downloads the SPL
programs, so the guest needs network access. Build dependencies on Ubuntu, as
listed in Solana's README: `libssl-dev libudev-dev pkg-config zlib1g-dev llvm
clang cmake make libprotobuf-dev protobuf-compiler`.

Aptos:

```sh
git clone https://github.com/aptos-labs/aptos-core.git && cd aptos-core
git checkout 2f99b2fbeb            # 2f99b2fbeb63ee3ee4d8fbdbd1aba164e39ed7b8
git apply /path/to/CoFDP/blockchain-fdp/aptos/*.patch
./scripts/dev_setup.sh -b          # build dependencies
pip3 install tabulate              # used by single_node_performance.py
cargo build --release -p aptos-executor-benchmark
```

To download only the base commit instead of the whole history, replace the
first two lines with

```sh
git init aptos-core && cd aptos-core
git fetch --depth 1 https://github.com/aptos-labs/aptos-core.git 2f99b2fbeb63ee3ee4d8fbdbd1aba164e39ed7b8
git checkout FETCH_HEAD
```

`single_node_performance.py` rebuilds `aptos-executor-benchmark` before it runs,
so building it beforehand only saves time. The runner raises the open-file limit
with `ulimit -n 1048576`; the hard limit must allow that
(`/etc/security/limits.conf`).

## Run

First reset the emulated SSD, format and mount PiDir-FS, and set up the binding
([`scripts/blockchain/`](../scripts/blockchain)). Then:

```sh
# Solana, from the solana checkout
export COFDP_SOLANA_WAL_DIR=/home/femu/tenant0/p3    # CoFDP binding only
NDEBUG=1 multinode-demo/cofdp/setup.sh
NDEBUG=1 multinode-demo/faucet.sh &
NDEBUG=1 multinode-demo/cofdp/bootstrap-validator.sh &
NDEBUG=1 multinode-demo/cofdp/bench-tps.sh

# Aptos, from the aptos-core checkout
./testsuite/performance_benchmark_cofdp.sh --cofdp          # or --cofdp-time
```

| Variable | Default | Used by |
|---|---|---|
| `COFDP_TENANT_ROOT` | `/home/femu/tenant0` | `multinode-demo/cofdp/{setup,bootstrap-validator}.sh` |
| `COFDP_PDIR` | `p1` | `multinode-demo/cofdp/{setup,bootstrap-validator}.sh` |
| `COFDP_SOLANA_WAL_DIR` | unset: the WAL stays in the ledger's `rocksdb/` | `solana-genesis` (`setup.sh`), `solana-validator` (`bootstrap-validator.sh`) |
| `COFDP_APTOS_DIR` | `/home/femu/tenant0/p1` | `testsuite/performance_benchmark_cofdp.sh` |

`setup.sh` and `bootstrap-validator.sh` must see the same values. The write
amplification of a run is taken from the emulator's statistics, which are printed
when the device is reset before the next run (see `scripts/blockchain/`).

## PiDir-Binding of the node directories

`mkfs.f2fs -O lost_found` creates one Initial P-Directory per device PID, `p0` to
`p7`, in the root of the PiDir-FS volume (mounted at `/home/femu/f2fs_fdp_mount`
with `fdp_log_n=8`). The nodes keep their data under `/home/femu/tenant0/p1`, a
bind mount of `p1`. To give a data directory of the node its own PID, the binding
mounts another P-Directory onto it; everything the node creates there inherits
that PID. The CoFDP bindings are:

Solana (`--ledger` and `--snapshots` are `tenant0/p1`, `--accounts` is
`tenant0/p1/accounts`; the accounts-hash cache and the disk index are in their
default places below the ledger; `COFDP_SOLANA_WAL_DIR` is `tenant0/p3`):

| Directory | Contents | P-Directory |
|---|---|---|
| `tenant0/p1` (ledger root) | genesis, `banking_trace/`, full snapshot archives | `p1` |
| `tenant0/p1/{accounts,accounts_hash_cache,accounts_index}/` | account storage, accounts-hash cache, disk index | `p2` |
| `tenant0/p3` | write-ahead log of the blockstore | `p3` |
| `tenant0/p1/rocksdb/` | blockstore (RocksDB) without its write-ahead log | `p4` |
| `tenant0/p1/snapshot/` | bank snapshots | `p5` |

Aptos (`COFDP_APTOS_DIR` is `tenant0/p1`):

| Directory under `tenant0/p1` | Contents | P-Directory |
|---|---|---|
| `db/ledger_db/` | ledger DB | `p1` |
| `db/state_kv_db/` | state key-value DB (all shards) | `p2` |
| `db/state_merkle_db/` | state Merkle DB (all shards) | `p3` |
| `cp/ledger_db/`, `cp/state_kv_db/`, `cp/state_merkle_db/` | checkpoint of the three DBs | `p4`, `p5`, `p6` |

In the single-PID baseline the whole `tenant0` tree is backed by one
P-Directory, so all data of the node is written with one PID. The exact mount
commands of each configuration are in [`scripts/blockchain/`](../scripts/blockchain).

## Known issues

These are documented, not fixed; they are also listed in the top-level
[`KNOWN_ISSUES.md`](../KNOWN_ISSUES.md).

- BC-2: the Aptos execution-latency percentiles (`<prefix> pXX: N us`, the
  `pXX us` table columns and the `pXX_latency_us` JSON fields) only contain the
  samples of the threads that print results. The BlockSTM worker threads, which
  execute the user transactions, never hand their samples over, so the
  percentiles in practice describe the one block-epilogue transaction per block
  that the pipeline's execution thread runs itself.
- BC-3: in the Aptos binding `db/` and `cp/` are different mounts, so RocksDB
  copies the SST files into the checkpoint (`link(2)` fails with `EXDEV`), while
  with the single-PID binding it hard-links them.
- BC-5: in the time-based Aptos test, block generation always stops after 530 s;
  the runner's `BENCHMARK_DURATION_SECS=1200` only switches the account sizing
  of the time-based mode.
- BC-6: when a time limit stops a raw-transfer workload early, the benchmark
  still counts the requested number of blocks, which misreports the transaction
  count and TPS or makes the run panic (not used by the CoFDP tests, which run
  `apt-fa-transfer`).
- BC-8: in the Solana binding `accounts/`, `accounts_hash_cache/` and
  `accounts_index/` are bind mounts of the same directory (`p2`), and the bind
  mounts reach the validator's directories only through mount propagation
  (shared mounts, as on systemd systems). Because the three directories are one,
  the accounts-hash cache cleanup after every full accounts-hash calculation also
  deletes the disk index's bucket files and its restart file while they are in
  use (they stay mapped, so the running validator is not affected); if the
  validator crashes and `bootstrap-validator.sh` restarts it, the index setup
  then erases the shared directory, account storage included.
- BC-11: `HIDE_OUTPUT=0` still hides the benchmark output in
  `single_node_performance.py`.
- BC-13: a DB left in `$COFDP_APTOS_DIR` by a previous run is not detected;
  reformat the volume before every run.

## License

The patches modify upstream code and are distributed under the license of the
code they modify. Each directory contains that license:

- `solana/`: Apache-2.0, the license of Solana ([`solana/LICENSE`](solana/LICENSE)).
- `aptos/`: the Innovation-Enabling Source Code License of aptos-core
  ([`aptos/LICENSE`](aptos/LICENSE)). The `testsuite/` scripts of `0002` keep
  the Apache-2.0 header of the upstream scripts they are based on.

The Innovation-Enabling Source Code License, in short (the license text is
binding):

- It grants the right to copy, modify, create derivative works of and
  redistribute the code, and to use it for non-production and non-commercial
  purposes, solely for your internal purposes.
- It does not allow using the code, or any information in it, to create,
  publish, deploy, launch or offer a blockchain or protocol (mainnet, testnet,
  devnet, beta or alpha) outside personal, non-production and non-commercial
  environments.
- All copies of the original or modified code and all derivative works,
  including these patches, are subject to the license. The license must be
  displayed conspicuously with every copy.
- Four years after Aptos Foundation publicly uploaded a version of the code,
  the license of that version ends, and the code can then be used under the
  terms of the Apache-2.0 license.
