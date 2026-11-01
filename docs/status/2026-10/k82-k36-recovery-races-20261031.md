# K82 — TSan data races in `k36_recovery` (Task BV)

**Verdict: FIXED (scoped), blocks alpha: no.** Purpose: K82, found by Task BR's batch F rehearsal
([finding 1](batch-f-rehearsal-20261031.md)). Acceptance still owed: batch F lane F2 on the candidate.

| Check | Result |
| --- | --- |
| `EFS_SAN_ONLY=k36_recovery bash tests/sanitizer_check.sh tsan`, unfixed base `bc2af2bf8c88` (devel), 4 runs | **FAIL 4 of 4** (2 to 7 reports per run) |
| Same lane, fixed tree `e09360033f68`, 3 runs | PASS 1, FAIL 2: both failures are one report on `rpc_prof_enabled` (K83, `inode_rpc.c:160`), none in K82's code |
| Same lane, fixed tree plus a one-line K83 fix on the host only (not committed), 3 runs | **PASS 3 of 3** |
| `make_test_summary.sh` (`make test-all`, 8 jobs), fixed tree, 2 runs | **FAIL 3 and FAIL 4, all host-limited, not 0 FAIL** (below); `test_d4_plateau` failed once (EIO under pool load) and passed in the rerun and 3 of 3 alone |

## What the races were

All three are lock-discipline bugs, not logic bugs; each needed two threads in one process.

1. **`inode_at` (`src/meta/metadata.c`, efs-fuse; 8 of the 12 reports).** `inode_at` is a getter
   that wrote on every call: a shared LRU tick (`ino_slabs[si].tick = ++ex->ino_slab_tick`) and the
   row's `slab_idx` stamp. Callers hold different locks (`efs_export_present_add` from the PUT
   workers' `efs_export_set_chunk`; `efs_export_present_count` from `inode_allocated_bytes` under
   the dir lock and `idx_mu`), so two threads wrote the same words. The tick was never read
   (`grep` of `ino_slab_tick` and `.tick` on slabs finds only the two writes), so it is deleted
   with its two fields; the idempotent `slab_idx` stamp stays and is now a relaxed atomic store.
2. **`efs_export_set_chunk` (`metadata.c:3498`) and `chunk_idx_get`/`idx_init` (efs-fuse).**
   The reader was `inode_allocated_bytes` → `file_chunk_present` → `efs_export_get_chunk`, which
   probed the tail chunk after releasing the dir lock and `idx_mu` it had taken for the count,
   while a PUT worker rebuilt the chunk index (`idx_init`) and appended in `set_chunk`. The seek
   path (`efs_fuse.c` SEEK_DATA/HOLE) already holds both locks around the same call; the stat
   path now does too (`efs_fuse.c`, the `tail` probe).
3. **`server_join_cluster` (`src/server/cluster.c:35`) against `server_recovery_drop`
   (`store.c:2219`).** The HELLO read `local->used` without `s->lock`; the K36 recovery scan
   debits it under `s->lock`. The read now takes `s->lock` around `server_local_node` and the
   read only; `server_compute_local_usage` (a directory walk) stays outside the lock. All three
   callers (`efsd.c:1468`, `cluster.c:761`, `store.c:3511`) call it without `s->lock` held.
   **Not reproduced on this host** in 4 base runs and 6 fixed runs; Task BR saw it once. The
   fix follows from the reported pair and the lock the writer uses; its verdict is by inspection.

## Which change since Oct 25

No single commit wrote the racy lines; every one predates Oct 25. By `git blame`/`git log` only
(no bisect runs):

- `inode_at` stamp: Sep 1 (`b3275d22e`), tick: Aug 31 (`1d7d5822b`); `efs_export_present_count`
  and its counter Oct 5 (`9944efb02`); the unlocked tail probe in `inode_allocated_bytes` Sep 28
  (`54a500da9`); `server_join_cluster`'s unlocked `->used` read Oct 23 (`d7bca79d6`), the locked
  debit in `server_recovery_drop` Oct 24 (`e8d25cc22`, the K36 recovery itself).
- What changed the interleaving since Oct 25 are candidates, not proven: `5a820f91c` (Oct 28, mixes
  allocation bits into the dcache buckets and I/O locks, so inodes that shared a lock no longer
  do) for the two client races; `d9186e38a` (Oct 25, a join interrupted before its store identity
  is retried), `381d65125` (Oct 29, rejoin thread joined at teardown) and the Oct 26-27 shutdown
  and heartbeat commits for the server read, which only races when a rejoin overlaps the scan.
  The `test_k36_recovery.py` file has no change since Oct 24. An A/B on the Oct 25 tree would settle it;
  none was run.

## Fix

Commit `e09360033f68` on `claude/bv-k82-tsan-k36-d6efee`, four files: `include/efs/metadata.h`
(two dead fields removed), `src/meta/metadata.c`, `src/client/efs_fuse.c`, `src/server/cluster.c`.
No suppressions, no sleeps, no `no_sanitize`.

## Environment and reproduction

amd (AMD Ryzen AI MAX+ 395, 32 threads, gcc 15.2, libtsan, Python 3.14.4), `git archive` copy under
`~/efs-taskBV/{base,src}`, `TMPDIR=~/efs-taskBV/tmp`, `EFS_GIT_ID` set to the commit, TCP loopback,
buffered I/O, the lane's own fixtures. Lane (each ~60-75 s plus a 19 s TSan build):

```
git archive <commit> | tar -x -C ~/efs-<run>/src && cd ~/efs-<run>/src
EFS_SAN_LOGDIR=<dir> EFS_SAN_ONLY=k36_recovery bash tests/sanitizer_check.sh tsan   # PASS or N reports
TMPDIR=<xfs-or-any> EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh                # about 16 min
```

Expected: lane `PASS k36_recovery`; `VERDICT: PASS`. Cleanup: `~/efs-taskBV/{src,base,tmp}` removed.
The K83 overlay used for the 3 of 3 run replaced the two-line body of `rpc_prof_enabled` with a relaxed
atomic load and store of `efs_rpc_prof_on`; K83's own session owns that fix.

## Gate

Run 1: `TOTAL 583 FAIL 3 NOTRUN 3 STATUS FAIL (965s) OVERBUDGET 3`; run 2: `TOTAL 583 FAIL 4 NOTRUN 3
STATUS FAIL (905s) OVERBUDGET 4`. Failures: `test_stop_progress` (both runs), `test_settings_listing`
(both), `test_k42_peer_stop_open` and `test_k54_member2_unmount` (run 2): `fusermount3 -u` busy or
efs-fuse not exiting after it, the gvfs volume monitor fault of batch F fault 3 (9 gvfs processes on
amd). All four also fail alone on the unmodified base `bc2af2bf8c88` (2 of 2 each), so none comes
from this change. `test_d4_plateau` (run 1): `load rewrite ... EIO`, once, passed in run 2 and 3 of 3
alone; not attributed (pool load, the K81/`pump_logging` family is the nearest suspect). NOTRUN: cluster
scripts absent, RDMA hardware, XFS. **The "0 FAIL" gate line was not reached on amd**; it needs a
headless gate host (intel2 ran wave 47 at FAIL 0). No test was added to the gate.

## Raw captures

`~/efs-artifacts/taskBV-20261031/` on amd (reports `*/k36_recovery.tsan.*`, lane outputs, both gate
summaries, solo and base reruns; build trees removed), `SHA256SUMS` sha256
`7168a77051767534b221da520ab9c439dc4e7eafaa7c207ce9aaf020254cf31c`.

## Limits

`server_join_cluster` race unreproduced; attribution by history only; gate not 0 FAIL on this host;
`make test` could not build on macOS (`fopencookie` in `log_ts.c`), so local iteration was not possible.
