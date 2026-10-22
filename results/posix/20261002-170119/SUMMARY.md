# posix/20261002-170119 — POSIX suite vs XFS baseline, 17:00Z redeploy gate

## Purpose and linked item

17:00Z redeploy gate run of the POSIX conformance suite against an EFS FUSE
mount, compared test-by-test against a same-suite XFS baseline. This is the
run that exposed the client lookup-memo stale-stat regression: backlog queue
row **0j** ("the client's 50 ms lookup memo returns pre-mutation stats",
`docs/backlog/work-items.md`, later given the W identity **W69**), which cites
this directory by name. The run was added by commit `ab4922cb0` ("docs: W54
fold-GC-deletes-live-base + lookup-memo rows (0i/0j)…"), whose message records
"17:00Z redeploy gate on 20745142: posix 164/201 (lookup memo stale stat, row
0j)". Row 0j attributes the regression to client commit `20745142a` (LOOKUP row
answers the following GETATTR within 50 ms, so a stat right after a
write/chmod/link/utimens on the same client returns pre-mutation attributes).

(Assigned in the summary batch as `posix2/20261002-170119`; the actual run
directory is `results/posix/20261002-170119`.)

## Verdict

**FAIL** — not an unqualified success: 36 of 201 tests pass on the XFS
baseline but fail on EFS (`compare-fcstor008.txt` verdict line: "not an
unqualified success"). 161 tests pass on both, 3 pass only on the target
(virt_* checks), 1 target SKIP (`mmap_write_read`, mmap unsupported).

## Source commit and build identity

Not recorded in the run (no identity/version files). The linked docs tie this
gate to client commit `20745142a` (with `efc0f499cbae` also named in the
adding commit's message); the immediately preceding 15:19Z tree had scored
200/201 (row 0j).

## Environment

- Target: host `fcstor008`, EFS FUSE mount `/tmp/efs-mount`, suite header
  timestamp 2026-10-02T17:02:01Z (`efs-fcstor008.tsv`).
- Baseline: XFS on `node9901`, mount `/data1/efs`, 2026-10-02T17:01:34Z
  (`xfs-baseline.tsv`).
- Concurrency: jobs=1 (per work-items row 0j).
- Cluster topology, transport, buffered/direct I/O: not recorded in the run.

## Method

- Suite: `tests/posix/posix_suite.py <mount-dir> --results <tsv> --jobs 1`
  (core suite; `--suite extras` covers optional mmap / .efs checks).
- Baseline: same suite against XFS, then
  `tests/posix/compare.py xfs-baseline.tsv efs-fcstor008.tsv >
  compare-fcstor008.txt` (exit 1 when any evaluated pair is not equivalent;
  "EFS-BUG" = baseline PASS, target FAIL).
- Cleanup: suite default (no `--keep` artifacts present).

## Results

| Metric | Count |
|---|---|
| Tests compared | 201 |
| Both pass | 161 |
| EFS bugs (baseline PASS, target FAIL) | 36 |
| Both fail (matching) | 0 |
| Target better | 3 (`virt_find_not_a_real_dir`, `virt_find_query`, `virt_stats_readable`) |
| Target skipped | 1 (`mmap_write_read` — mmap unsupported) |
| Incomplete | 0 |

Failure pattern (paraphrased from `compare-fcstor008.txt`): the 36 failures
are dominated by stale-attribute symptoms — post-write sizes read as 0 or the
old value (`basic_dd_rw`, `content_*`, `sparse_*`, `fdatasync_write`,
`opt_fallocate`), old modes after chmod/fchmod (`attr_chmod`, `perm_fchmod`,
`perm_suid_sgid_stored`), old mtimes (`attr_utimens_ns`,
`chmod_preserves_mtime`), and old nlink after link/mkdir (`hardlink_basic`,
`dir_nlink`) — consistent with the single 0j root cause (50 ms lookup memo
serving pre-mutation stats) rather than 36 independent bugs. Baseline for
comparison: the 15:19Z tree scored 200/201 (row 0j).

## Limitations and remaining gates

- No build identity in the run; the commit linkage comes from docs, not the
  raw data.
- mmap checks not exercised (target SKIP).
- Row 0j records an Oct 5 implementation checkpoint (memo cleared on mutation
  entry/exit, mutation-serial guard) with local gates passing; the remaining
  gate at that time was Linux posix jobs=1 back to the 200/201 baseline plus
  a Spark du performance measurement.

## Evidence index

| File | sha256 (16) |
|---|---|
| compare-fcstor008.txt | 3610c169b85d53ed |
| efs-fcstor008.tsv | f54622106eddaf52 |
| xfs-baseline.tsv | b66d0c1c1106230f |
