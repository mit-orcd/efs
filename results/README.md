# efs test results (tracked in git)

This tree records correctness + performance runs so regressions are visible in
`git log` / `git diff`. Each run gets a UTC `run_id` directory.

## Layout

```
results/
  posix/<run_id>/
    xfs-baseline.tsv        # posix_suite on XFS (node9901:/data1/efs) — the reference
    efs-<host>.tsv          # posix_suite on efs (<host>:/tmp/efs-mount)
    compare-<host>.txt      # compare.py: efs bugs = PASS on XFS, FAIL on efs
  posix2/<run_id>/          # two-client visibility, same three files per pair
  perf/<run_id>/            # honest fio matrix: raw.tsv + per-host job logs
  nvme/
    history.tsv             # local /data1/01-06 NVMe ceiling (fcstor003-006)
    <run_id>/
      nvme-<host>.tsv       # per-server, per-drive, serial + parallel
      summary.tsv
  meta/
    history.tsv             # efs-bench --meta ops/s (read+write phases)
    <run_id>/
      meta-<host>-wN.txt    # raw bench output per worker count
      summary.tsv           # run_id host workers phase rw ops wall_s ops_s
  leaks/<run_id>/           # valgrind memcheck gate output
```

## Running

From the login node (needs ssh access to the test nodes):

```bash
tests/run_tests.sh posix  fcstor007.ib            # POSIX vs XFS baseline
tests/run_tests.sh posix2 fcstor007.ib fcstor008.ib
tests/run_tests.sh nvme full both                 # 4-server /data1/01-06 NVMe ceiling
tests/run_tests.sh meta fcstor007.ib              # efs-bench --meta 1/4/16 workers
tests/run_tests.sh leaks fcstor003.ib             # valgrind gate
COMMIT=1 tests/run_tests.sh posix                 # + git-commit the results

bash tests/stress/fio_honest_matrix.sh results/perf/<run_id>-honest
```

efs throughput comes from `fio_honest_matrix.sh` only. A `--direct=1` fio
skips the kernel page cache but **not** the client's userspace dcache, so a
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
