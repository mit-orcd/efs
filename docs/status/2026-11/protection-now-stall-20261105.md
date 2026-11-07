# `test_k38_protection_now` waited 30 s under gate load (Task DP)

**Verdict: PASS. The cause was in the test, not a product defect, so there is no K row.** Step 2 waited out the packer's 30 s back-off after a degraded write. Under gate load its batch of five small files splits over two packer passes. The first pass writes its segment degraded, as the step forces, and the group's packer then pauses for `EFS_PACK_FAIL_BACKOFF_MS` (30 s by default). The test waits for all five files to be packed. The test now sets the back-off to 2 s, as `test_k37_segment_rebuild.py` and `test_b6_unavailable.py` already do. With 7 neighbour workers on node9900, 4 of 20 runs stalled before the fix (50.4 to 53.5 s) and 0 of 20 after (21.3 to 26.0 s). The after loop still split the batch in 3 runs, and each split cost 2 s instead of 30 s. 8-job gate on fstor007: `TOTAL 591 FAIL 0 NOTRUN 3`, `test_k38_protection_now` 23.6 s.

Purpose: Task DP, the 52.5 to 53.4 s `test_k38_protection_now` runs in both wave 58 first gates (budget 30 s) and Task DJ's 51.7 s. Tested source: devel `cb51d1cd6` plus the test change on this branch (`git archive` builds, so the build id reads `unknown`). No daemon, client or library source changed.

Environment: node9900 (192 cpus, `/data1/efs/deps/libfuse`) for the loops; fstor007 (96 cpus, private libfuse shim) for `make test` and the gate. Private loopback clusters of three members (EFS_FAULTS `efsd`, the RDMA transport in auto mode on mlx5_0 as the logs show, buffered I/O, 1 GiB quota per member) under a private TMPDIR, free ports, one mount through node 1. Both hosts were otherwise idle (load 1 to 4).

## The step and the trace

The new per-phase marks (`[t s] phase`, also written to `EFS_K38_TIMES`) show where the time went in the four slow runs. It was step 2's `wait_packed` for the refused segment PUT: 32.2 s every time, against 2.1 to 2.3 s in the other 16 runs (both figures include the step's 1.5 s sleep). Every other phase matched the fast runs.

Run 5 (52.5 s), node 1's log (group 0 leader, node 3 refusing segment PUTs):

```
12:08:47.280Z packer: segment g0 id=e5736a5ed7ce5d01 gen=1 written degraded: fragment 2 for node 3 not acknowledged (rc=-1); marked for rebuild on return
12:08:47.281Z packer: group=0 term=1 ... candidates=5 eligible=1 segments=1 unindexed=1 packed_total=11 ... pass_ms=9
12:09:17.338Z packer: segment g0 id=e573715e01a69601 gen=1 written degraded: fragment 2 for node 3 not acknowledged (rc=-1); marked for rebuild on return
12:09:17.338Z packer: group=0 term=1 ... lag_s=30 ... candidates=4 eligible=4 segments=1 unindexed=4 packed_total=15 ... pass_ms=19
```

Runs 6, 8 and 11 show the same pattern: run 6 split 3 + 2 files (12:09:40.24, then 12:10:10.30 with `lag_s=30`). Only one of the five files had passed the 500 ms minimum age when the first pass ran. That pass wrote its segment degraded, and `src/server/packer.c` sets `backoff_until_ns = now + PK_FAIL_BACKOFF_NS` after every degraded or failed upload. The next pass therefore came 30 s later. It took 19 ms, so nothing was blocked; the packer was waiting on purpose. In the fast runs all five files became eligible in one pass, which made one degraded segment and the next phase started at once. Load stretches the five writes across the minimum-age boundary, which is why the stall appeared only under gate load (wave 58 had `test_k35_fsync_degraded` failing beside it). Waves 59, DH's own gate and the quiet pairings did not happen to split the batch.

This was not the peer-pool receive timeout (`EFS_IO_TIMEOUT_MS`, also 30 s). No receive timed out in the slow window. The RDMA connections closed at 12:09:17.3 were idle pool connections reaped after their 30 s idle limit (`EFS_PEER_CONN_IDLE_MS`) once the packer used the pool again.

## The change

`tests/test_k38_protection_now.py`:

- Sets `EFS_PACK_FAIL_BACKOFF_MS='2000'`. The setting's own comment in `packer.c` says "tests that run with a member down shorten it", and the two sibling tests that refuse or stop a member already do. This test refuses a member in step 2 and stops one in step 3, but it lacked the setting. This shortens a test pause; it raises no timeout, budget or bound.
- Prints elapsed seconds per phase and appends them to `EFS_K38_TIMES` when that is set, so a passing gate run still records where the time went.

Every claim of the test is unchanged: step 2 still requires a `written degraded` segment, `Degraded segments: >= 1` and `Protection: DEGRADED` on every member under the hold, then a rebuild to `full`. The daemon's back-off is unchanged.

## Results

| Check | Before (`cb51d1cd6` test, phase marks only) | After (with the back-off setting) |
|---|---|---|
| 20 runs beside 7 neighbour workers, node9900 | 4 of 20 over 30 s: 50.4, 51.5, 52.5, 53.5 s; the other 16 ran 21.2 to 24.9 s | 0 of 20 over 30 s: 21.3 to 26.0 s, median 23.3 s |
| Step 2 `wait_packed` (includes the 1.5 s sleep) | 2.1 to 2.3 s, or 32.2 s in 4 runs (batch split) | 2.1 to 2.3 s, or 4.1 s in 3 runs (batch split, 2 s pause) |
| Neighbour runs (load.tsv) | all rc 0 | all rc 0 |

- Gate (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, `make test-all`), fstor007, after: `TOTAL 591 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (667s)`. The NOTRUNs are cluster scripts absent, Memcheck header absent and RDMA hardware, the same as in the wave 59 gate. No over-budget row. `test_k38_protection_now` 23.6 s.
- `make test`: rc 0 in 139 s on fstor007.
- TSan: not run, because no daemon code changed.
- Budget row: unchanged at 30 s. Slow-free medians are about 22 to 23 s, and 1.25 times that rounds up to the same 30 s. No three-gate refit was run.

## Limits

- One host for the loops (node9900) and one gate (fstor007). The fstor007 wave 58 stall is the same signature by timing (53.4 s, the same +30 s), but its daemon logs were not kept.
- The daemon's policy is unchanged: after one degraded segment the group's packer pauses 30 s, even when the refusing member answered and the segment is durable on two fragments. Whether a degraded write that was marked should pause the whole group is a throttle-policy question, not decided here.
- Seen, not traced: step 3's wait for `full` after the member returns takes 11 to 12 s in every run, solo or loaded (the chunk rebuild after rejoin), well inside the budget.

## Raw data

On the Mac under `~/efs-artifacts/taskDP-20261105/`: `node9900-loops.tar.gz` (before and after `runs.tsv`, `times.txt`, per-run logs, `load.tsv`, and the four slow runs' daemon and mount logs) and `fstor007-gate1.tar.gz` (gate `summary.txt`, `timings.tsv`, the gate's k38 phase marks, the `make test` log). `SHA256SUMS` sha256: `91bfbe12effe0da969f54fbf37a3f36fe585d34a1fe94ce5ea8fd2a8f5e60e31`. The `/tmp/efs-taskDP*` trees were removed on both hosts.

## Reproduction

From a built tree (`make test-bins tests/faults/efsd`; on fstor007 export `EFS_FUSE_PREFIX` to the shim first), with TMPDIR set to a private directory and the timings of a gate that ran this test at index 503 (wave 58 node9900 gate 1):

```bash
scripts/evidence/dp/k38_loaded_loop.sh /tmp/dp-before 20 7 ~/efs-artifacts/merge-wave58-20261105-node9900-gate1/timings.tsv
```

Run it once with the test as on `cb51d1cd6` (delete the `EFS_PACK_FAIL_BACKOFF_MS` key) and once as committed. Expected: before, some runs over 30 s with `2 packed` about 30 s after `1 full` in `times.txt`, and the kept `work-N/k/n*.log` show a second `written degraded` pass with `lag_s=30`; after, `runs.tsv` has every run under 30 s. A run takes about 23 s, and the 7 workers stop after the last run. Remove the output directory and the TMPDIR afterwards. Remaining gates: none.
