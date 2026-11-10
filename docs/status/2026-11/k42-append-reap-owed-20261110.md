# K42 reopened: a clean unmount refused for the whole budget with `append-reap-owed` (Task FP)

**Verdict (Nov 10): cause found and reproduced, fix and regression tests committed; PASS on a private 4-member TCP cluster with the reaper stopped, 20 of 20 clean stops. The RDMA multi-host gate that failed (Task EY, `rdma-4x1`) is still owed. K42 stays OPEN until the coordinator rules; this page does not edit the K42 row or the counts line.** Branch `claude/kind-agnesi-20633e` on devel `7cd0a2637`, code commit `ce2c61998` (`84284911c` only rebuilds the code graph); not merged, not pushed.

## Step 1: the code path

Line numbers are devel `7cd0a2637`.

A clean unmount ends in `efs_session_revoke` (`src/common/session_revoke.c`): BEGIN, fence, FINISH, then per touched shard two cleanup phases (leases, then appends) through `efs_session_cleanup_bounded` (`session_revoke.c:66`). The append phase repeats `EFS_MD_REQ_SESSION_APPEND_NEXT` until the server says the shard is empty.

| Question | Answer |
|---|---|
| Where is `append-reap-owed` decided? | The server's step, `host_session_append_step` (`src/server/raft_host.c:19601`). It calls `efs_meta_retire_append_session` (a DONE row or receipt of the session) or `efs_meta_finish_append_fenced` (an OPEN reservation of the session). Both found the inode row absent and set `*wait = EFS_META_APPEND_WAIT_GONE` (`src/meta/append_plan.c:453` and `:687` on devel). The host turns the `NOT_FOUND` into `BUSY` with salt `EFS_MD_SESSION_APPEND_RETRY_SALT + wait = 7` (`raft_host.c:19644`, `:19646`; `include/efs/meta_cmd.h:333`). The client names salt 7 `append-reap-owed` (`include/efs/session_revoke.h:72`) and `efs_session_cleanup_retry_ok` (`:55`) lets it repeat the step. |
| What state makes the reap "owed"? | The session still has append rows (its receipt `EFS_KV_KIND_APPEND_RECEIPT`, DONE or OPEN reservations `EFS_KV_KIND_APPEND_RSV`) on an inode that was unlinked and reclaimed: the inode row is gone and an `EFS_KV_KIND_REAP` marker stands on the anchor shard. Only `REAP_MULTI` removes those rows (`efs_meta_apply_reap_batch`, `src/meta/meta_apply/sweep.c:384`). |
| Which thread performs the reap, and what triggers it? | The GC thread of the group leader, `host_gc_thread` (`raft_host.c:12665`) calling `host_gc_reap_pass` (`:10923`) once per loop: scan up to `REAP_SCAN_MAX = 512` markers from a round-robin cursor (`:9532`), sweep their lanes (up to 32 rounds), have every member accept the inode for reclaim (`host_gc_inode_nodes_batch`, `:11072`), then propose `REAP_MULTI` for the accepted ones. It is periodic: a pass that is capped and made progress reruns after 20 ms, any other pass waits `GC_LOOP_MS = 1000` (`host_gc_backlog_idle`, `:11528`). One queue serves every dead inode of the cluster. |
| What does the revoke do while it waits? | Only polls. `efs_session_cleanup_bounded` repeats the same step every 50 ms (`session_revoke.c:113`) until `EFS_SESSION_CLEANUP_BUDGET_MS` = 20 s (`session_revoke.h:77`). The step is read-only on the reap side: nothing asks the GC thread for the inode, nothing is deleted. |
| What bounds the wait on a healthy cluster? | Nothing. The time is the marker's position in the queue (512 per pass), times the pass time, plus any member that has not accepted the inode (the marker is skipped each pass). With the reaper stopped it is infinite. Under a delete load the queue is longer than 20 s; Task EY's group 2 leader had a non-empty reap queue when the refusal came. This was the defect: a refusal reason that nothing actively clears within the budget. |

Why only RDMA failed in Task EY is inferred, not shown: its page has no server line for shard 1460 and the queue length at 14:40 was not sampled. The natural race did not reproduce on devel here (3,000 appended and unlinked files on one shard, reaper running, 2 of 2 clean stops in 0.3 to 0.8 s), so the reproduction below stops the reaper with an existing hook.

## Step 2: reproduction on devel

- **Build:** devel `7cd0a2637` as `git archive HEAD` on intel1, `make` and `make tests/faults/efsd`. Binaries (no `.git`, so the build id reads `unknown`): `efs-fuse` `bb8cf2ed9eeb...`, `tests/faults/efsd` `bf9a952b4534...` (sha256 prefixes).
- **Workload** (`tests/test_k42_reap_owed.py`, committed): a client mounts, creates 24 files with two `O_APPEND` writes each, unlinks them, then `fusermount3 -u`. The hook `EFS_FAULT_GC_DISABLE=1` (existing, `tests/faults/efsd` only) stops the reaper, so the REAP markers are never driven. A second pass of the same test with the reaper running does not need the hook. The `posix-durability` suite itself was not run: the state that matters is dead inodes that still carry this session's append rows at the stop, which the test creates directly; the EY failure was the same state left by the preceding d4 load on that mount.
- **Command:** `python3 tests/test_k42_reap_owed.py --members 4 --rounds 20 --files 24 --port-base 18600 --keep` (four private members on 127.0.0.1:18601-18604, discovery 18699, one mount per round on members 1..4 in turn).
- **Result on devel: 20 of 20 clean stops FAIL**, `efs-fuse` exit 1, `revoke rc=-13 phase=cleanup shard=1`, `cleanup_ms` 20,068 to 20,121. Client `reason=`: `append-reap-owed` 9 times, `uncertified-busy` 11 times. Server side, in a separate diagnostic run (one telemetry line added to the server, devel otherwise; 197 steps over 20 s): every step was `found=0 rc=NOT_FOUND wait=4` (GONE), never anything else. So all 20 failures are the same wait; the 11 `uncertified-busy` labels are the client losing the certified reason at the deadline (below, findings).
- The 3-member default form (`python3 tests/test_k42_reap_owed.py`, the new `make test-long` row) fails on devel in 25 s.

## The coordinator's lead: `txn-finish ... abort decided rc=0 (coordinator lost its group)`

Task EY saw this line about every 10 ms, 3,000+ times, on two group-2 followers of the failing RDMA cell (none on the 3-member cell), plus `prev-compacted` AppendEntries rejects. It is printed once per transaction by `host_fin_one` (`src/server/raft_host.c:10240`): the coordinator proposed no COMMIT and its own DECIDE ABORT failed (the coordinator group's leader is another member), so the finisher thread decides ABORT at the current leader and then RESOLVEs. `rc=0` is that decision succeeding, so each line is a transaction that finished, not one that cannot. About 10 ms apart means a caller whose PREPARE is refused over and over (a retry loop; EY places it in posix-2client), not a single stuck transaction.

**My reproduction refutes it as a necessary part of the cause and does not test it as a contributor.** The 4-member private cell had the layout the lead names (`formed: 4 members, g0 voters 0x7, g2 voters 0xe`: the two metadata groups on different members), failed 20 of 20, and its logs hold no `txn-finish` line and no AppendEntries reject (0 in all four daemon logs, `logs-before-gcoff-20rounds/`). The refusal is fully explained by the reaper not having run. What I cannot exclude is that on EY's cell such aborting transactions held prepared intents on the dead inode's keys or lanes and so made the GC's `REAP_MULTI` items BUSY (the unit test shows prepared intents do refuse a reap batch and leave its marker), lengthening the reap; that is untested, and EY's own observation that the GC passed every 1.6 to 2.5 s with reaps pending after every pass fits either a long queue or a refused head of it. The fix does not depend on it: the revoke's step deletes the session's rows of a dead inode itself, and no live appender can hold intents on a dead inode. The abort loop and the `prev-compacted` rejects are a separate finding for the RDMA cell and stay undiagnosed.

## Step 3: the fix

`retire_dead` in `src/meta/append_plan.c`. When the step finds the inode gone, it deletes the session's own rows of that inode itself, in one exact-image transaction (the PREPARE/DECIDE/RESOLVE of every other cleanup step), guarded by the inode's absence: its receipt and its reservations (OPEN or DONE) of this uuid and epoch, up to 11 rows per transaction (the 12 images one shard takes with the inode guard). Both selections use it (`efs_meta_retire_append_session` and `efs_meta_finish_append_fenced`). Other sessions' rows stay for the reaper; a row the reaper took first is progress, not an error (no transaction when none is left).

**What `append-reap-owed` protected, and why the protection is kept.** The step must never report the phase empty (salt 1) while a row of the session remains on a dead inode, because `REAP_MULTI` alone used to own it. Now the step removes those rows in a committed transaction, and `efs_meta_append_session_idle` still reads the rows afterwards, so the phase is empty only when none is left. If the transaction cannot commit, the step still returns BUSY or the transport error (refused PREPARE: `append-prepare-aborted`), and the revoke still fails closed with its reason. A late `REAP_MULTI` item that lists keys the step already deleted completes the inode (`tests/test_append_cleanup.c`, `dead_inode_retired`); a `REAP_MULTI` that runs between the step's PREPARE and its decision is refused BUSY by the prepared intents and keeps its marker. `EFS_META_APPEND_WAIT_GONE`, salt 7 and the client's `append-reap-owed` string are kept because the client side and `tests/test_session_cleanup_cli.py` still recognise them, but nothing produces them any more (the coordinator may retire them).

Callers (grep over `src` and `tests`): `efs_meta_retire_append_session` and `efs_meta_finish_append_fenced` have one product caller, `host_session_append_step` (`raft_host.c:19633`, `:19637`); tests `tests/test_append_cleanup.c`, `test_append_transaction.c`, `test_append_interleave.c` and the stubbed `tests/faults/test_append_cleanup_retry.py`. The live-inode paths are unchanged.

An earlier version deleted only the selected row per step: 4.4 s of cleanup for the 24 files; batching the session's rows of an inode into one transaction brought it to 1.6 s.

## Step 4: tests

- `tests/test_k42_reap_owed.py` (new, `make test-long`, `tests/budgets.tsv` row, 15 s): fails on devel (exit 1, `rc=-13`, 25 s), passes with the fix.
- `tests/test_append_cleanup.c` `dead_inode_retired` (in `make test`): one step deletes the three rows of the revoked session (two reservations, receipt), the shard is idle, session 43's rows stay, a `REAP_MULTI` list built before the step still completes the inode, a fenced step on a gone row is OK, the reaper running between PREPARE and the decision is refused, an OPEN reservation of the dead inode is retired. Fails on devel at the first step (`efs_meta_retire_append_session` returned BUSY/GONE).
- Unchanged and passing: `tests/test_session_cleanup_cli.py`, `tests/faults/test_append_cleanup_retry.py` (it stubs the retirement), `tests/test_k42_peer_stop_open.py` (not run here), `tests/test_append_transaction.c`, `tests/test_append_interleave.c`.

## Step 5: results

| Run (intel1, 4 private members, TCP) | devel `7cd0a2637` | fix `ce2c61998` |
|---|---|---|
| 20 rounds, 24 files, reaper stopped, clean stop | 0 of 20 exit 0; cleanup 20,068 to 20,121 ms; `append-reap-owed` 9, `uncertified-busy` 11 | **20 of 20 exit 0**, `rc=0`; cleanup 1,564 to 1,614 ms |
| 10 rounds, 300 files, reaper running | not run | 10 of 10 exit 0; cleanup 197 to 731 ms |
| `tests/live/e2_peer_stop_loop.py --members 4 --port-base 18600` x3 (20 runs each) | not run | 20/20, 20/20, 20/20; revoke 159 to 609 ms |

| Check (fix `ce2c61998`, intel1) | Result |
|---|---|
| `make test` | rc 0 |
| `python3 scripts/tests_for.py meta/append_plan --run`, `server/raft_host.append --run`, `common/session_revoke --run` | rc 0 each (the first includes the new test) |
| `bash tests/sanitizer_check.sh tsan` | PASS, 31 run, 0 failed, 538 s. `k96_status_slots_live` (K107) passed in this run; it was not a failure here |
| `bash tests/sanitizer_check.sh asan` | PASS, 59 run, 0 failed, 373 s |
| `python3 scripts/check_doc_links.py`, `python3 docs/gen-architecture-full.py`, `make docs-check` | PASS (run on the Mac before the final commit) |
| Budget row measured (default 3-member form) | 5.3, 6.4, 6.6 s wall |

The host was shared: Task FL ran its TSan and gate runs on intel1 during all of this (load average 3 to 6), so every time above includes that load.

## What is not verified

- **The RDMA multi-host gate (Task EY `rdma-4x1`, with the 205,743-entry GC backlog) is still owed.** Nothing here ran on RDMA or on more than one host.
- The natural race (a loaded reaper that is merely late) was not reproduced on devel; the fix is shown with the reaper stopped, and safe with it running (the 300-file runs and the `e2` loops).
- Not run: the full `make test-all`, `tests/test_k42_peer_stop_open.py`, any run with a member down.
- Per-shard ceiling: on the reaper-stopped 4-member cell, the dead-inode step costs about 63 ms per appended file (its three rows in one transaction), measured twice each: 100 files 6.3 s, 200 files 12.6 s, 300 files 18.9 s of the 20 s budget; 600 files (first run) fail after 20 s. So roughly 300 dead appended inodes on one shard is the most one clean stop clears without the reaper; beyond that the stop fails closed, as before, and `efs-mgmt raft-session fence` resumes it. The test puts all inodes on one shard (one client's inodes are strided onto shard 1); a real session spreads them (1,225 touched shards in the EY run). The control, not changed by this fix: files appended and left linked retire through the live-inode path at about 21 ms each (100 files 2.0 to 2.1 s) but fail at 20 s in one of two runs at 200 files and in two of two at 300 (`rc=-13`, label `uncertified-busy`). The live-inode retirement therefore has a lower per-shard ceiling than the new dead-inode step; it predates this task and is a separate finding below.

## Findings for the coordinator (not fixed here)

1. **The client loses the certified reason at the deadline** (11 of 20 devel failures said `uncertified-busy` while every server step was GONE). In `efs_session_cleanup_bounded` (`session_revoke.c:106-114`) a `NOT_PRIMARY` answer sets `last_wait = 0`, so a final `NOT_PRIMARY` at the deadline erases the earlier certified wait; keeping the last certified salt would label it correctly. Not verified as the only path.
2. `EFS_META_APPEND_WAIT_GONE` / salt 7 / `append-reap-owed` are dead on the server side now.
3. **Per-shard ceiling of the append cleanup, one transaction per inode** (numbers in the previous section): a client that appended to a few hundred files of one shard (live or dead) can still exhaust the 20 s per-shard budget at the stop. The cost is the three-phase transaction per inode (one PREPARE proposal per image, then DECIDE and RESOLVE); a single-participant fast path for the cleanup, like the reservation's `APPEND_ATOMIC`, or batching several inodes per transaction, would lift it. Not done here: it needs a new log command.

## Reproduce

```
git archive HEAD | ssh intel1 'tar -x -C ~/efs-taskFP/src'     # on a host with FUSE
make -j6 && make -j6 tests/faults/efsd
python3 tests/test_k42_reap_owed.py --members 4 --rounds 20 --files 24 --port-base 18600
```

Expected: 20 of 20 `exit=0 revoke_rc=0`. Cleanup: `~/bin/edelete --force --delete ~/efs-taskFP`.

## Raw evidence

Outside Git, on intel1: `~maik/efs-artifacts/taskFP-20261110/` (run outputs, the diagnostic server logs `step2-dbg/`, binary hashes `final-binaries.sha256`), listed with sha256 in `SHA256SUMS`; sha256 of `SHA256SUMS` (76 files): `3c9cc20c6c2147d8cd7a3036c3a86628355dc43290b4e03d3025fbb346dfd845`. Binary hashes: devel `efs-fuse` `bb8cf2ed9eeb...`, `tests/faults/efsd` `bf9a952b4534...`; fix `efs-fuse` `8dca2a2ce008...`, `tests/faults/efsd` `9b42ba3df27a...` (full values in `devel-binaries.sha256` and `final-binaries.sha256`). The `intel1` trees were deleted afterwards (`~/bin/edelete`).
