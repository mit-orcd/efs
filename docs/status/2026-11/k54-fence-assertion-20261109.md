# K54 fence test asserts what K54 forbids (Task EJ)

Task EJ, Nov 9 2026, fstor007, devel `33cd0a839` (after wave 65). Purpose: `tests/test_k54_member_down_fence.py` (K54, the 35 s row) failed scenario (ii) twice, on a file *removal* rather than an addition: once on Nov 3 ([Task CU](k54-revoked-epoch-oneoff-20261103.md)) and once in the wave 65 node9900 gate 1 (`~erbmi1/efs-artifacts/merge-wave65-20261109-node9900-gate1/fail-516.log`: added `[]`, removed one path under `data/exports/1`). K54 forbids a returned member storing a fragment for a revoked epoch, which would be an addition.

**Verdict: PASS. Test change only, no K row, blocks alpha: no.** The removal comes from abandoned-upload collection (D31), which can only delete a fragment of a never-published ticket. The assertion now fails on any added file, and on any removed file other than the control PUT's revoked fragment. Over 20 solo runs under a parallel `make -j8 test` loop, 20 of 20 passed and the removal count was 0. An earlier attempt did catch the removal: in attempt-1 run 1 the collector deleted the chunk directory while the test was walking it, and the old `pathlib` glob raised `FileNotFoundError`.

## The collector and its timing

The collector is `host_put_orphan_pass` (`src/server/raft_host.c:12521`). The group leader's GC thread runs it on every maintenance block, once per `GC_LOOP_MS` = 1000 ms (`raft_host.c:9517`, `:12769`), right after `host_put_drain_pass` (`:12732-12733`). A non-leader resets its cursors and runs neither.

1. **The fence completes.** `efs_session_begin_fence` raises the epoch (`src/meta/session.c:239`). `finish_fence` leaves the session ACTIVE at that epoch (`session.c:316`). From then on, `put_orphan_revoked` (`raft_host.c:12479`) accepts the control ticket's epoch: the session is ACTIVE and its epoch is past the ticket's. No further admit is needed.
2. **The drain.** The control ticket is ADMITTED and never published. The drain abandons it to REJECTED and retires it into the orphan ledger. PUBLISHED heads retire without an orphan record (`include/efs/put_ticket.h:115-127`).
3. **Collection.** On that block or a later one, `efs_put_orphan_collect` (`src/meta/put_orphan.c`) asks each placed member to delete its fragment (`put_orphan_delete`, `raft_host.c:12496`). A member that is down returns IO. It is skipped for that pass (`put_orphan.c:40`) and the record waits for it.
4. **On the member.** `server_put_orphan_delete` (`src/server/put_orphan.c`) deletes only under four conditions:
   - the fragment is placed on this member;
   - its storage session floor is above the epoch (otherwise it learns the floor from the session shard, or returns BUSY, `:47`);
   - after a ReadIndex, the ticket's group still holds the orphan record (`:48`);
   - the stored checksum matches the ticket's (`server_delete_fragment_if_sum`, `:58`).

   After the delete it drops the emptied chunk directories (`server_gc_superseded_delete`, `:64`). That directory removal is what hit the walk in attempt-1 run 1.

**Timing.** The control fragment is normally gone within a few maintenance blocks after the control fence, well before scenario (ii) takes `before`. It survives into (ii) in two cases:

- **Leader change.** The leader of the ticket's group changes, so cursors reset and the new leader starts over.
- **Fragment holder down.** The member holding the fragment is the scenario (i) victim (SIGKILLed). Collection then waits until that member restarts, which is shortly before (ii) admits its ticket and snapshots its victim. The (ii) victim is chosen by the same rule as the (i) victim (the first placed member above 2), so it can be the same member.

Either way the delete lands between `before` and `after` only under load-dependent timing. That matches two failures in many gates.

**Why an acknowledged fragment cannot be taken.** Condition 3 requires an orphan record. Orphan records exist only for REJECTED tickets, so a published object's fragment never has one. Condition 4 requires the exact ticket checksum; a different body under the name returns PROTO and is kept. There is no defect.

## The assertion change

Commits `c41f4badf` and `a8c5f23da`, `tests/test_k54_member_down_fence.py`:

- **Control.** The control step records `control_files`: the files that appear under `data/exports/1` on any member when the control PUT is accepted. The control checks themselves are unchanged.
- **Scenario (ii).** The check is now: no file added on the returned member (`the returned member stored a revoked-epoch fragment`), and every removed file is in `control_files`. The step prints `(ii) stored set: added 0, removed N`. The old `after == before` failed exactly when N ≥ 1.
- **`stored()`.** It uses `os.walk`, which skips a directory removed during the walk; `Path.glob` raised. Scenarios (i), (iii) and (iv) are unchanged.
- **Budget.** Unchanged: the row stays at 35 s.

## Reproduction and environment

- **Host and build.** fstor007: 96 CPUs, TCP, buffered I/O, a 4-member loopback cluster with quota 1G. Ports are the kernel ephemeral range 32768-60999, picked by `ticket_put.free_base`; nothing touched 17xxx or 27xxx.
- **Source.** The tree was shipped as a `git archive` of `a8c5f23da` (tar sha256 `965acec0da32…`; attempt 1 used `c41f4badf`) to `/tmp/efs-taskEJ`. Build identity is "unknown", since the archive has no git metadata.
- **Build.** `make -j96 all` with `TMPDIR=/tmp/efs-taskEJ-tmp` and a libfuse shim `/tmp/efs-taskEJ-fuse` (`lib64` and `include` linked to `~/orcd/scratch/efs/orcd-test/deps/libfuse`) as `EFS_FUSE_PREFIX`, with `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` set to the shim's `lib64`.
- **Driver.** `run20.sh` (kept with the raw files) runs in a detached screen. A background loop runs `make -j8 test` repeatedly and starts 20 s before the first run. Then it runs `python3 tests/test_k54_member_down_fence.py` 20 times in sequence, each under a 300 s outer kill, and greps the removal count.
- **Cleanup.** `/tmp/efs-taskEJ*` and the tar were removed afterwards.

## Results

| Attempt | Runs | Pass | Removal in (ii) | Wall (s) | Load avg |
|---|---|---|---|---|---|
| 1 (`c41f4badf`, glob walk) | 2 (stopped) | 1 | 1: run 1 raised `FileNotFoundError` in `stored()` while the collector removed `n3/data/exports/1/0000/0000/0000`, so the old test would also have crashed | 8.3 (run 1) | 2.2 |
| 2 (`a8c5f23da`, os.walk) | 20 | 20 | 0 | 25.3-28.0, median 26.5 | 1.5-2.3 |

The load loop completed 5 `make -j8 test` passes, all rc 0.

## Limitations

- **Light load.** Attempt 2 saw no removal, so the new subset branch was not exercised end to end there. Attempt 1 exercised the removal, but on the walk rather than at the assertion.
- **One host.** Only fstor007 was used, at about load 2 on 96 CPUs. node9900, where the gate hit the failure, was not used.
- **What the print does not show.** It does not say whether the (ii) victim held the control fragment at `before`. The timing account above is derived from the code, not traced in a run.

## Artifacts

`~/efs-artifacts/taskEJ-20261109/` on fstor007 holds 34 files: `runs.txt`, 20 run logs, 5 load-loop logs, `attempt1/`, the build logs, the env file and `run20.sh`. The sha256 of its `SHA256SUMS` is `a43a9121bcec154ab1f2646be3ddda909f074310381819124355b292593a58c6`.
