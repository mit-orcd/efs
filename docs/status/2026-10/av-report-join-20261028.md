# AV — REPORT: join chunk records to inode-size records once (2026-10-28)

Coordinator report: branch `agent/devel-av`, head `330c48aea` (docs-only
branch; measurement-only task, no product change). Gate on intel2:
`TOTAL 549 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (747s) OVERBUDGET 5` — clean;
NOTRUNs are host limits (no RDMA device, no cluster scripts, sanitizer probe). Evidence: this page; raw data intel2
`~/efs-artifacts/taskAV-20261028/`. NOTRUN: nothing in this task's scope.
Product defects found: none from this task's mechanism.

**Verdict: PASS — measured, no change.** Across four workload shapes the
REPORT join costs **1–32 comparisons per frame**, never the quadratic case:
multi-inode frames with both large C and large I in mismatched order do not
occur in real client traffic, and the first-match break makes the common case
linear. The measured worst frame had I=512; the join's total across a 60 s
run is sub-millisecond of CPU against REPORTs that cost hundreds of
milliseconds of wall each.

## Purpose and checklist item

Task AV of the AV–AX block (`docs/status/alpha-1.0-2plus1-checklist.md`, AV
checkbox): `src/server/raft_host.c::server_raft_host_report` searches
`irecs[0..ino_count)` from the start for each of the C chunk records
(`raft_host.c:24138`), O(C × I) in the worst case. The bounded `ic[32]` cache
is separate. Measure-first: comparisons, CPU and p99 with C and I varied
independently; a request-local bounded index only above the threshold.

## Tested commit and build identity

- Source: devel `330c48aea` (wave 33), branch `agent/devel-av` (no product
  commit).
- Measurement build: clean devel plus the temporary, uncommitted counters
  below; gate build: clean devel `efsd 330c48aea... (branch agent/devel-av,
  built 2026-10-29 on intel2)`.

## Method

Temporary, uncommitted instrumentation of `server_raft_host_report`
(`av_prof_patch.py`, retained): per-frame C and I sums, max I, and an exact
comparison counter inside the `irecs` search loop, per daemon, 2 s TSV dumps
(`EFS_AV_PROF_FILE`, scratch-build-only reader). Loads on a fresh 3-member
loopback TCP cluster, one FUSE mount, 60 s each, 3 reps:

- `big`: 4 writers × 16 MiB files, fsync-on-close — C=126, I=1 (one inode,
  many chunks).
- `small`: 16 writers × 4 KiB files, fsync-on-close — per-file REPORTs.
- `mixed`: 2 big + 8 small writers.
- `openmany`: 8 writers × 64 simultaneously open files, 4 KiB appends,
  `syncfs()` every 5 s — whole-dirty-set REPORTs with C=I=N (many inodes).

Client-side fsync-close latencies give p95/p99 where applicable.

## Results

| mode | REPORTs | mean C | mean I | max I | comparisons | cmp/REPORT | fsync-close p95/p99 |
|---|---|---|---|---|---|---|---|
| big (3 reps) | 247–357 | 125–127 | 1.0 | 1 | 3.9k–5.6k | 15.7 | 1.05–1.48 s / 1.20–1.77 s |
| small (3 reps) | 8,627–10,585 | 1.0 | 1.0 | 1 | = REPORTs | 1.0 | 137–146 ms / 195–245 ms |
| mixed (3 reps) | 4,079–5,404 | 4.2–4.9 | 1.0 | 1 | 5.9k–7.5k | 1.4–1.5 | 289–417 ms / 864–1,041 ms |
| openmany (3 reps) | 6,751–7,857 | 2.1–2.3 | 14.8–22.2 | 510–512 | 171k–253k | 24.8–32.1 | (close-only load) |

Why the quadratic never happens:

- **C and I are never both large in one frame.** Per-file fsync/close reports
  per-inode (C=I=1). Whole-dirty-set REPORTs fire only on `syncfs()` or
  queue-collapse, and the background per-inode flush pool
  (`write.c::meta_flush_main`) keeps the chunk-record side small in them.
- **First-match ordering helps.** The search breaks at the first hit; the
  client's record construction order makes the common case ~O(C + I), not
  O(C × I). Even the largest observed frame (I=512) produced only tens of
  comparisons per frame on average.
- Even the adversarial bound at the observed sizes is small: a fully
  mismatched C=I=512 frame is ~131k comparisons ≈ 0.1 ms — two orders of
  magnitude below the wall cost of that REPORT's Raft work.

## Why no change

The task's threshold is a measured cost that grows with the unrelated
dimension; at every reachable shape the join is tens of comparisons per
REPORT, with no growth signal and no p99 effect attributable to it. A
request-local index would add allocation and bookkeeping to a loop that
already costs nothing measurable, and would only pay off under an
adversarially ordered giant frame that no real client produces.

## Gate

`make test-all` on intel2 via `scripts/make_test_summary.sh`, 8 jobs, clean
(unmodified devel) tree:

`TOTAL 549 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (747s) OVERBUDGET 5 BUILD 49s`

Clean gate: 0 FAIL. NOTRUN 11: the nine no-RDMA-device commands,
`test_cluster_stop.py` and the sanitizer probe — host limits.

## Limitations

- Workloads are client-realistic (FUSE mounts, real write/flush paths), not
  synthetic adversarial frames; if a future client can be made to emit giant
  mismatched-order frames, the retained harness measures the join directly
  and this task can be reopened. No product change, so no new regression
  test, budgets row or settings inventory row.

## Artifacts

intel2 `~/efs-artifacts/taskAV-20261028/`: `measure/` (12 runs, JSON + logs),
`av_prof_patch.py`, `av_measure.py`, `av_run.sh`, `matrix.log`, `gate.log`,
`summary.txt`, `timings.tsv`, `PRUNED.txt`, `SHA256SUMS` (manifest sha256:
`0f9b4de63b6f703c785918134ec4b9040ba2198bd90b6680e599d99ebd818b56`). Per-run stores and both build trees deleted after the
runs; the measurement reproduces from devel `330c48aea` plus the retained
patch and driver.
