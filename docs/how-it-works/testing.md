# Testing and profiling

[Quick start](../using/quickstart.md) · [Operations](../operations/operations.md) ·
[What to work on](../status/README.md) · [Verification plan](verification.md)

## `make test`, `make test-long` and `make test-all`

The code tests come in two groups, so that the everyday suite stays fast:

| Target | What runs | Typical time (8-core host) | When |
| --- | --- | --- | --- |
| `make test` | Fast, CPU-bound unit tests; each command well under a second | about 45 s | Every edit |
| `make test-long` | Tests that wait on real deadlines, start daemons or processes, inject faults, or carry heavy workloads (1 s or more each) | under 15 min at `EFS_TEST_JOBS=8` (W160) | Before merging and in gates |
| `make test-all` | Both | under 15 min at `EFS_TEST_JOBS=8` | Gates, release checks, `make check` |

A new test goes into `test` only if it is CPU-bound and finishes in well
under a second; anything slower goes into `test-long`.
`tests/test_make_test_recipe.py` checks that `test-all` runs every unit
binary and Python test, and that no command runs in both groups.
`scripts/make_test_summary.sh` runs `test-all` by default; set
`EFS_TEST_TARGET=test` or `test-long` to run one group. It writes
`timings.tsv`, slowest first, to keep the split honest. With
`EFS_TEST_JOBS=8` it runs tests in a bounded pool, each with its own
`TMPDIR`. Tests that use real processes, shutdown, RDMA, timing,
sanitizers, benchmarks or build introspection still run one at a time,
after the pool.

Every `test-long` command has a row in `tests/budgets.tsv` (W160): the
seconds it may take at `EFS_TEST_JOBS=8` on an 8-CPU host, the pool slots
it occupies and, above the per-command bound, why. Slots weigh disk use,
not CPU (the daemon tests use 0.15–0.3 CPU each, but their stores fsync on
one disk, so eight at once only slow each other down and fail on
deadlines): 6 for a test with a latency bound that needs a quiet disk
(report step-down), 5 for one whose driver fsyncs for most of its run (B
growth, stop progress; two of those never overlap), 2 for a daemon test
that writes a moderate set, 1 for one that mostly waits on daemon timers
or is CPU-bound. The pool starts the commands with the most slots first,
then the largest budgets, and admits a command only while its slots are
free, skipping to the next that fits. A command that runs past its budget
is listed `OVERBUDGET` in `summary.txt` (the verdict does not change). `tests/test_make_test_recipe.py` requires a row for
every `test-long` command, a reason for a budget above 300 s, and keeps
the gate the budgets add up to (the pool's schedule plus the exclusive
commands) under 780 s, so a new or slower test must raise its budget in
the same change or fail the recipe test. Measured on nuc (8 CPUs, one
NVMe, October 25; docs/status/gate-speed-20261025.md): see that page for
the serial and 8-job times before and after the budgets.

Verdicts must be identical in serial and pooled runs. A test that fails
only in the pool belongs in the runner's `EXCLUSIVE` pattern, or takes
more slots. Do not retry it.

Together they run the suites listed in the current Makefile, including the
simulator, metadata/session/transaction, store, buffer, publication and
writeback gates. The list evolves; use `TEST_BINS` and the `test` and
`test-long` targets rather than this guide as a fixed suite inventory. Focused protocol tests
exercise logical behavior; public callbacks, real storage and RDMA require
their own acceptance gates. [Verification](verification.md) distinguishes
simulation, process restart and hardware power loss.

Everything must be green. There is no accepted-failure list.
`make check` runs `test-all`, `docs-check` and `leak-check`, and fails if
any of them fails.

## `make leak-check`

A Valgrind memcheck leak gate for unit binaries (K26). It works where
LeakSanitizer cannot: LSan needs ptrace and aborts on hosts with
`kernel.yama.ptrace_scope` 2, such as the ORCD build nodes. There the
ASan-linked tests run with `ASAN_OPTIONS=detect_leaks=0`, and this gate
does the leak check instead.

- **Output.** One `PASS <name>` or `FAIL <name>: <reason>` line per binary,
  then `VERDICT: PASS|FAIL`. A FAIL prints the first definite-loss stack.
- **Exit status.** 0 PASS; 1 on any definite leak, memory error, test
  failure, timeout or missing memcheck report; 3 with
  `NOTRUN: valgrind missing` when valgrind is absent. Never 0 without a
  complete report for every binary.
- **List.** The four tests that relied on ASan for leaks
  (`test_readindex_pair`, `test_raft_ack_window`, `test_host_outbox`,
  `test_stage_evict_lru`), plus the binaries that own the long-lived heap
  structures of the metadata path: KV (`test_kv`, `kv_lsm`,
  `kv_insertion_buffer`, `kv_snapshot_tail`, `kv_seg_index`,
  `kv_seg_bloom`, `kv_block_hash`, `kv_reader_gate`), Raft (`test_raft`,
  `raft_store`, `follower_bound`, `meta_apply`), store
  (`store_overwrite`, `store_durability`, `store_gc`, `reclaim`) and
  `txn`, `txn_resolve_faults`, `txn_retire`, `session`, `lock`. F1 (Oct 25)
  added the unit binaries of the Oct 23–25 product code: `kv_lsm_concurrent`
  (W126 readers, writer and peeks), `membership` (K33 record and discovery
  datagrams), `member_view`, `data_commit`, `put_ticket`, `put_orphan`,
  `put_orphan_server`, `append_receipt`, `frag_record`, `small_segment`,
  `packed_segment`, `pack_ledger_recovery`, `retained_namespace_audit`, and
  the Python harnesses `test_put_ticket_batch.py` (K32), `test_w155_receipt_table.py`
  and `test_session_revoke_parallel.py` (E2), which take `EFS_LEAK_WRAP`
  through `tests/sanitize_env.py`. The K36 recovery thread and the K37
  rebuild run only inside efsd: their leak check is the LeakSanitizer half of
  `make asan-check` below. Client, wire and daemon leaks on a node belong to
  `tests/run_tests.sh leaks` (`tests/valgrind_leaks.sh`).
- **Own build.** The gate copies the sources into its scratch directory
  and builds the listed binaries with a baseline `-march`. On the efs VMs
  `-march=native` emits LRCPC2 instructions that valgrind 3.26 cannot
  decode. Your build tree is not modified.
- **Verdict source.** The tested process's own memcheck report, not
  `--error-exitcode`. Memcheck would also apply that code to forked
  crash-simulation children, which `_exit` holding state on purpose.
  Forked children are therefore not leak-checked.
- **Python-driven tests** honour `EFS_LEAK_WRAP`: the gate sets it to the
  memcheck command, and the test runs its plain positive binary under it
  and skips its ASan build.
- **Suppressions.** `tests/leak_check.supp` holds third-party noise only,
  never a stack whose allocating frame is efs code. It is empty: only
  "possibly lost" libnl constructor blocks appeared, and those do not fail
  the gate.
- **Knobs.** `EFS_LEAK_ONLY="kv raft"` selects entries,
  `EFS_LEAK_TIMEOUT` sets the per-binary timeout (default 1800 s), and
  `EFS_VALGRIND` names the valgrind binary.
- **Runtime.** About 6 minutes on efs2 (4 vCPU aarch64): 24 s build and
  316 s of runs, of which `test_txn_retire` takes 203 s
  ([evidence](../status/k26-leak-gate-20261022.md)). That is short
  enough for `make check`.
- **Self-test.** `tests/test_leak_gate.py` runs under `make test`. A
  leaking fixture must FAIL, a clean one PASS, valgrind absent NOTRUN
  exit 3, and a run without a report must not pass.

## `make tsan-check` and `make asan-check`

The F1 concurrency and memory gates (`tests/sanitizer_check.sh tsan|asan`,
Oct 25). Each copies the sources into its scratch directory, builds the whole
tree (`make test-bins`) with the sanitizer in `EXTRA_DEFS` and `LDFLAGS`, and
runs a bounded list from that tree: unit binaries, the Python-driven C
harnesses, and the daemon tests, whose efsd, efs-mgmt and efs-fuse are then
the sanitized binaries. `make sanitize-check` runs both.

- **Output and exit.** One `PASS <name>` or `FAIL <name>: <reason>` line
  per entry (`<n> ThreadSanitizer reports`, `<n> AddressSanitizer errors`,
  `<n> LeakSanitizer reports`, `<n> UBSan errors`, `test rc=<n>`,
  `timeout`), the first report of a failed entry, then `VERDICT: PASS|FAIL`.
  Exit 0 PASS, 1 FAIL, 3 `NOTRUN: <mode> runtime missing` when the compiler
  cannot build and run a sanitized threaded program (no libtsan/libasan),
  2 on usage. An empty selection FAILs.
- **Verdict source.** Every sanitized process writes its reports through
  `log_path` into `<name>.tsan.<pid>` / `<name>.asan.<pid>` /
  `<name>.ubsan.<pid>`, so a report inside a daemon that a harness started
  counts too, and a daemon's own log cannot hide it; a harness binary that
  prints to stderr is read from `<name>.out` as well. TSan runs with
  `halt_on_error=0` so one entry shows all its races, `die_after_fork=0`
  (the unit tests fork crash-simulation children that open a store) and
  `detect_deadlocks=0`: libtsan's lock-order detector aborts when a thread
  holds more than 64 mutexes (the efs-fuse metadata fetch holds every
  inode lock stripe) and that abort self-deadlocks, so the tsan gate
  reports data races, not lock-order inversions. UBSan with
  `halt_on_error=1`.
- **tsan list.** KV (`test_kv_lsm_concurrent 4`, `kv_lsm`, `kv_reader_gate`),
  `raft_store`, `reclaim`, `query_disk_io`, `query_delta`, the client data
  commit, the member view, the W138 harnesses (`test_apply_ring.py`,
  `test_session_latch_tsan.py`), the E2 parallel revoke, the W155 receipt
  table and the K32 ticket batch on the TSan `libefs.a`, and the daemons:
  `test_k36_recovery.py` (recovery thread), `test_k36_segment_recovery.py`,
  `test_k37_segment_rebuild.py` (audit and rebuild) and a 60 s
  `test_d4_progress.py` (client read and write paths through efs-fuse).
- **asan list.** The four tests that always built an ASan binary, every
  harness with a sanitizer switch (`EFS_TEST_SANITIZE=1`, `EFS_SANITIZE=1`,
  `--sanitize`), the parsers and decoders (`membership`: K33 record and
  discovery datagrams; `member_view`, `frag_record`, `small_segment`,
  `packed_segment`, `packed_reader`, `pack_ledger`, `pack_ledger_recovery`,
  `append_receipt`, `put_ticket`, `put_orphan`, `put_orphan_server`,
  `data_commit`, `retained_namespace_audit`, `wire`, `kv_lsm`, `raft`), and
  the daemons: `test_env_removed.py` (the A6 removed-settings refusal),
  the K36 and K37 tests (recovery scan pages, segment audit) and the short
  D4 load. LeakSanitizer is on (Linux; it needs `ptrace_scope` < 2), so a
  clean daemon exit is leak-checked too.
- **Baseline ISA for asan.** As the leak gate: with `-march=native` on an
  AVX-512 host gcc emits 64-byte-aligned vector stack accesses (BLAKE3),
  which ASan's fake stack (`detect_stack_use_after_return`) does not give,
  and every hashing binary died with SEGV. `EFS_SAN_ARCH` overrides.
- **Harness switches.** `tests/sanitize_env.py` gives a Python harness its
  compile flags (`EFS_TEST_TSAN=1`, `EFS_TEST_SANITIZE=1`), the memcheck
  wrapper (`EFS_LEAK_WRAP`) and a scaled timeout. A harness that links
  `libefs.a` must use them: the gate's tree is built with the sanitizer.
- **Knobs.** `EFS_SAN_ONLY="kv_lsm raft_store"` selects entries,
  `EFS_SAN_TIMEOUT` (default 3600 s), `EFS_SAN_LOGDIR` keeps reports and
  the build (kept on FAIL, deleted on PASS), `EFS_SAN_BUILD` names a tree
  already built with the mode's flags for reruns, `EFS_SAN_JOBS` the build
  parallelism.
- **Self-test.** `tests/test_sanitizer_gate.py` runs under `make test`: a
  racy fixture FAILs tsan with its report shown, a heap overflow, a signed
  overflow and (where LSan runs) a leak FAIL asan, a clean threaded fixture
  PASSes, a failing test FAILs without a report, a missing runtime is
  NOTRUN exit 3, and `VAR=value` prefixes and `.py` entries work.
- **Not in `make check`.** The daemon entries make each mode take tens of
  minutes; `make check` keeps `leak-check`. F1 runs all three
  ([evidence](../status/f1-memory-concurrency-gates-20261025.md)).
- **RSS plateau (F1, fixed working set).** `tests/test_d4_plateau.py`
  (`make test-long`: a 75 s load, then the 150 s drain bound) runs the D4 loopback cluster with
  `scripts/d4_progress.py --working-set`: every writer creates its files
  before the timed load starts (every eighth 192–512 KiB, never packed; the rest 1 B–8 KiB, packed)
  and then only overwrites (`O_TRUNC`), truncates and rewrites, or rewrites
  in place a random one with a new version, while the foreground rewrites a
  ring of 1 MiB files (`--fg-set`); nothing is unlinked or created after
  that, so created == live == threads × set is asserted (no net creation).
  The driver samples every efs-fuse and efsd RSS once a second and
  `scripts/rss_plateau.py` gives the verdict per process: PLATEAU when the
  least-squares slope over the last third of the load, times its span, is
  at most 5 % of the peak. The table is
  printed on every run and enforced from a 600 s load (`EFS_D4_PLATEAU=1`
  forces it); every node's stored data (`efs-mgmt gc-status`
  data_usage_bytes) must stay bounded under the same switch: the last
  third's peak at most 25 % above the peak before it (reclaim works in
  batches, a sawtooth a slope misreads); and on every run the orphan
  ledger's abandoned uploads after the load must stay at most 5 % of the
  rewrites (an abandoned upload is collectable only when its mount's
  session ends). The F1 measurement is
  `EFS_D4_LOAD_S=3600 EFS_D4_SET=256 EFS_D4_QUOTA=8G python3 tests/test_d4_plateau.py`
  ([evidence](../status/f1-followups-20261025.md); drain within the default
  150 s since the duplicate-candidate fix, [evidence](../status/d4-pack-drain-20261025.md)).
- **Pack duplicate candidate (D4).** `tests/test_pack_dup_candidate.py`
  rewrites small files in place across a pack-epoch boundary inside the
  minimum age, so each has two candidate entries, and requires the backlog
  to reach 0 within 20 s of the files becoming quiet (rewritten every 2 s across the boundary, minimum age 10 s).

## Current release acceptance and local POSIX suites

[v0.2.0-pre-alpha AMD acceptance](../status/v020-amd-release.md) records
four modes: buffered/direct fragment I/O × TCP/RXE RDMA (buffered fragment I/O
has since been removed; the store is `O_DIRECT` only, D42). Each has 216 POSIX
passes, one unsupported writable-mmap skip, 64 peer passes and 26 cold-remount
verification passes. Full Linux `make test` passes. These are functional gates,
not power-loss durability or cross-host hardware-RoCE measurements.

The current suites are organized as:

- **POSIX**: default core filesystem checks (`posix_suite.py`).
- **POSIX concurrent**: existing two-client visibility/race checks
  (`posix_2client.py`; `posix2` remains a compatible command).
- **POSIX extras**: optional shared/private mmap and the three `.efs` command
  checks (`posix_extras.py`, or `posix_suite.py --suite extras`). Unsupported
  capabilities remain reported as SKIP in extras, not silently counted as passes.

The in-repository remote driver accepts `bash tests/run_tests.sh posix`,
`posix concurrent`, and `posix extras`; it retains its existing fcstor SSH/site
configuration. This does not rename commands in external cluster controllers.
Default core excludes all five extras; `--suite all` restores the combined
selection. Existing concurrency checks within core remain core correctness tests;
the concurrent entry point is the independent two-client suite. Historical
result tables and acceptance totals above are unchanged.

Against two already serving disposable mounts:

```bash
python3 tests/posix/posix_suite.py /mount/one --jobs 4 --timeout-s 30 --results /tmp/posix.tsv
python3 tests/posix/posix_extras.py /mount/one --jobs 4 --results /tmp/posix-extras.tsv
python3 tests/posix/posix_2client.py --local /mount/one /mount/two --parent posix-peer --results /tmp/posix2.tsv
python3 tests/posix/posix_persist.py /mount/one --phase prepare --results /tmp/persist-prepare.tsv
# Cleanly stop and remount /mount/one with the same intended transport/settings.
python3 tests/posix/posix_persist.py /mount/one --phase verify --results /tmp/persist-verify.tsv
```

`--timeout-s` is a minimum wall-clock budget per POSIX case; longer explicit
case budgets remain in effect. For Mac virtual disks with slow durable flushes,
use `--timeout-s 600` with `--jobs 1` to validate the same operations and
assertions. Use `--timeout-s 600` for each persistence phase as well. The
two-client Mac correctness profile uses `POSIX2_STEP_SEC=2400` for its
500-fsync and 800-record workloads; keep their operation counts unchanged. Record this choice with the
results, and retain failures at the original deadlines separately: a pass with
more time proves correctness within that budget, not the original latency target.
The filesystem's admission and RPC deadlines are not increased by this option.

Do not run verify before a real remount, discard failed writes to obtain a
pass, or let the remount silently change an RDMA test into TCP. Retain the
binary/source identity, daemon I/O flags, actual transport and full verdicts.
Totals evolve with the suite; a historical count is not a fixed test contract.

The site's `~/git/devops/nuc-efs` drivers select NUC/AMD, while
`~/git/cluster` controls xorinox's VMs. Those directories and the external
portal are not bundled here. The retained
[AMD matrix driver and evidence](../../results/measure/20261008-v020-amd/README.md)
document that environment; they are not a general cluster installer.

## Recorded fcstor cluster gates

The following commands describe the recorded fcstor harness, not a live
inventory or the NUC/xorinox controller. Verify its configuration and use the
correct cluster runbook before invoking setup/restart. Build on node-local
scratch rather than the NFS home (see [cluster test operations](../operations/cluster-testing.md) for
pre-flight, deployment and timeout guidance).

`bash tests/preflight.sh` first (read-only: one efsd per server, one build
ID, both Raft leaders, `commit == applied`, idle commit rate, every mount
`fuse.efs-fuse`). `tests/cluster.sh stop|start|restart [--clients] [--perf]
[--strace]` is the only way to bounce the cluster.

`tests/run_tests.sh <cmd>`:

| Command | What it gates |
|---|---|
| `setup` | rsync + build + mount the clients |
| `posix` / `posixstress` | POSIX suite 1 vs an XFS baseline, one client or N in parallel |
| `posix2` | cross-client visibility, one pair or several |
| `posixpersist` | acknowledged data survives the selected remount/process-crash scenario; does not establish hardware power-loss durability ([W71](../backlog/work-items.md#w71)) |
| `nvme` | the local `/data1/01-06` NVMe ceiling (no efs involved) |
| `meta` | `efs-bench <seed:port> --meta` cluster RPC metadata op rates; distinct from local `--bench meta` |
| `leaks` | valgrind memcheck, client and server, TCP and RDMA (unit-only gate: `make leak-check`) |

A timeout is a FAIL, not a skip.

## Honest throughput numbers

A write number counts only with the flush inside the clock: `dd bs=1M
conv=fsync` of a non-zero source (all-zero chunks skip PUTs), or fio with
`--end_fsync=1` and no `time_based`; read numbers only after a remount of
every reading client. A `time_based` fio with `--direct=1` skips the kernel
page cache but not the client's userspace dcache, so its write column
can include buffered acceptance/cache work. Confirm publication boundaries,
working-set size and post-run integrity before calling it storage throughput.
Flushing inside the clock does not independently prove target persistence
(W71). The dated references and measurement limits are in
[performance.md](performance.md) ("Baselines"). `tests/stress/fio_honest_matrix.sh` is the
fio form; `tests/measure/*.sh` hold the dd and IOR runbooks.

Before every number: `findmnt -o FSTYPE /tmp/efs-mount` must print
`fuse.efs-fuse` and `stat` of the mount must succeed. Sanity check physical fragment allocation and readback on each actual storage
root; `/data1/0*/efs` is specific to the recorded fcstor layout.

## Other harnesses

- `tests/stress/raft_host_smoke.sh` — targeted metadata-op smoke (layout
  variants, cross-group transactions). Its EXIT trap stops the cluster it
  started, so do not point it at a cluster you want to keep.
- `tests/stress/unlink_storm.sh`, `tests/stress/scale_grow.sh` — mass
  unlink and namespace-growth harnesses.
- `tests/rdma_first_inode.sh` — localhost repro for the empty-table first
  `mkdir` over RDMA.
- `tests/perf/io500/` — IOR easy/hard through the FUSE mounts (needs an MPI
  stack and the IO-500 driver; not a valid list submission).
- `tests/perf/tcp_rdma/` — paired TCP/RDMA benchmarks. Transport echo and
  the SEND baseline are separate from filesystem runs. They do not switch
  the live cluster. See `tests/perf/tcp_rdma/README.md`.
- `tests/faults/` — fault injection beside the POSIX suites. The simulator
  layer and a private 3-server FUSE cluster are separate. Repair stays a
  gap. See `tests/faults/README.md`. It is not part of `make test`.

## Profiling

`--perf` on `server.sh` or `client.sh` attaches `perf record -F 499 -g` to
the daemon. `stop` writes `flat.txt`, `by_thread.txt`, and `callers.txt`
next to the data file under `~/orcd/scratch/efs/perf/` (`efsd-<port>/` or
`<mount-name>/`).

```bash
./scripts/server.sh --perf 127.0.0.1:17432 /tmp/efs/s1:5G
./scripts/server.sh stop 127.0.0.1:17432
./scripts/client.sh --perf 127.0.0.1:17432 /mnt/efs
./scripts/client.sh stop /mnt/efs
```

On the test cluster `tests/cluster.sh restart --perf [--strace]` does the
same for the four servers (`/tmp/efs-perf/efsd.data`, `efsd.strace`);
`cluster.sh stop` SIGTERMs so the perf file gets its footer.

Attaching to a live daemon: `perf record -g -p $(pgrep -x efsd)`. A
profile taken at mount sees only the initial threads — the client's
reclaim/put pools are created at the first write, so attach after the I/O
has started. `perf record -a` is refused at `perf_event_paranoid=1`. Run
`perf report` on the node that recorded. Never restart a daemon under
`strace` and then quote the throughput; a traced daemon sits on the strace
plateau (~100 K lines/s) and the MB/s is a tracer number.

## NFS publication byte oracle (W55)

`tests/live/nfs_publication_stress.py` creates a fresh owned directory and runs
eight writers through fresh copies, overwrites, truncate/rewrite and name reuse.
It records successful close boundaries without adding fsync, compares every
operation's bytes, and saves final inode/hash expectations in a ledger. macOS
uses explicit `cp -X`; this does not validate plain cp extended-attribute support.

Run `tests/live/nfs_publication_audit.py --help` for the read-only gateway,
authoritative GETCHUNKS and three-node files-store audit. It requires node IDs
1–3 in order and checks exact generation/fragment names, not just any inode
objects. Use an independent FUSE client as `--gateway` for a second byte view.
This nonzero workload does not cover zero-data elision, storage restart or
power-loss durability. The [recorded run](../../results/measure/20261009-xorinox-w55-stress/README.md)
contains concrete commands and scope. Coordinate exclusive cluster access
before running alongside other tests.
