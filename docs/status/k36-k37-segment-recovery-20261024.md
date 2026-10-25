# K36 + K37 · segment fragments dropped by verified recovery are rebuilt (Oct 24, 2026)

**Purpose.** Checklist items K36 and K37 in
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md) (section K).
Alpha 1.0 requires every acknowledged write, packed or not, to be
re-protected after a single member's unclean stop. K36
([evidence](k36-verified-recovery-20261024.md)) proves a store after an
unclean stop and marks dropped chunk fragments for W154, but a referenced
packed-segment fragment it dropped was only counted: K37
([evidence](k37-segment-rebuild-20261024.md)) was written before K36 landed.
This change connects the two: a segment fragment that verified recovery
drops is marked through K37's authority before the fence clears, and K37's
rebuild restores it.

**Verdict: PASS (scoped, loopback regression evidence).** intel1 `make
test-all` at `ad7fe1ded`: 468 commands, 2 FAIL that also fail on the base,
15 NOTRUN host limits. Code commit `ad7fe1ded` on branch
`claude/devel-k36-k37-segments-5885d9`, base `merge-wave6` (`646a43041`),
not merged, not pushed. Live multi-host, RDMA and orcd-mini: **NOTRUN**
(parent). Single-host loopback daemons are regression evidence, not
qualification.

## One mechanism

Recovery writes the same mark the K37 audit, packer and rebuild write: one
`EFS_KV_KIND_PACK_DEGRADED` (249) row per segment, through Raft tag 247
`EFS_MD_CMD_PACK_DEGRADED` op MARK, reason `EFS_PACK_DEGRADED_R_RECOVERY`.
No second authority, no new KV kind, Raft tag or wire type. Recovery does
not wait for or call the audit, and the audit cannot see the fragment
first: a member answers the audit only once its stores are settled (fence
cleared), and by then recovery has committed the mark, so the audit's sweep
skips that fragment (its collector skips fragments already marked). Both
paths OR bits into the one row; a second bit from either means
unrecoverable.

| Piece | What it does |
| --- | --- |
| `server_segment_mark_commit` (`segment_rebuild.c`) | The one MARK proposal (`sr_propose`), from any member: `host_bg_propose` forwards it to the group's leader when this node does not lead it; the group's apply verdict comes back. `server_segment_mark` (packer, audit, rebuild) is now that call plus its local-replica check. |
| `rv_segment_ref` (`recovery.c`) | For each referenced fragment (ACTIVE segment with switched files placing a fragment here, from the K36 segment inventory) that is missing, torn or mismatching: dropped (as before), then `rv_seg_mark`. |
| `rv_seg_mark` | Reads the segment and its mark mask at a linearizable point (`server_raft_host_pack_state`: ReadIndex locally or on a hosting voter). Segment gone or without live files: `superseded`. Bit already set: `already_marked` (a crash after the mark and before the fence, or a packer `R_WRITE` mark). Otherwise commit the MARK, then read it back; the bit must be there. A transient verdict (no leader, peer down) retries with the reason shown; an unreadable mark row fails recovery closed. |
| Fence | Unchanged order: the segment inventory runs before `server_put_sync_recovered()`, and `rv_seg_mark` returns only once the mark is read back committed (or the segment is proven gone), so `recovery_uncertain` clears only after every dropped segment fragment is durably marked, exactly as for chunk stripes. |
| Second loss | If the mask read back has another bit (another fragment already lost), the segment is reported `UNRECOVERABLE` and counted; it is never healed (K37's rebuild keeps a two-bit mark and reports it). After the fence the watch pass reports a mark that gains a bit the same way. |
| Watch (after the fence) | `rv_seg_watch_pass`, beside the chunk watch: a cleared mark whose local copy verifies (`server_segment_check`) counts `rebuilt`; a cleared mark without a verified copy here is marked again; a segment no longer live is `superseded`. |
| Status | `struct efs_recovery_stats` gained `segments_marked`, `segments_already_marked`, `segments_superseded`, `segments_unrecoverable`, `segments_rebuilt`, `segments_waiting`; `efs-mgmt sync-status` prints `segments checked= missing= marked= already_marked= superseded= unrecoverable= rebuilt= waiting=`. Fresh format only (the reply size changed; a mismatched `efs-mgmt` refuses the reply). |
| Fault point | `EFS_FAULT_CRASH=recovery-segment-marked` (`EFS_FAULTS` builds only; A6 row updated): exit 137 right after a segment MARK commits. |

## Test

`tests/test_k36_segment_recovery.py` (`make test-long`): three efsd
(`tests/faults/efsd`) and efs-fuse on loopback, packer minimum age 500 ms,
scheduled compaction moved out of the run.

| Step | Checks |
| --- | --- |
| 1 baseline | two batches of 8 small files (1 B–16 KiB) packed; no mark |
| 2 crash a member | node 3 restarted with `EFS_FAULT_CRASH=put-acked`, exits 137 after acknowledging a chunk PUT of a 2 MiB file; one of its segment fragments deleted, another truncated to 4 KiB; every packed file reads back from the peers |
| 3 crash during recovery | restarted with `EFS_FAULT_CRASH=recovery-segment-marked`: exits 137 right after its first segment mark; exactly one `not proven here; dropped` line; the torn file dropped (`store: recovery dropped …`) |
| 4 fence held until marked | restarted with `EFS_FAULT_RECOVERY_HOLD_FILE`: parked at `phase=fence` with `segments missing=2 marked=1 already_marked=1 unrecoverable=0 waiting=2`; `efs-mgmt status` on node 1 shows `Degraded segments: 2` while node 3 is still fenced (no audit can have marked them); no audit mark line for node 3; data-commit to node 3 refused |
| 5 rebuild | fence released: `recovery: verified`, data-commit OK; rebuild released (`EFS_FAULT_SEGMENT_REBUILD_HOLD`): `Degraded segments: 0`, both fragment files back at full length, `segment-rebuild: rebuilt … fragment f on node 3` once each, node 3's recovery `rebuilt=2 waiting=0`; still no audit mark for node 3 |
| 6 read back and outages | no degraded chunk or segment; every file exact through a fresh mount; node 1 stopped (mount via node 2) then node 2 stopped (mount via node 3): every file exact each time, which needs node 3's rebuilt fragments |
| 7 second loss | rebuild held; a new batch of 4 packed; node 3 `SIGKILL`ed, its fragments of the new segments deleted, its recovery marks them (`marked=n`); node 2 `SIGKILL`ed, its fragments of the same segments deleted, its recovery reports each `UNRECOVERABLE` (`marked=n unrecoverable=n`); released: K37 reports each `UNRECOVERABLE`, `Protection: LOST`, no `rebuilt` line for them after 5 s; their files read `EIO`, every other file exact; removing them retires the marks (`Protection: full`) |

`tests/test_k36_recovery.py` step 11 (a referenced segment fragment deleted
on a member of a four-node cluster) now also requires the mark and waits for
K37's rebuild (`segments … rebuilt≥1 waiting=0`).

Development run 1 on `8cf5dc970` (intel1): **all 7 steps PASS in 68.2 s**.
Its daemon log lines show the order: node 3's recovery
`… fragment 0 not proven here; dropped`, (crash), then `fragment 0 already
marked degraded`, `fragment 2 marked degraded`; the group leader
`rebuilt g0 id=e1a1b89ad026d101 gen=1 fragment 0 on node 3 (from nodes
1,2)` and `… e1a1b8ab463cf701 gen=1 fragment 2 …`; node 3
`rebuilt here and verified` for both; in step 7 `segment-rebuild: g0
id=e1a1c4cc90bdbd01 gen=1 UNRECOVERABLE: fragments mask=5 lost
(reasons=10)` — reasons `0x10` is `R_RECOVERY` alone: both bits came from
the two recoveries, none from the audit.

Regression runs on `6c260b59f` (same tree as `ad7fe1ded`, which only
rewrote the commit message): `tests/test_k36_segment_recovery.py` PASS
(131.5 s), `tests/test_k36_recovery.py` PASS (67.9 s, step 11 now waits for
the rebuild), `tests/test_k37_segment_rebuild.py` PASS (195.2 s).

## Gate

intel1 (8 CPU, Rocky 10, loopback TCP, buffered test stores under the
tree's `tmp/`), private tree `~/build/k36seg-gate` from `git archive
ad7fe1ded`, `EFS_GIT_ID=ad7fe1ded scripts/make_test_summary.sh` (default
`test-all`, one job) in `screen`:

- `TOTAL 468 FAIL 2 NOTRUN 15 STATUS FAIL (1999s) BUILD 23s`.
- In it: `tests/test_k36_segment_recovery.py` PASS (66.5 s),
  `tests/test_k36_recovery.py` PASS (68.3 s), `tests/test_k37_segment_rebuild.py`
  PASS (161.7 s), `tests/test_kv_kind_unique.py`, `tests/test_env_inventory.py`,
  `tests/test_shutdown_bounded.py` PASS.
- FAIL 1, `tests/test_get_integrity.py`: its generated stub source no longer
  compiles (`redefinition of 'read_reprobe_down'`, `-Werror`); this change
  does not touch the client read path or the test. **Fails on the base**
  `646a43041` the same way (1 of 1).
- FAIL 2, `tests/test_b6_unavailable.py`: mode `down` passes, then mode
  `refuse` asserts `packed_during_fault == 0`, and the packer packed 1–3
  files while node 3 refused every PUT (K37's policy: a segment copied with
  two acknowledged fragments is marked and switched). This change does not touch the packer.
  **Fails on the base** `646a43041` 2 of 2 (`packed_during_fault` 2 and 3)
  and on the branch 2 of 2 more (1 and 2). B6's expectation and K37's
  packing policy disagree; a follow-up for B6, not K36/K37 recovery.
- NOTRUN, all host limits: no `~/git/cluster`, no valgrind (memcheck, leak
  gate), no RDMA device (declaration gate RDMA path, `test_rdma_xprt`,
  seven `test_rdma_segmented` modes), `test_follower_bound` compactor case
  (needs a whole `EFS_FAULTS=1` build), `test_query_first_catalog.py`'s
  UNFORMED race not occurring, and the runner's own self-test.
- Docs gate: `cd docs && python3 gen-architecture-full.py` and
  `python3 docs/check-architecture.py`: all 5 checks passed.

## Limitations

- One host on loopback TCP: three daemons are not three machines.
- The recovery inventory covers segments with switched files. A segment
  still pending (copied, not yet switched) whose fragment here is dropped is
  not marked by recovery; the packer switches only after three (or two plus
  an `R_WRITE` mark) acknowledged, fsynced PUTs, so this needs storage that
  loses fsynced data. Such a fragment is found when a rebuild needs it as a
  source or by a later audit, not at recovery.
- Rebuild waits until all three placement members are available (K37); a
  member that never returns leaves the mark visible (no re-placement, W74).
- A segment rewritten between the inventory read and the mark is
  `superseded` (STALE apply on a non-ACTIVE segment); no test drives that
  race.

## NOTRUN

Multi-host, RDMA, orcd-mini, power-loss qualification, five or more members
(segments on storage-only members), many degraded segments at once.

## Reproduce

```bash
git archive <commit> | ssh intel1 'mkdir -p ~/build/k36seg && tar -x -C ~/build/k36seg'
ssh intel1 'cd ~/build/k36seg && export TMPDIR=$HOME/tmp-k36seg && mkdir -p $TMPDIR && make -j8 EFS_GIT_ID=<commit> all tests/faults/efsd && python3 tests/test_k36_segment_recovery.py'
```

Prerequisites: `/dev/fuse`, `fusermount3`, Python 3; ports chosen by the
test (never 17xxx). About 70 s. `EFS_K36S_KEEP=1` keeps the work directory
under `TMPDIR` (delete it afterwards); otherwise the test removes it. The
gate is `scripts/make_test_summary.sh` (`make test-all`) in the same tree.

## Raw outputs

Kept on intel1 under `~/efs-artifacts/k36-k37-segments-20261024/` (not in
Git), 22 files, 220 KB, `SHA256SUMS` sha256
`9817196bf350337fa47e49fafd9512d74d4d4a2be80004cff8c60d62cd0fb7fd`: the
gate's `summary.txt`, `progress.txt`, `timings.tsv`, `cmds.txt`, runner
output and build warnings, both failure logs; the base runs of the two
failing tests and the branch's extra `test_b6_unavailable.py` runs; the
development run's output and its recovery / segment-rebuild daemon log
lines (`dev1-*`); the three regression runs (`reg-6c260b59f-*`). The private
trees (`~/build/k36seg*`, `~/tmp-k36seg`) were deleted.
