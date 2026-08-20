# efs test results (tracked in git)

This tree records correctness + performance runs so regressions are visible in
`git log` / `git diff`. Each run gets a UTC `run_id` directory.

## Layout

```
results/
  posix/<run_id>/
    xfs-baseline.tsv        # posix_suite on XFS (node9901:/data1/efs) — the reference
    efs-<host>.tsv          # posix_suite on efs (<host>:/tmp/efs/mnt)
    compare-<host>.txt      # compare.py: efs bugs = PASS on XFS, FAIL on efs
  perf/
    history.tsv             # one line per (run_id, suite, test): summed bw + #hosts
    <run_id>/
      perf-<host>.tsv       # per-node dd + fio rows
      summary.tsv           # all nodes merged
```

## Running

From the login node (needs ssh access to the test nodes):

```bash
tests/run_tests.sh posix  fcstor007.ib            # POSIX vs XFS baseline
tests/run_tests.sh perf   single fcstor007.ib quick
tests/run_tests.sh perf   multi  quick            # all 9 pure clients
tests/run_tests.sh all                            # posix + perf multi
COMMIT=1 tests/run_tests.sh perf multi full       # + git-commit the results
```

`tests/posix/posix_suite.py <dir>` and `tests/perf/perf_node.sh <mnt> <out>`
are self-contained and can also run standalone on any node.

## Baseline semantics (POSIX)

The XFS run is the source of truth for "correct" behaviour:
- **EFS BUG**   = PASS on XFS, FAIL on efs  → a real efs correctness gap.
- **both fail** = FAIL on both              → matches a real fs; not a bug.
- **target++**  = FAIL on XFS, PASS on efs  → efs is stricter (review).

## TSV formats

posix: `test \t result(PASS|FAIL|SKIP) \t detail`
perf:  `ts \t host \t suite(dd|fio) \t test \t bw_mib_s \t iops \t rc`
