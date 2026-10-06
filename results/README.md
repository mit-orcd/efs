# efs test results (tracked in git)

This tree records the correctness and performance runs that a live document
cites, so a number in a doc can be checked against its raw files. Each run
gets a UTC `run_id` directory.

## Retention

Keep recent correctness gates, current investigations, reference baselines,
and runs cited by active documentation, tests or scripts. An uncited recent
report can remain while it supports work in progress. Old failed setup attempts,
superseded runs and repetitive debug captures can be removed once their useful
findings are recorded. Generated documentation and history-only citations do not
require indefinite raw-log retention. When pruning a cited run's raw files, keep
its summary and explicitly identify the evidence that was removed.

Oct 6 2026 cleanup removed two uncited older POSIX2 runs, an unused server-perf
attempt, three superseded W23 setup attempts, and the W50 pass-3 leader logs and
derived identity dumps. W50's summary, counts, analyses and earlier logs remain.
Current Oct 6 gates/reviews, XFS references and cited performance results remain.
Removed tracked evidence is recoverable from Git before this cleanup; this does
not rewrite repository history. Cursor runtime logs are ignored separately.

## Layout

```
results/
  posix/<run_id>/
    xfs-baseline.tsv        # posix_suite on XFS (node9901:/data1/efs) — the reference
    efs-<host>.tsv          # posix_suite on efs (<host>:/tmp/efs-mount)
    compare-<host>.txt      # compare.py: efs bugs = PASS on XFS, FAIL on efs
  posix2/<run_id>/          # two-client visibility, same three files per pair
  posixpersist/<run_id>/    # durability across unmount/remount
  io500/<run_id>-<transport>/   # IO-500 debug runs: result_summary.txt, per-phase txt/csv, launch/preflight
  measure/<run_id>-<name>/  # tests/measure/*.sh runbooks: SUMMARY.txt + raw files
  stress/<run_id>-<name>/   # same_parent_storm, unlink_storm, N-1 gates
  perf/<run_id>/            # honest fio matrix: raw.tsv + per-host job logs
  leaks/<run_id>/           # valgrind memcheck gate output
  nvme/
    history.tsv             # local /data1/01-06 NVMe ceiling (fcstor003-006)
    <run_id>/
      nvme-<host>.tsv       # per-server, per-drive, serial + parallel
      summary.tsv
  meta/                     # efs-bench --meta ops/s; recreated by run_tests.sh meta
```

## Running

From the login node (needs ssh access to the test nodes; anything over
10 s goes through `efs-bg.sh start`):

```bash
tests/run_tests.sh posix  fcstor007.ib            # POSIX vs XFS baseline
tests/run_tests.sh posix2 fcstor007.ib fcstor008.ib
tests/run_tests.sh nvme full both                 # 4-server /data1/01-06 NVMe ceiling
tests/run_tests.sh meta fcstor007.ib              # efs-bench --meta 1/4/16 workers
tests/run_tests.sh leaks fcstor003.ib             # valgrind gate
COMMIT=1 tests/run_tests.sh posix                 # + git-commit the results

bash tests/measure/dd_wall.sh                     # 1/4/9-client dd+fsync wall
bash tests/stress/fio_honest_matrix.sh results/perf/<run_id>-honest
```

A write number counts only with the flush in the clock (`dd conv=fsync`,
fio `--end_fsync=1` without `time_based`). A `--direct=1` fio skips the
kernel page cache but **not** the client's userspace dcache, so a
`time_based` run without `end_fsync` measures memory bandwidth, not efs.

`tests/posix/posix_suite.py <dir>` is self-contained and can also run
standalone on any node.

## Baseline semantics (POSIX)

The XFS run is the source of truth for "correct" behaviour:
- **EFS BUG**   = PASS on XFS, FAIL on efs  → a real efs correctness gap.
- **both fail** = FAIL on both              → matches a real fs; not a bug.
- **target++**  = FAIL on XFS, PASS on efs  → efs is stricter (review).

## TSV formats

posix: `test \t result(PASS|FAIL|SKIP) \t detail`
nvme:  `ts \t host \t suite(dd|fio) \t test \t bw_mib_s \t iops \t rc`
