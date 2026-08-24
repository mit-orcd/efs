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
  nfs/
    history.tsv             # Engaging scratch NFS baseline (not mixed into perf/)
    <run_id>/
      perf-<host>.tsv
      summary.tsv
  nvme/
    history.tsv             # local /data1/01-06 NVMe ceiling (fcstor003-006)
    <run_id>/
      nvme-<host>.tsv       # per-server, per-drive, serial + parallel
      summary.tsv
  ewrite/
    history.tsv             # ewrite.sh runs (30s efs sweep + full NFS 1 2)
    <run_id>/
      ewrite-<host>.tsv     # per-host rows + script chatter
      summary.tsv           # ts host jobs wall_s bytes mib_s rc [dest]
      notes.txt             # optional: dest, compare to efs
  meta/
    history.tsv             # efs-bench --meta ops/s (read+write phases)
    <run_id>/
      meta-<host>-wN.txt    # raw bench output per worker count
      summary.tsv           # run_id host workers phase rw ops wall_s ops_s
```

## Running

From the login node (needs ssh access to the test nodes):

```bash
tests/run_tests.sh posix  fcstor007.ib            # POSIX vs XFS baseline
tests/run_tests.sh perf   single fcstor007.ib quick
tests/run_tests.sh perf   multi  quick            # all 9 pure clients
tests/run_tests.sh all                            # posix + perf multi
COMMIT=1 tests/run_tests.sh perf multi full       # + git-commit the results
tests/run_tests.sh nvme full both                 # 4-server /data1/01-06 NVMe ceiling
tests/run_tests.sh ewrite fcstor007.ib            # 30s ewrite.sh 1 2/4/8/16
tests/run_tests.sh meta fcstor007.ib              # efs-bench --meta 1/4/16 workers
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
