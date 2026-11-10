# K112: clean client stop refused for the whole cleanup budget (Task FQ)

**Merge state (Nov 10):** merged into devel in wave 76 as a diagnosis; K112 stays OPEN. An independent analysis for the coordinator found the safe form of the proposal (only the stop's own retirement, only the revoked session's completed rows at or below the smaller of the file size and the frontier); the fix is a task of its own.

**Status**
- Source commit: devel `06bfdb393` (no code change on this branch).
- Verdict: **DIAGNOSIS PARTIAL, NO FIX.** The failing state is reproduced as slowness, the cause is narrowed to the append-row retirement of the stop, and no change was made because the safe change needs a protocol decision (below).
- Remaining failures: K112 stays OPEN. Steps 3 to 6 of the task (fix, test, post-fix loops, gates) are not done.
- Next action: decide the proposal in "Proposal"; then implement and run the before/after loops with `scripts` named below.
- Branch: `claude/happy-solomon-d5e8f3`, **unmerged**.

## Step 1: answers from the Task EY logs (RDMA, candidate `9bb50fe79`, loop 3)
Logs: `~erbmi1/efs-artifacts/taskEY-20261109/rdma-k42-e2/keep-e2-peer-stop-loop-keep-3/efs-e2-peer-uzxx9lq7/c/`.

**Placement.** Loop 3: `group=0 became leader` on n1 (17:01:29.658), `group=2` on n4 (17:01:31.281). Group 0 voters are 0x7 (n1-n3), group 2 voters 0xe (n2-n4), so only n2 and n3 host both groups and neither leads one. Loop 2 had the same placement (n1/n4) and passed, with revokes of 170-240 ms. Loop 1 had n1/n2: n2 hosts both and leads group 2, no `txn-finish` line, revokes ~50 ms.

**a. The retry "every 10 ms" is not what the logs show.** `txn-finish ... (coordinator lost its group)` appears only in `n2.log` (817+760) and `n3.log` (512+496); n1, n4, n5: 0. They come in bursts of 2-5 lines 5-10 ms apart, one burst about every 1.09 s (about 4.3 per second over 10 minutes; loop 2 had 3.2 per second and passed). So the line is a steady background of aborted publication units on the dual-group followers, not a tight loop, and it does not by itself cause a refusal. Operation: reports of the two appending clients on inode 7847 and its siblings (shard 3751): `raft-host: report resolve ino=7847 rc=-13 why=retire (retry)` (71 times, n2 only) followed 50 ms later by `report-receipt ... rc=0 outcome=applied attempts=2` (for example 17:01:42.276/42.326). Refusal reason of the PREPARE is not logged (BUSY, `rc=-13`).

**b. `uncertified-busy`.** `efs_session_cleanup_reason(salt, rc)` prints it for any BUSY whose salt is not one of the seven certified ones (`include/efs/session_revoke.h`). In `efs_session_cleanup_bounded` (`src/common/session_revoke.c`) the stage reason is `last_wait_reason ? last_wait_reason : r.salt`, and `last_wait` is reset to 0 by every `NOT_PRIMARY` retry and by progress, and the function's result starts as `EFS_ERR_BUSY`. A stop that ends at the deadline right after a `NOT_PRIMARY` (or a step that made progress) therefore reports BUSY with salt 0, printed as `uncertified-busy`. The server also logs `session append cleanup ... BUSY uncertified at <why>` for a BUSY it cannot certify, but no such line is in the member logs, and the members reported `telemetry: overflow=1`, so the line may have been dropped. Not determined from the logs: what the server answered during the 20 s.

**c. `append-open-by-other-session` (stops 2, 3, 6, 12).** Peer001 (session `7796b14f`) failed first (`17:01:39.67`-`17:01:59.79`, `uncertified-busy`), so its session's append rows stayed. The next peers started 17:01:59.9 (`1d972c17`), 17:02:28.4 (`b18a5f59`), 17:03:54.2 (`f5b621d6`), 17:06:45.8 (`2b2c53e3`). Holders that were alive: the primary (`f3849fe7`, appending throughout) and, after the first failure, the failed peers' leftover rows. The logs do not say which session held the OPEN append for each of the four. Not determined.

**d. Loop 2 versus loop 3.** Same leaders, same abort rate. Not determined from the logs; the amd runs below suggest the difference is the length of the retirement (below), which depends on load and per-step latency.

## Step 2: reproduction (amd, TCP loopback, 5 members, `e2_peer_stop_loop.py --runs 20 --keep`)
Tree: `git archive` of devel `06bfdb393`, `make` and `tests/faults/efsd`. amd also ran another session's sanitizer lanes (load average 8-12).

| run | leaders | stops | revoke_ms (min / max) | abort lines |
|---|---|---|---|---|
| before-loop1 | g0 n1, g2 n4 | 20/20 | 2,674 / 15,652 | 221 (n2 only) |
| dbg-loop1 (debug line, sampled) | n1/n4 | 20/20 | 2,254 / 16,062 | n/a |
| dbg-loop2 (6 runs) | not n1/n4 | 6/6 | 253 / 304 | n/a |
| dbg3-1..3 (6 runs each) | n2/n4, n3/n3, n3/n3 | 6/6 each | 53 / 236 | low |
| dbg3-4 (6 runs) | n1/n4 | 6/6 | 208 / 370 | 34 |
| dbg4 (20 runs, debug line on every reply) | n1/n4 | 20/20 | 1,730 / 9,368 | 185 (n2 only) |

No stop was refused in 12 loops of runs because none exceeded 20 s; with leaders n1/n4 and the 20-run loop every revoke took 2 to 16 s against the 20 s budget, where the same code with fewer runs takes 0.2 to 0.4 s, and the ORCD loop 1 (n1/n2) 50 ms. The slow state is the failing state: one more second of step latency refuses the stop. Not forced with a fault hook (`EFS_FAULT_*`, `place_leaders`): not tried.

One slow revoke in full (`dbg4`, peer012, 9,368 ms, per-reply debug line `fqdbg` in the amd copy only): 142 cleanup replies for one shard: 106 `rc=0 salt=0` steps of the append phase (each retires one page of the session's rows), 19 `BUSY salt=6` (append-prepare-aborted), 14 `BUSY salt=4` (append-open-by-other-session). The revoke time is the number of append rows left by the session (one DONE reservation per append, retired at most 8 per step by `efs_meta_prune_append`/`APPEND_PRUNE_PAGE`, `src/meta/append_plan.c`) times the latency of one cross-group step. While another session has an OPEN append on the inode, `prune_capture` returns without selecting any row (its comment: DONE entries still carry frontier dependencies while OPEN exists), so the retirement makes no progress until the other writer is between two appends; the other writer here (primary) appends continuously. Each retried step also adds an aborted PREPARE (salt 6) on the dual-group follower (n2), which is the `txn-finish` noise.

## Proposal (not implemented)
1. Retire DONE reservations that lie wholly below the inode's committed floor regardless of an OPEN append of another session (rows `offset+length <= min(base_size, cursor frontier)`; they cannot move the frontier). This needs the protocol owner to confirm that the cursor frontier is a safe floor while OPEN exists; if so `prune_capture` loses the early return and the retirement no longer waits on the other writer. A unit test in `tests/test_append_cleanup.c` (session A with N completed appends, session B holding an OPEN reservation, steps to idle) fails on devel (BUSY wait OPEN, no progress) and passes after.
2. Report the real reason: keep `last_wait` across a `NOT_PRIMARY` retry in `efs_session_cleanup_bounded` and return the last server answer, so `uncertified-busy` names what it was.
3. Optional: fix the cover pick so cleanup steps are sent to the dual-group member that leads a group when one exists (`host_pick_peer` returns the first member hosting both groups, n2, for every request).

## Not verified / owed
RDMA rerun; any post-fix loop; `make test`, sanitizers and doc checks (nothing code-level changed on this branch). Raw output stays outside Git: `~maik/efs-artifacts/taskFQ-20261110/` on amd, `SHA256SUMS` sha256 `cc992edfbac80b51c728316eb71c0b0d1e7b3b9649dbc36522e7296a5368a625`. Reproduction: `tests/live/e2_peer_stop_loop.py --runs 20 --keep` on a built tree (`make`, `make tests/faults/efsd`), five members, one loop at a time; compare leader lines (`became leader`) and `session revoked` `cleanup_ms` per run.
