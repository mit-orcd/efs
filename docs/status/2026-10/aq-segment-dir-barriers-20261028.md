# AQ — segment directory barriers through the existing sync group (2026-10-28)

Coordinator report: branch `agent/devel-aq`, head `9c4bfd171ccc5450ae852a2597351150e543730a`
(docs-only branch; measurement-only task, no product change). Gate on intel2:
`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (809s) OVERBUDGET 3` — the one FAIL is
the already-registered K55 (`test_b6_unavailable.py` slow mode), which fails
identically on the untouched base (this gate *is* the base: the branch carries
no product change; 4th occurrence, third host). Evidence: this page; raw data
intel2 `~/efs-artifacts/taskAQ-20261028/`. NOTRUN: none beyond the gate's usual
host limits (no RDMA device, no valgrind, no cluster scripts). Product defects
found: none from this task's mechanism; K55 is pre-existing on devel. No
product change.

**Verdict: PASS — measured, no change.** Same-directory in-flight overlap of
segment directory fsyncs was **0.20–1.27 %** across six full-rate packer runs
(threshold: 10 %) and **0 %** across three K37 rebuild runs. Sharing barriers
through `directory_sync_group_run` would batch essentially nothing.

## Purpose and checklist item

Task AQ of the Stage 1 server-disk-I/O block (AP–AR): `sg_write` in
`src/server/segment_store.c` fsyncs each new segment fragment file and then
`sg_sync_path` opens/fsyncs/closes its directory; `sg_mkdirs` syncs parents of
new levels. Ordinary fragments share same-directory barriers through
`directory_sync_group_run`; segment directories do not. The task warns that
segment buckets spread files across directories, so overlap may be rare —
measure before building. Checkbox: AQ.

## Tested commit and build identity

- Source: devel `9c4bfd171ccc5450ae852a2597351150e543730a`, branch
  `agent/devel-aq` (no product commit; this page plus checklist/README).
- Measured build: instrumented `tests/faults/efsd` of the same commit built
  2026-10-29 on intel2 (see Method). Gate build: `efsd
  9c4bfd171ccc5450ae852a2597351150e543730a (branch agent/devel-aq, built
  2026-10-29T19:42:14Z on intel2)`.

## Host and configuration

intel2 (8 CPUs, NVMe), quiet before each stage. Pack runs: fresh 3-member
loopback TCP cluster per run (`tests/k33_daemons.py`, instrumented faults
daemon, `EFS_RAFT_OBS=1`, quota 4G, stores under `$HOME/efs-taskAQ/tmp`),
packing on `EFS_PACK_MIN_AGE_MS=1000 EFS_PACK_INTERVAL_MS=200`, one FUSE mount,
8 or 32 writers creating 4 KiB files with fsync for 90 s plus a 10 s pack
drain. Rebuild runs: `python3 tests/test_k37_segment_rebuild.py` (rc=0 on all
three).

## Method (measurement)

Temporary, **uncommitted** profiler in `segment_store.c` (transformer and
drivers retained in the artifacts): every `sg_sync_path` directory fsync logged
with the path and `CLOCK_MONOTONIC` start/end timestamps, the `sg_write`
fragment-file fsync logged likewise, each `sg_mkdirs` level logged. Per-daemon
line-buffered files (`EFS_SG_PROF_FILE.<pid>`, a scratch-build-only reader).
No strace: per-syscall tracing would distort exactly the timing under study.

**Overlap**, the decision number: the fraction of directory fsyncs that start
while another fsync on the *same directory in the same daemon* is still in
flight — precisely the arrivals `directory_sync_group_run` could absorb into
one barrier. Sequential calls on one directory cannot be batched by design
(only mutations completed before a batch's barrier starts may share it), so
in-flight overlap is the whole opportunity.

## Results

| run | files written | dir fsyncs | dir fsyncs/s | overlap | distinct dirs | dir fsync p50/p99 |
|---|---|---|---|---|---|---|
| pack-w8-r1 | 12,940 | 2,010 | 22.3 | 0.20 % | 108 | 1 µs / 5.2 ms |
| pack-w8-r2 | 11,884 | 1,269 | 13.9 | 0.47 % | 108 | 1 µs / 6.2 ms |
| pack-w8-r3 | 11,869 | 1,275 | 14.0 | 0.78 % | 108 | 1 µs / 6.0 ms |
| pack-w32-r1 | 19,022 | 2,142 | 22.9 | 0.33 % | 108 | 1 µs / 7.2 ms |
| pack-w32-r2 | 12,144 | 1,422 | 15.7 | 1.27 % | 108 | 1 µs / 6.6 ms |
| pack-w32-r3 | 19,741 | 2,037 | 21.3 | 0.25 % | 108 | 1 µs / 7.2 ms |
| rebuild-r1 | (K37 PASS) | 125 | 0.6 | 0 % | 42 | 972 µs / 5.9 ms |
| rebuild-r2 | (K37 PASS) | 85 | 0.5 | 0 % | 32 | 1.1 ms / 6.9 ms |
| rebuild-r3 | (K37 PASS) | 95 | 0.5 | 0 % | 36 | 936 µs / 6.3 ms |

Interpretation:

- Even at the highest measured packer rate the cluster produces only ~14–23
  directory fsyncs/s, and almost none of them are concurrent on one directory:
  the three fragments of a segment are written sequentially by one job, and the
  4096-bucket directory fan-out spreads concurrent jobs. The median directory
  fsync is 1 µs (nothing pending); p99 is 5–7 ms. Rebuilds produce 0.5/s.
- The file fsyncs, not the directory fsyncs, dominate pack/rebuild I/O time
  (file fsync mean 2.4–5.4 ms, ~2,000 per 90 s run at 32 writers).
- mkdir parent syncs happen only while the bucket tree is first populated
  (~105 levels on a fresh cluster; steady state creates none).

So `directory_sync_group_run` is **not** adopted for segment directories: the
overlap it exists to exploit is 0–1.27 %, an order of magnitude below the task's
10 % threshold, and the median directory fsync is already free. No fsync is
removed, no cache, no delay — the code stays exactly as it is.

## Gate

`make test-all` on intel2 via `scripts/make_test_summary.sh`, 8 jobs, clean
(uninstrumented) tree of the same commit:

`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (809s) OVERBUDGET 3 BUILD 33s`

The one FAIL is `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py` with
the K55 signature (`slow: fsync refused: {'F0': ('EIO', 60.06), ...}`), already
registered as an OPEN base defect (blocks alpha: yes). **Base comparison:**
this branch carries no product change (the deployed tree is `git archive` of
unmodified devel `9c4bfd171`), so this gate is itself a base run; the identical
failure was recorded by the intel1 and xorinox gates and a solo rerun (now
4/4 across three hosts). NOTRUN 11: the nine no-RDMA-device commands,
`test_cluster_stop.py` (no `~/git/cluster`), `test_sanitizer_gate.py` (probe
NOTRUN): host limits.

## Limitations

- Loopback TCP on intel2 NVMe only; a slower device (node9901's disks) would
  lengthen barrier windows and could raise overlap, but with 14–23 dir fsyncs/s
  and p50 of 1 µs there is no plausible path to 10 % from these rates. If AQ is
  ever revisited on a real-host packer run, the same driver and profiler apply.
- No product change, so no new regression test, no budgets row, no settings
  inventory row; the durability invariants (every directory barrier kept,
  ancestry ordering, fail-closed) are untouched.

## Artifacts

intel2 `~/efs-artifacts/taskAQ-20261028/`: `measure/*.json` (one per run),
`measure/*.log`, per-daemon `*-prof.<pid>` fsync timelines, `matrix.log`,
`aq_prof_patch.py`, `aq_measure.py`, `aq_run.sh` (temporary tooling),
`gate.log`, `summary.txt`, `timings.tsv`, `SHA256SUMS` (manifest sha256:
`353354f53e06ac574740036c1a4f6578897116153d41647bf6f21144ae0bae33`). Scratch trees deleted after the gate; the measurement is
reproduced from devel `9c4bfd171` plus the retained patch script.
