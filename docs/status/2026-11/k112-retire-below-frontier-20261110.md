# K112: a clean stop retires the revoked session's rows below the frontier (Task FS)

**Merge state (Nov 11):** merged into devel in wave 79 with its second round (Task FV). The merge replaced the time threshold in step 1 of `tests/test_k42_peer_stop_open.py` by an ordering check (the peer must exit while the OPEN append is still held). K112 is VERIFY; the RDMA five-member stop loop is owed.

**Status (Nov 10):** source commits `4c70e2542` (change), `b63cf2750` (test), `af776509c` (loop option), branch
`claude/intelligent-shirley-a21c12`, **unmerged**, not pushed. **Verdict: PASS for the unit tests, `make test` and ASan; TSan has one failure that also fails on devel (`reclaim`); the stop is only partly faster (see the loop table), so K112 is not shown closed.** Remaining: the RDMA five-member
loop, a look at the aborted-PREPARE retries (below). Next action: the coordinator decides at the merge.

Task FS, amd (TCP loopback, ports 18500 to 18599, `~/efs-taskFS/`), linked row K112, follows
[the Task FQ diagnosis](k112-stop-refused-txn-abort-20261110.md).

## The rule as implemented

Only `efs_meta_retire_append_session` (`src/meta/append_plan.c`) changed, plus its own helpers `floor_row` and
`retire_below_floor`. With the cursor's OPEN count zero nothing changed. With it non-zero the step no longer answers
WAIT_OPEN at once. It scans the inode's reservation rows in key order and selects, up to one page (8), the rows that are
DONE, belong to the revoked uuid and epoch, and satisfy `offset + length <= min(base_size, frontier)`, both taken from the
captured inode and cursor images (the same images the replicated prune guards). It stops at the first row of that session
past the floor. Selected rows: normal progress. No row selected and none left: the receipt is deleted as before. Rows of the
session left above the floor: BUSY with WAIT_OPEN. `prune_capture` and `prune_row` are untouched, so its other four callers
(including `efs_meta_append_prune_due` before every reservation) keep their early return. No new command or field.

## Tests (`tests/test_append_cleanup.c`, real apply code, amd)

| case | devel source | fixed source |
|---|---|---|
| `foreign_open_wait` (changed): A completes 200, B holds OPEN 208, A fenced; A's steps return OK until idle, size 208, after B completes 216 | **FAILS** (BUSY, no progress, WAIT_OPEN; assert at line 281) | pass |
| `hole_row_kept` (new): R1 at 200, R2 at 208, R2 completes; prune removes 0, `prune_due` 0; after R1 size 216 | pass | pass |
| `floor_split` (new): A's row at 200 goes, A's row at 216 stays with WAIT_OPEN; after B completes size 224 and A is retired | FAILS at the first retirement step | pass |

Deviation from the brief for `floor_split`: a second session cannot reserve while another session's reservation is OPEN
(`append_foreign_open` answers BUSY, see `fenced_lost_reply`), so A's DONE row above the frontier cannot be produced with
the append calls. The test copies A's DONE row to offset 216 with `efs_kv_put`. The rule's "rows remain above the floor"
branch is therefore only reachable from such a state (a shrink or reset range); that case is protected by the test, not
shown in production.

## Stop loops (`python3 tests/live/e2_peer_stop_loop.py --runs 20 --keep --leaders 0:1,2:4`, five members, in a screen)

`--leaders` is new in this task (it calls `place_leaders` per group). Alternating devel and fixed trees (`git archive`),
one loop at a time. Cleanup reply counts come from a one-line stderr print in `session_revoke.c`, applied to both amd trees
only (`fqdbg-measurement.patch` in the artifacts); the fixed tree was restored from git afterwards.
`salt=6` is `append-prepare-aborted`, `salt=4` `append-open-by-other-session`.

| loop | stops | revoke_ms min / max | aborted (salt 6) | open-by-other (salt 4) | replies |
|---|---|---|---|---|---|
| devel 1 | 20/20 | 209 / 322 | 2 | 3 | 225 |
| devel 2 | 20/20 | 1,352 / 7,427 | 129 | 103 | 1,151 |
| devel 3 | 20/20 | 915 / 6,363 | 123 | 86 | 1,018 |
| fixed 1 | 20/20 | 674 / 4,679 | 111 | 0 | 825 |
| fixed 2 | 20/20 | 737 / 3,824 | 82 | 0 | 629 |
| fixed 3 | 20/20 | 738 / 3,460 | 71 | 0 | 653 |

Reading: the WAIT_OPEN replies are gone on the fixed tree (0 against 86 to 103 in the slow devel loops), and the maximum
revoke fell from 6.4 to 7.4 s to 3.5 to 4.7 s. It is not back to the 0.2 to 0.3 s of devel loop 1, which had almost no
conflict (the two devel loops that did conflict are the K112 state). The remaining time is the aborted PREPAREs: the prune
compares a cursor image that the other writer changes on every append, so 71 to 111 steps per loop still abort and are
retried after 50 ms. The analysis expected this; no retry or wait was added. No stop was refused (all 120 passed), so
the 20 s budget was never reached here either, as in Task FQ. Differences between loops also come from host load (amd
also ran other work in the Task FQ loops; not measured here).

## Gates (amd, tree `4c70e2542` plus the two test commits, built without the wave 76 merge)

- `make test`: rc 0. `python3 scripts/tests_for.py meta/append_plan --run`: rc 0 (no other component touched).
- `bash tests/sanitizer_check.sh asan`: PASS (62 run, 0 failed).
- `bash tests/sanitizer_check.sh tsan`: 31 of 32 PASS; **FAIL `reclaim`** (`test rc=1`, journal verification line in its log),
  the same on a second run and on the unmodified devel tree (`tsan-run1/2.out`, `tsan-devel.out`), so it is not caused by
  this change; it is not fixed or investigated here.
- Mac: `check_k_register.py`, `gen_ledger.py --write`, `check_doc_links.py`, `gen-architecture-full.py`, `make docs-check`: pass.

## Not verified

The RDMA five-member loop is owed. The merge of devel (wave 76) was made after the amd runs, so the gates above did not
run on the merged tree. The aborted-PREPARE retries (cursor churn by the live writer) are not addressed.

## Round 2 (Task FV)

An independent review found the floor rule correct and three defects around it. Repaired in this round; the floor rule,
`prune_capture` and `prune_row` are unchanged.

- **F1, `tests/test_k42_peer_stop_open.py` step 1 asserted the old wait.** It required `revoke_s >= HOLD_S - 0.5` (the peer
  waits about 3 s on the primary's held OPEN append). Measured on amd with the Task FS tree before any change: the test
  failed there with `revoke_s = 0.01` s and `rc=0`, as predicted: the peer's two rows lie below the frontier (the primary
  reserves after them), so they are retired at once. Step 1 now asserts exit 0, `rc=0` and `revoke_s < HOLD_S - 0.5`; its
  text says the rows below the frontier are retired while the OPEN append is held. Step 2 (past the budget, fail closed with
  `reason=append-open-by-other-session`) and the budget row are unchanged.
- **F2, second read of inode and cursor.** `retire_below_floor` read both again through the capture; if the live writer
  changed the cursor between the two reads, `finish_record` answered BUSY with no wait reason, so the stop was refused as
  uncertified where devel answered a certified WAIT_OPEN. The caller now reads the frontier in the same
  `efs_meta_apply_append_state` call that captured the cursor, and `retire_below_floor` takes the inode from the images
  already captured (`captured_get`), so nothing is read twice. BUSY paths of `efs_meta_retire_append_session` after the
  change: WAIT_OPEN (rows remain above the floor, nothing removed); WAIT_FRONTIER (no row removed, receipt row selected,
  as devel); ABORTED (a refused PREPARE, as devel); BUSY without a reason only from `finish_record` "source moved while
  collecting", which devel has in the same places (the capture reads and `prune_row`), and which `floor_row` shares.
- **F3, tests did not pin the rule.** New cases in `tests/test_append_cleanup.c`: another session's row and the same uuid at
  another epoch below the floor stay; a row between base_size and a higher frontier stays, and a row between a lower
  frontier and base_size stays; a row straddling the floor stays and a row ending exactly at it is retired; 12 rows below the
  floor plus one more are retired in pages of 8 (8, then 5, with the rows left counted after each call). The stale comment
  before `foreign_open_wait` is rewritten.

Mutation proof (amd, one at a time, each reverted): dropping the uuid filter, dropping the uuid and epoch filters, replacing
the minimum by base_size, replacing it by the frontier, and retiring a straddling row each make `test_append_cleanup`
abort (rc 134). Outputs: `mutation-*.txt` in the artifacts. The owner-filter mutations fail in the older
`foreign_open_wait` too (the foreign OPEN row is counted as remaining); the new `floor_owner_filter` is what pins a foreign
DONE row.

Gates (amd, Task FS tree plus these changes): `make`, `test_append_cleanup`, `tests_for.py meta/append_plan --run`, `make test`, `test_k42_fixture_cleanup.py`, `test_k42_reap_owed.py`: rc 0; `test_k42_peer_stop_open.py` three times: rc 0 (revoke 0.01, 0.17, 0.01 s); `sanitizer_check.sh asan`: PASS (65 run, 0 failed). Raw output: `~maik/efs-artifacts/taskFV-20261110/` on amd, with `SHA256SUMS`.

Review notes, not changed here:

- The branch "rows remain above the floor" can be reached when the same uuid appears at a higher epoch (a row of the same
  client at another epoch is not retired). Whether a client reuses its uuid across epochs is not verified.
- The cleanup holds the inode and cursor intents while a writer is live, so the writer can see a short BUSY.

## Reproduction and raw output

On a built tree (`make`, `make tests/faults/efsd`): `python3 tests/live/e2_peer_stop_loop.py --runs 20 --keep --leaders 0:1,2:4`
with `--port-base 18500` (default), one loop at a time; compare with a `git archive` of devel. Unit: `make
tests/test_append_cleanup && ./tests/test_append_cleanup`. Raw output (loop outputs, kept work dirs, reply counts,
gate logs, measurement patch): `~maik/efs-artifacts/taskFS-20261110/` on amd, `SHA256SUMS` sha256 `9b176ae4fa9ea8ba1ec40f329271bdff5de5bb51c14493c2a84b261852fb3bca`.
