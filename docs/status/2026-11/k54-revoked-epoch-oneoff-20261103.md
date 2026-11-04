# K54 one-off "returned member stored a revoked-epoch fragment" (Task CU)

Task CU, Nov 3 2026, node9900 (192 CPU, XFS, TCP, buffered I/O, 4-member loopback cluster, quota 1G). Purpose: classify the single K54 failure Task CS saw in run 3 of 30 under port churn (`k54-harness-port-race-20261103.md` on Task CS's branch `claude/dreamy-hawking-817f48`, not yet merged) and decide whether it reopens K54. Verdict: **C, no recurrence in 60 runs; the kept log is inconclusive about the file-set difference, but it rules out the port race as the cause.** No K row is added.

## What the kept log shows

`fail-before-3.log` (8 lines, Task CS artifacts) has the control and scenario (i) PASS lines, then the assertion at `test_k54_member_down_fence.py:153`. That is scenario (ii), after `late_all_stale` (STALE from all three placed members, including the restarted victim) and after the sealed-certificate check, so the victim efsd was alive on its assigned port, answered STALE and wrote its certificate into its own directory. A member on a wrong port or a stranger on that port could not do that, and `stored()` reads the victim's directory, not its port. The five port-race failures (runs 1, 16, 21, 24, 29) all fail earlier, at `cluster.start(3)`, with `Address already in use` in the daemon log. Run 3 does not share that signature. The log has no ports, daemon logs or file names: the test dir is deleted and the assertion printed no diff.

## Why the assertion can fire without a fence gap

It was `stored(work,[victim]) == before`: any difference in the file set under `data/exports/1` of the victim fails it, additions and removals alike. `before` is taken after the control PUT, whose accepted fragment is revoked and then belongs to abandoned-upload collection, which may legitimately remove it from the restarted member (timing dependent, load sensitive). That is a hypothesis; it was not observed.

## Reproduction

Source: devel `ab264d99e` plus one change, the assertion in `tests/test_k54_member_down_fence.py` now prints the added and removed files (committed with this page). Unfixed devel `ticket_put.py` (no port hold). `git archive` to `/tmp/efs-taskCU`; build `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig TMPDIR=/tmp/efs-taskCU-tmp make -j32` (build id "unknown": no git metadata in the archive). Then, in a detached screen (`SCREENDIR=$HOME/.screen`) from the tree root: `tests/live/port_churn_loop.sh 60 OUTDIR` (8 RST-closing loopback churners, 60 sequential runs, ~30 s each; script from commit 97df0be06 on Task CS's branch). Private ports only; 17432-17435, `/data1/efs` and the `efs` user untouched.

## Results (60 runs, 49 pass)

| Outcome | Runs |
|---|---|
| PASS | 49 |
| `Address already in use` at a restart (harness port race) | 9 (9, 17, 22, 29, 37, 40, 49, 57, 58) |
| **`returned member stored a revoked-epoch fragment`** | **0** |
| New signature: revoke of the SIGKILLed member fails, `phase=participant-floor rc=-1` | 2 (44, 50) |

Two earlier-unseen failures (44, 50) are not the subject here and are not explained: `fence` returned rc=-1 at phase `participant-floor` with one member SIGKILLed (the test retries only `begin`/`cleanup` phases). They may be churn-load related or a separate K54 timing issue; follow-up, not investigated. Scenario (ii), where CS run 3 failed, was exercised with the diff-printing assertion in at least the 49 passing runs.

## Limitations and remaining gates

The one-off is unreproduced, so the removal-versus-addition question stays open; if the assertion message shows an **added** file on a recurrence, open a K row (blocks alpha: yes) at once. Daemon-side code (`raft_host.c`, `put_upload.c`) was located, not audited. Only node9900, one source commit.

## Artifacts

`~/efs-artifacts/taskCU-20261103/` on node9900 (loop.txt, 11 `fail-N.log`, build.log, churn script, 15 files); `SHA256SUMS` there has sha256 `c2fc1cce5ae4f0b4191b4e6cb9f3992b53be71607df8f4041348cf0ba1d202b2`. `/tmp/efs-taskCU` and `/tmp/efs-taskCU-tmp` removed.
