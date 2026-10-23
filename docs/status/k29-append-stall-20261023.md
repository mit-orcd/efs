# K29 — append stall under load: cause and fixes (Oct 23, 2026)

**Verdict: cause established from the code and the K15/nuc summaries; two
fixes with failing-first tests; `make test-all` PASS on xorinox (exit 0, no `make` Error lines).
The load verification is NOTRUN (the parent session's, on orcd-mini).**

- **Purpose:** alpha 1.0 checklist row K29 ([checklist](alpha-1.0-2plus1-checklist.md)):
  under sustained `O_APPEND` load with no leader change, both K15 lanes fell
  from 10 to about 0 records/s for 10 minutes, REPORT and APPEND_RESERVE took
  up to 30 s, `txn-recover` aborted 10–17 s old transactions, group 0 grew
  about 57 Raft entries per 4 KiB record, followers rejected AppendEntries
  `prev-beyond-log` throughout, and leader `apply-sleep` reached 7 s
  ([K15 live gates](k15-live-gates-20261022.md); the nuc W148 section of
  [gates-nuc](gates-nuc-20261022.md) shows the same class). Raw logs of
  both runs were deleted; this analysis uses their summaries only.
- **Tested source:** branch `claude/devel-k29-2185a1` on devel `757d76844`,
  commits `1754cf9be` (Raft probe) and `438d613d9` (TXN_BATCH). The gate tree
  was `git archive 438d613d9`, built with `EFS_GIT_ID=438d613d9df7
  EFS_GIT_BRANCH=devel-k29`. sha256: `efsd`
  `5ff23bcb7f4aeeedff30b54512b8303d995f534774b52badf26b93cd587dee1d`,
  `efs-fuse` `7ea9a6de9cf12879ed4432e72c3b9b5f82cbdbce0eb3fe1008c41e6015c17b2a`,
  `efs-mgmt` `57ab8b3ed78af0adaefb9a33581672b00f425e4b01f1d9f6f2367202b7096779`,
  `libefs.a` `efc9b381ae3e2a6d67c89926639cdb79a1ae61b294be47402014c57d2922f7bd`.
  The later commit that adds the no-retirement columns to
  `scripts/k29_append_entry_count.py` changes no built code or test.
- **Environment:** xorinox, Rocky Linux 10.2, kernel 6.12.0-211.67.1, 16 CPU,
  60 GB, gcc 14.3.1, Python 3.12.15. Work tree under `$HOME/efs-k29`,
  `TMPDIR=$HOME/efs-k29/tmp`. In-process tests only: no daemon cluster, no
  load run, no RDMA. The user's FUSE client at `/data1/efs/intel-mnt` and
  ports 17xxx were not touched.

## Causal chain

1. **Lanes grow with the file.** Chunks are 128 KiB
   (`include/efs/common.h:28`) and a chunk's writer lane is its index modulo
   64 (`src/meta/put_ticket.c:42`, `efs_meta_get_lane_writer_view`). A stream
   of 4 KiB records therefore activates a new lane every 32 records and all 64
   lanes by record 2,048. Lane shards alternate parity
   (`efs_kv_lane_shard`, odd stride, `src/kv/kv_key.c:39`), so the lanes split
   evenly between groups 0 and 2 (`efs_raft_shard_group`,
   `include/efs/raft.h:99`).
2. **Every reservation prepares every active lane.**
   `efs_meta_capture_append` (`src/meta/meta_apply.c:8190`) captures one exact
   image per active lane (each raises that lane's append barrier to the new
   watermark), plus inode, cursor, receipt and reservation;
   `efs_meta_execute_append_owned` (`src/meta/append_plan.c:54`) adds the
   session-owner image; `efs_meta_execute_append`
   (`src/meta/meta_apply.c:8336`) prepares each image separately. The lane
   images are required (W52/W97 notes in
   [nuc-w52-w97-resume](nuc-w52-w97-resume-20261011.md): they change the
   barrier and cannot be dropped).
3. **The host gave each image its own Raft entry.** `host_append_transaction`
   (`src/server/raft_host.c:16264`) takes the one-entry APPEND_ATOMIC path
   only when every image is on one shard (`host_append_atomic_eligible`,
   `:16144`), which holds only while lane 0 is the only lane (the first 32
   records). After that, `host_append_execute_batched` (`:16205`) proposed
   the session and inode guards synchronously and then every other image
   with its own `host_propose`; on a non-solo leader each proposal takes its
   own log hold and waits for its fsync (`host_propose_impl`, `:5271`, the
   wait at `:5384`). Then one DECIDE, one RESOLVE per participant shard
   (`host_resize_resolve`, `:15893`, a cohort of 8 sharing a hold) and the
   finisher's RETIRE.
4. **Completion repeats it.** REPORT runs `host_resolve_caught_up`
   (`:19633`): one APPEND_CATCHUP (or COMPLETE_ATOMIC) entry, then, when no
   reservation remains open, `efs_meta_retire_append_barriers`
   (`src/meta/append_plan.c:375`), whose collector
   (`src/meta/meta_apply.c:8778`) clears the barrier on every active lane: a
   second transaction of the same size. A sequential writer retires after
   every record; concurrent writers retire whenever the open count reaches 0.
5. **Entries per record (counted).** `scripts/k29_append_entry_count.py` runs
   the real capture, execute, catch-up and retirement code on an in-memory
   KV, counts every PREPARE/DECIDE/RESOLVE per group and maps them onto the
   host's entries. It excludes data publication, PUT tickets and session work,
   which do not depend on the lane count. For an inode on group 0:

   | active lanes | group 0 before | group 0 after | group 2 before | group 2 after | group 0 before, no retirement | group 0 after, no retirement |
   | --- | --- | --- | --- | --- | --- | --- |
   | 1 | 9 | 8 | 0 | 0 | 2 | 2 |
   | 2 | 15 | 11 | 6 | 6 | 10 | 7 |
   | 8 | 27 | 11 | 18 | 6 | 16 | 7 |
   | 16 | 43 | 11 | 34 | 6 | 24 | 7 |
   | 32 | 75 | 11 | 66 | 6 | 40 | 7 |
   | 64 | 139 | 11 | 130 | 6 | 72 | 7 |

   Before the fix, group 0 costs 11 + 2L entries per record with the
   retirement and 8 + L without (L ≥ 2). Over the K15 run's first 2,120
   records the mean lane count is 33.6: about 78 entries per record with a
   retirement every record and about 42 with none. The measured 57 lies
   between, as expected with four writers that sometimes overlap. This
   corroborates the mechanism; it does not reproduce the run.
6. **Why the rate collapsed rather than slowed.** An APPEND_RESERVE (71) or
   REPORT (67) RPC performed its O(L) proposals one after another, each
   waiting for an fsync of the shared Raft log while the other writers'
   entries queued in front of it. At L ≈ 44–53 (records 1,400–1,700, where
   the K15 lanes stalled), one RPC was about 50–60 sequential fsync waits plus
   the apply waits; at 50–500 ms per wait on a loaded host that is 3–30 s,
   matching the slow-ok p50 of 3–4 s and the 30 s receive timeouts. This step
   is inference: the fsync latencies of the run were not recorded.
7. **`apply-sleep`.** `host_wait_applied` (`:4401`) logs a proposer that
   waited more than 20 ms for its index to apply. Each entry is applied in its
   own KV apply scope and committed with its cursor (`host_apply`), so the
   leader's apply cost also scales with entries per record, and a KV flush
   backlog defers apply (`efs_kv_lsm_apply_room`; nuc `kv-flush files=139`,
   L0 142; W126). Behind O(L)-entry records from four writers, waits of 77–88
   ms p50 and seconds at the tail follow.
8. **`prev-beyond-log` throughout (a Raft defect, independent of load).**
   The host outbox sends from its priority lane (heartbeats and empty
   AppendEntries) before its entry lane (`host_sender_pick`, `:841`).
   `send_commit_probe` (`src/raft/raft.c:888`, before the fix) named the end
   of the still-queued batch as `prev_index`, so the probe reached the
   follower first and was rejected `prev-beyond-log` (`raft.c:1706`). The
   leader's rejection path (`raft.c:1945–1990`) cleared the window, rewound
   `next_index` to the follower's tail and rebuilt the batch when the old one
   had already left the outbox. This repeated on every commit advance: wasted
   bandwidth and follower log reads, and reject lines, but no safety effect
   (Log Matching still held).
9. **BUSY and the 10–17 s recoveries.** A proposal is refused BUSY by leader
   admission at 3/4 of the unapplied cap or KV apply room (`host_admit`,
   `:5246`), and an apply wait gives up after 400 ms (`HOST_READ_TRIES` ×
   `HOST_TICK_US`, `:81`). The reservation then takes its ABORT path; an ABORT
   that itself returns BUSY is handed to the finisher (`host_fin_add`,
   `:9059`, a bounded queue). Until the abort lands, the transaction's
   PREPARE intents sit on the file's inode, cursor and lane keys, and every
   later reservation or completion on that file meets them and returns BUSY.
   `txn-recover` acts on intents older than 5 s (`HOST_REC_AGE_NS`, `:9260`;
   `host_txn_recover_one`, `:9313`); its own DECIDE ABORT met the same
   pressure (`decide-abort rc=-13 (retry next pass)`), so the transactions
   aged to 10–17 s while the file stayed blocked. The summaries cannot tell
   whether a proposer was still running when recovery acted: before the fix
   a reservation's 70 sequential proposals could exceed 5 s on their own.
   After the fix a reservation is at most 7 sequential proposals.
10. **Client errors.** The 30 s receive timeouts (REPORT, APPEND_RESERVE),
    opid-admission BUSY, EBUSY writes and the BUSY abandons are the client
    side of 6 and 9. W155 (ambiguous EIO after these timeouts) is the
    consequence, not a separate cause.

## Fixes

**F1 — Raft: a commit probe claims only the acknowledged prefix**
(`1754cf9be`, `src/raft/raft.c` `send_commit_probe`). The probe's
`prev_index` is now the follower's `match_index`, which the follower has
acknowledged, so it is valid whichever lane delivers it first. The follower
commits only through that prefix; the batch's own acknowledgement triggers
the next probe (existing `send_ae(…, 1)` path). Test:
`test_commit_probe_overtakes_batch` in `tests/test_raft.c` delivers empty
AppendEntries at once and holds entry batches, as the two-lane outbox does.
Before the fix it fails (3 checks: the follower and the leader each count
rejections); after it passes, and the slow follower applies every committed
entry once the batches arrive. The whole `test_raft` passes in both modes.

**F2 — host: one TXN_BATCH entry per group for a transaction's PREPAREs
and RESOLVEs** (`438d613d9`). New log command `TXN_BATCH` (86,
`include/efs/meta_cmd.h`): `[86][sub][n:2]([len:2][command])*n`, `sub` is
PREPARE or RESOLVE, at most 96 commands, all for the entry's group.

- Apply (`apply_txn_batch_cmd`, `src/server/raft_host.c:2944`) checks the
  whole frame first: a malformed frame is UNSUPPORTED (the group halts, as for
  any undecodable entry) and a command for another group's shard is refused
  INVAL; neither applies anything. Then every command is applied by
  `apply_txn_cmd` exactly as its own entry would be, in order, inside the
  entry's single atomic KV scope. The verdict is the first non-OK command
  verdict, which is all `host_wait_refs` ever reported for a set of
  references; IO, NOMEM and UNSUPPORTED stop at once and the whole entry
  retries. The apply policy treats TXN_BATCH like PREPARE/RESOLVE (business
  verdict advances with the ring verdict, storage failure retries), and a
  RESOLVE batch keeps RESOLVE's admission exemption.
- Proposal (`struct host_txn_batch`, `:15411`): reservations and barrier
  retirements keep the two synchronous guards; the other images are buffered
  per group and proposed at the decision, one entry per group, and an ABORT
  proposes none of them. `host_txn_commit` and `host_resize_resolve` propose
  one RESOLVE entry per group inside their existing cohort. A group with a
  single command proposes that command unchanged.
- Effect (table above): per record 11 group-0 and 6 group-2 entries at any
  lane count, instead of 139 and 130 at 64 lanes; an RPC makes at most about
  7 sequential proposals instead of about 70. The KV work per record is
  unchanged: every lane image is still prepared and applied (see P3).

Tests (all in `make test`):

| Test | Checks | Before → after |
| --- | --- | --- |
| `tests/test_txn_batch_apply.py` (new) | production `apply_txn_cmd`/`apply_txn_batch_cmd` over the real txn layer: the same KV as single entries after PREPARE, after a conflicting PREPARE (first verdict, other commands applied) and after DECIDE + RESOLVE batch; IO stops at the failing command; malformed frames UNSUPPORTED; foreign shard INVAL; nothing applied in either | the command does not exist before |
| `tests/test_append_prepare_batch.py` | 64-lane reservation: 2 PREPARE entries (34 + 33 images, each once); a caller's cohort is not used; every applied verdict before COMMIT; ABORT proposes nothing; failure paths | negative control `EFS_APPEND_BATCH_PER_IMAGE=1` (the pre-fix per-image proposal) fails `proposals==2` with 67 |
| `tests/test_prepare_cohort.py` | 144-part RESOLVE: one entry per group, every shard once in part order, split at the cohort item size, single command unchanged, failure/finisher paths | asserted 144 entries before |
| `tests/test_txn_apply_retry.py` | TXN_BATCH apply policy | TXN_BATCH added |

`test_submit_unknown_tag.py` (applied-tag table vs. dispatcher) and
`test_apply_unsupported.py` pass unchanged.

## Not fixed: proposals and the measurement for the parent (orcd-mini)

- **P1 — verify F1+F2 under load (required before K29 can close).** Run the
  K15 W52 workload one lane at a time on a three-daemon orcd-mini cluster, at
  devel `757d76844` (before) and at this branch (after), same transport and
  I/O mode:
  `python3 tests/live/append_failover.py --rel $PWD --root $R/w52 --port <p> --records 20000 --writers 4`.
  Sample every 30 s: visible records, and `efs-mgmt raft-status` on the
  group-0 and group-2 leaders (group `last`, `commit`, `applied`,
  `rejects_sent`, per-peer `rejects`/`busy`, `admit_busy`,
  `kv_apply_deferred`). Count in the daemon logs `apply-sleep`, `AE rejected
  by peer` and `txn-recover`, and in the client log `inode-rpc: retry
  type=67|71 why=recv`. Expected after: group-0 entries per record roughly
  constant (about 11–20 including publication and tickets) instead of growing
  with the record count to about 57 on average; no collapse after record
  1,400; zero 30 s receive timeouts; `rejects_sent` and `AE rejected` near
  zero; `apply-sleep` p50 well under the before value. Then rerun the
  lost-reply gate (`--lose-reserve-every 97 --lose-reserve-max 4`); it should
  show exactly the injected failures.
- **P2 — recovery age vs. live transactions.** If the after run still shows
  `txn-recover … -> ABORT` lines, record for each the transaction age and
  whether its proposer was still running (a COMMIT that later fails PROTO).
  Only if live transactions are aborted, register in-flight txids on the
  proposer and skip them in `host_txn_recover_one`; this is unit-testable but
  not supported by the present evidence.
- **P3 — KV work still O(lanes) per record.** Each record still writes every
  lane image twice (barrier raise, barrier clear). Measure leader apply time
  per entry (`EFS_WRITE_PROF`, `obs_apply_cycle_us`) at 64 lanes. Reducing it
  needs a barrier design that touches only the receiving lane and retires
  lazily; the W52 notes rule out simply dropping lane images, so this is a
  design item, not part of this fix.
- **P4 — other per-image paths.** Append pruning and last-close cleanup
  (`host_append_batch_prepare`) still propose one entry per image inside a
  cohort; they are not per record. Converting them is mechanical if P1 shows
  them.
- **P5 — harness.** The K15 harness counts every non-injected failure and its
  hole as a mismatch; it needs a rule for abandons caused by real timeouts
  (K15 page).

## Gate

`make test-all` on `git archive 438d613d9`, xorinox, 18:00:05–18:04:30Z
(build 14 s, tests 4 min 11 s): **exit 0 (`TESTALL_RC=0`), 0 lines matching
`^make(\[[0-9]+\])?: .*Error`, 420 test commands.** 14 NOTRUN lines, all
environmental: RDMA (no IB device: `test_rdma_xprt`, eight
`test_rdma_segmented` modes, the declaration gate's RDMA path), cluster
stop/remount scripts (`~/git/cluster` absent), `test_stalled_compactor`
(needs `EFS_FAULTS=1`) and the leak gate (valgrind absent). The `FAIL` and
`journal verification FAILED` lines in the log are the expected output of
negative controls. Raw log, kept outside Git:
`xorinox:~/efs-artifacts/k29-20261023/make-test-all-438d613d9.log`, sha256
`78e1d6c98b0c76496defad95d75884759291b078f265ecf5c758c7b917c02f52`.

## Reproduction

```
git archive 438d613d9 | ssh xorinox 'mkdir -p ~/efs-k29/gate && tar -x -C ~/efs-k29/gate'
cd ~/efs-k29/gate && export TMPDIR=$HOME/efs-k29/tmp
make -j16 EFS_GIT_ID=438d613d9df7 EFS_GIT_BRANCH=devel-k29
make test-all EFS_GIT_ID=438d613d9df7 EFS_GIT_BRANCH=devel-k29
grep -E '^make(\[[0-9]+\])?: .*Error' <log>      # expect nothing
python3 scripts/k29_append_entry_count.py           # the table above
EFS_APPEND_BATCH_PER_IMAGE=1 python3 tests/test_append_prepare_batch.py   # must fail
```

The F1 failing-first check: build `tests/test_raft` from `757d76844`'s
`src/raft/raft.c` with this branch's `tests/test_raft.c`; it reports 6
failures (3 per mode) in `test_commit_probe_overtakes_batch`.

## Limitations and NOTRUN

- NOTRUN: every load, cluster, RDMA and orcd-mini run (P1). The stall's
  fsync and apply latencies were never recorded, so step 6 is inference.
- The entry count uses the metadata code on an in-memory KV and a mapping of
  its callbacks onto the host's proposals; the host functions themselves are
  tested by the extracted-function tests above, not in a running daemon.
- A TXN_BATCH entry is applied in one KV scope; a batch of 67 lane images is a
  few hundred keys, far inside the 32,768-key scope limit.
- No backward compatibility: a binary without tag 86 refuses the entry and
  halts the group (W127/W140), as for any unknown command.

Cleanup: the gate screen session ended by itself; `~/efs-k29` (source, gate
tree, build and test temporaries) was deleted on xorinox. Only the gate log
above remains, in `~/efs-artifacts/k29-20261023/`.
