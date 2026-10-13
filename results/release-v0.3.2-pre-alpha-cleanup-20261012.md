# Release cleanup validation — 2026-10-12

Verdict: **PASS** for the scoped dead-code/profiler cleanup. This is not a new
cluster POSIX, storage durability, RDMA or performance gate.

Source: `release-v0.3.2-pre-alpha`, base
`3a41493591dcede820700db7d934da9dc10f07ee`, plus the code changes in the commit
containing this summary. Private Linux binaries identify as `3a414935-cleanup`.

Removed the production-unreferenced `host_append_complete_batched` and three
unused internal wrappers: `efs_client_rpc_query_snapshot_v3`,
`efs_client_kick_meta_flush`, and `efs_dcache_replay_stale`. Removed tests only
for the orphan helper; active append prune/close cohort tests remain. Historical
APPEND_COMPLETE_ATOMIC apply/verdict handling remains and its regression passes.

Shared perf/strace launch code now lives in `src/common/recorder_start.h`.
Each caller retains its recorder PID, launch timing and shutdown policy; FUSE
launches remain after daemonization. Existing command arguments and optional
literal `EFS_STRACE_EXPR` are preserved. Invalid output parents now fail before
fork; existing regular files cannot masquerade as directories. New regression
coverage uses real fork/exec with temporary fake executables and checks argv,
filters, paths with spaces, directory creation, exec failure and invalid paths.

## Reproduction and results

Prerequisites: Python 3, C compiler; full Linux build requires the normal EFS
build dependencies (including libfuse3 and libibverbs development packages).
Run from the repository root:

```sh
make -j4 all EFS_GIT_ID=3a414935-cleanup EFS_GIT_DESCRIBE=3a414935-cleanup EFS_GIT_BRANCH=release-v0.3.2-pre-alpha
make test-client-memory EFS_GIT_ID=3a414935-cleanup EFS_GIT_DESCRIBE=3a414935-cleanup EFS_GIT_BRANCH=release-v0.3.2-pre-alpha
python3 tests/test_recorder_start.py
python3 tests/test_recorder_stop.py
python3 tests/test_append_cohort.py
python3 tests/test_append_prepare_batch.py
python3 tests/test_gc_verdict.py
EFS_TEST_SANITIZE=1 python3 tests/test_recorder_start.py
EFS_TEST_SANITIZE=1 python3 tests/test_append_cohort.py
python3 docs/check-architecture.py
git diff --check
```

- Full `make all`: PASS, zero compiler warnings, Fedora aarch64 Mac VM `efs1`.
- `test-client-memory`: PASS on that Linux VM.
- Five focused Python tests above: PASS on Linux.
- Recorder launch/stop, append cohort, FUSE workers, client memory and retained
  publication/writeback regressions: PASS on macOS.
- ASAN/UBSAN recorder launch and append cohort tests: PASS on macOS (symbolizer
  availability warnings only).
- Architecture checks: PASS, 5/5; whitespace check: PASS.

The failed-exec cases intentionally print missing perf/strace diagnostics.
An initial new test exposed acceptance of a regular-file parent; fixed and
rerun successfully. No other test failures remain in this scoped run.

Environment: one local Tart Fedora VM used only for private compilation/tests;
no daemon, FUSE mount, transport, I/O mode, quota or storage workload was started.
Installed cluster sources/binaries/stores were not deployed or modified. The VM
was initially stopped and is stopped again after testing. Tests use temporary
fixtures and clean them on completion. Raw build/client logs and private source
fixture are retained outside Git under
`efs1:/data1/efs-unit-tests/release-cleanup-*`; no raw artifacts are committed.

Limitations: full `make test`, live POSIX suites and real perf/strace attachment
were not run. The full build verifies callers link without the removed wrappers;
the deterministic fork/exec tests verify launcher behavior independently of
host perf permissions. Benchmark workloads/harness and performance policy were
not changed.
