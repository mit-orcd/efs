# K9 / W87 append interleavings and the K15/K29 accounting rule (Oct 24, 2026)

**Purpose.** Two alpha 1.0 Stage 1 append items from
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md):

1. **K9 / W87** (buffered concurrent append loses records under load; never
   reproduced in 23 + 110 oracle rounds). Work by analysis: trace the
   buffered `O_APPEND` path, list every interleaving that could drop or
   duplicate an acknowledged record, force each one deterministically in
   `make test` / `make test-long`, fix what fails.
2. **K15 / K29** (the append harness counted every non-injected failure and
   its hole as a mismatch, so a load stall with no leader change and no data
   loss FAILed): add a rule that accounts a failure the client saw, and
   keeps a lost acknowledged record or an unexplained hole a mismatch.

**Verdicts.**

| Item | Verdict |
| --- | --- |
| K9 / W87 | **No defect reproduced.** Every forced interleaving kept the exact record set. No source fix. **W87 stays OPEN; K9 is not closed.** The oracle soak under the original load is **NOTRUN** (parent session). |
| K15 / K29 accounting rule | **PASS (scoped).** Rule in `tests/live/append_failover.py` (`account()`), unit check with negative controls in `make test`. The live failover gates are **NOTRUN** (parent session). |
| Code gate | `make test-all` on amd: 445 commands, 10 FAIL, all of which also fail on the untouched base (amd host limits); the 3 new commands PASS ([Gate](#gate)). |

## Tested source and build

- Branch `claude/devel-k9-append-836bbe` on devel `19313ab06`. Code and tests:
  commit `fcb47f2bd`; this page and the status rows: the commit after it.
- No product source change. The only source edits are three `EFS_FAULTS`
  barrier points (below) that exist only in the fault build
  `tests/append_faults/efs-fuse`; `./efs-fuse` and the daemons are unchanged.
- No new KV kind, Raft tag or wire message type.
- Builds on amd from `git archive` trees under `~/k9w/` (`EFS_BUILD_ID=unknown`,
  gcc, x86_64, `-O3 -march=native`).

## Environment

amd (`amd-395`): 32 CPUs, Ubuntu, kernel 7.0, `/tmp` on tmpfs, `/dev/fuse` and
`fusermount3` available. Private loopback daemons on free ports (never
17xxx/27xxx), TCP, buffered I/O (`--no-direct-io`), quota 2 GiB, stores
under `$TMPDIR`. No running cluster, mount or `efs` user was touched. Another
session's W123 campaign ran on amd during the gates (load average about 6).

## K9: the buffered O_APPEND path, end to end

One FUSE `write(2)` on an `O_APPEND` descriptor (`efs_fuse.c`):

1. `append_publish_burst`: after 32 writes or 1 s of unpublished ownership on
   this mount, publish first (the same flush + REPORT as fsync), then leave a
   100 ms handoff window for a peer.
2. Take the inode's append stripe (`append_mu`, 64 stripes), then
   `append_reserve_offset`: allocate a receipt identity
   (`efs_client_append_begin`), `APPEND_RESERVE` to the inode group in a BUSY
   loop with the same identity. The server (`efs_meta_capture_append` /
   `efs_meta_execute_append_owned`, one exact-image transaction with the
   owner's session row, K29 TXN_BATCH) refuses BUSY while another session
   holds OPEN reservations, else grants `[eof, eof+len)`, records the receipt
   and raises the lanes' append barrier. A failed attempt that was sent is
   abandoned under the same live session (`append_abandon_identity`, K15/W64);
   the identity is then released.
3. Reflect the reserved size locally (not dirty: the barrier must not
   release before the bytes are PUT), pin, patch the bytes into the dcache
   (`efs_dcache_try_patch_sparse`: RMW onto the published base when the chunk
   exists, ranges kept for STALE overlay) or the write-back pool, release the
   stripe, return `size`. The record is acknowledged here.
4. Publication: fsync / close / burst / pressure run `efs_append_flush_report`
   under the same stripe: write-back drain, dcache PUT, REPORT (K32 batches
   consecutive units of one file per transaction). The REPORT completes the
   OPEN reservations (`efs_meta_finish_append_batch_owned`, decide-once), the
   frontier advances and the cursor folds into `base_size`.
5. Cleanup: an OPEN row ends COMPLETED (REPORT), ABORTED_HOLE (live owner's
   abandon, or last-close drain when no lease is left), or FENCED_HOLE
   (session fence cleanup). A completion over a retired row is refused;
   a retirement never overtakes a completion.

### Interleavings that could drop or duplicate an acknowledged record

| # | Interleaving | Where it is forced | Result |
| --- | --- | --- | --- |
| I1 | Concurrent writers, one mount: two reservations of one owner completed in any order, singly or in one batch | model (`test_append_interleave`), live S1/S6 | exact |
| I2 | Concurrent writers, two mounts: a foreign reservation while one session holds OPEN rows | model (BUSY), live S2 (peer waits for the unpublished reservation, then appends after it), S6 | exact |
| I3 | Flush racing a new append on one mount: a write parked after its grant (stripe held, bytes not cached) while a per-record open/write/close writer, an fsync of another descriptor and a read-only open/close run | live S1 (`reserved` barrier) | exact; the others wait for the stripe |
| I4 | Leader change between PUT and REPORT, with a peer waiting | live S2 (`published` barrier, both groups step down) | exact; REPORT goes to the new leader |
| I5 | Leader change mid-reservation: coordinator lost after DECIDE (COMMIT in the log, unresolved) or after PREPARE (no decision, recovery ABORTs) | model (decided-in-flight and prepared-only transactions, then recovery) | replay returns the granted offset; an aborted grant is re-reserved |
| I6 | Lost reserve reply: replay with the same identity, or abandon | model, live S3 (`lose` barrier + step-down of both groups before the abandon) | replay = same offset; abandon retires exactly one hole; offset never learnable afterwards |
| I7 | Abandon racing completion | model; `test_append_cleanup` (decide-once) | the first resolution in the log wins |
| I8 | Session fence with granted and lost identities | model (fence + `append_session_next` / `finish_append_fenced` cleanup, re-register next epoch) | OPEN rows become FENCED_HOLE; late completion refused (FENCED); acknowledged unpublished writes surface as a failed fsync, never silently |
| I9 | Client restart (new session) | live S4 (unmount/remount mount B, append again) | exact |
| I10 | Leader restart with acknowledged records only in the client cache | live S5 (group-0 leader stopped, leadership moves, restart, fsync) | exact |
| I11 | Append-row pruning interleaved with new reservations, and shrink/reset rows | model (prune between steps); `host_append_transaction` prunes before capture and `capture_append` refuses while prune is due | exact |
| I12 | GC / packing of the tail chunk racing a new publication | `test_pack_switch` (exact-source CAS: a switch after a newer publish is STALE); live tests run with `EFS_GC_DISABLE=1` | covered by the existing CAS test; the GC-load soak is NOTRUN |
| I13 | Last-close drain of OPEN rows | reasoning: `host_lease_close_effects` drains only when no lease is held on the inode by any session; a writer holds its lease while its descriptor is open and its FLUSH publishes first | no path found where an acknowledged write's row is drained while its owner still holds the bytes and gets no error |
| I14 | `close()` that returns early because nothing is unpublished while another thread of the same mount is between grant and patch | reasoning + live S1 | safe: that thread's own close/fsync or the next burst publishes its bytes |

### Tests added

- **`tests/test_append_interleave.c`** (`make test`, 0.13 s at 400 seeds):
  two owners, seven numbered writes each with distinct lengths, 120 seeded
  steps per seed over the real `meta_apply.c` / `append_plan.c` code on a
  memory KV. Events: reserve (delivered, reply lost, decided-in-flight,
  prepared-only), replay, abandon, recovery (COMMIT resolve / ABORT), REPORT
  completion (one or a batch of up to 8), session fence with cleanup and
  re-registration, prune. Checks after every event and at the end: replay
  offset equals the grant, grants never overlap, per-owner offsets grow in
  issue order, a live owner's completion of an acknowledged write is never
  refused and its row says COMPLETED, holes only from abandon or fence, the
  settled file tiles `[200, size)` exactly with `nopen == 0`. Negative
  control: the tiling checker refuses the same file with one completed
  record removed. 400 seeds in `make test`; **50,000 seeds also PASS (14 s)**:
  busy 1,059,369, granted 302,587, lost 27,468, replayed 27,704, abandoned
  251,443, decided-in-flight 27,230, prepared-only 27,228, recovered 54,458,
  completed 312,095, fenced 136,392, pruned 175,240, negative controls 49,671.
  `K9_TRACE=1 ./tests/test_append_interleave 1 SEED` traces one seed.
- **`tests/append_faults/efs-fuse`** (built by `make test` / `test-long` as a
  special binary): `efs_fuse.c` and `inode_rpc.c` compiled with
  `-DEFS_FAULTS=1`. `EFS_FAULT_APPEND_BARRIER=DIR`: the first arrival at a
  point while `DIR/POINT-arm` exists creates `DIR/POINT-entered` and waits
  (at most 60 s) until the arm file is removed. Points: `reserved` (granted,
  size reflected, stripe held, bytes not cached), `published` (dcache PUT
  done, REPORT not sent, stripe held), `lose` (granted reply dropped, before
  the abandon).
- **`tests/test_append_interleave_live.py`** (`make test-long`, about 85 s):
  three daemons, mount A on the fault client, mount B on the normal client,
  4 KiB records with a keyed digest (`append_failover.body`), scenarios S1–S6
  as in the table, then warm A, warm B and cold (all daemons and clients
  restarted) checks with `append_failover.verify` and the accounting rule.
  NOTRUN (exit 0) without `/dev/fuse`, `fusermount3` or the fault client.
  Results on amd: **PASS in every run of the final test** (run 10 before
  the cold wait was raised to 240 s, then the committed tree inside
  `make test-all` and twice standalone): each time 101 acknowledged records,
  1 failed write (the injected lost reply), 1 retired hole, and one sha256
  shared by warm A, warm B and cold (it differs between runs, because S6's
  thread interleaving varies). Earlier runs failed only on test-harness
  issues, fixed before the commit: parked main-thread open/close, fork+exec
  closing descriptors (observation 1), the cold-mount retry, and S5's
  SIGKILL (observation 2).

### Observations (not K9, for the parent)

These came up while building the live test. None loses an acknowledged
record silently; each surfaces as an error or a wait.

1. **fork+exec waits for a slow REPORT.** A process holding `O_APPEND`
   descriptors on a mount that forks and execs closes its CLOEXEC copies in
   the child; each close is a FUSE FLUSH, which waits for the append stripe
   while another thread's flush/REPORT holds it (30 s budget, then BUSY to the
   child's implicit close). Under the K29-style stalls, every `subprocess`
   call of such an application waits. The live test spawns all processes
   through a helper forked before any mount descriptor exists.
2. **fsync after an unclean daemon stop fails until verified recovery.**
   After SIGKILL and restart of one daemon, the store logs "recovery
   uncertain; COMMIT returns EIO until verified recovery or fresh format",
   and every fsync on the mount returned EIO for at least 60 s (the fragment
   scan reported `bad=0`). While a node is down, fsync also fails its
   every-node COMMIT (`data_commit.c`). Both are fail-closed by design, but
   the W64 `--failover kill` gate's final fsync will hit them; the parent
   should expect that gate to FAIL on availability unless it waits for
   verified recovery. Logs (no stores): amd `~/efs-artifacts/k9-20261024/s5-sticky-evidence/`.
3. **A simultaneous restart of all three daemons takes 30 s or more**: each
   tries to rejoin a persisted peer that is not listening yet (30 s connect
   timeout) before it serves (K33 area).
4. **A write parked longer than its 30 s budget fails EBUSY after its grant**
   (`write_buf: Device or resource busy`); seen only with a 60 s barrier.

## K15 / K29: the accounting rule

`tests/live/append_failover.py` now separates two cases (`account()`,
`client_events()`; `verify()` takes the hole interval the abandons explain):

- **Accounted, not a mismatch:** a write the client saw fail (errno), whose
  record is absent, with every zero hole explained. An injected lost reply
  leaves exactly one hole. A real failure whose abandon retired the
  reservation (`rc=0 retired=1`) leaves exactly one. A real failure whose
  abandon retired nothing (`rc=0 retired=0`) or did not complete (BUSY) leaves
  at most one: the reservation may never have applied, or session or
  last-close cleanup resolved it. A failure with no abandon (refused before
  reserving) leaves none. Abandons on either mount count; for an identity
  logged twice the last line decides.
- **Mismatch:** an acknowledged record missing, duplicated, torn, corrupt or
  out of order; a failed write's record visible; an unknown block; more holes
  than the abandons explain or fewer than the retirements require; an
  injected loss without an `rc=0` abandon of its identity; more abandoned
  identities than failed writes; a size that is not 4 KiB × (acknowledged +
  holes); views that differ.

The report keeps `verdict` (all gate checks, including the W52 REPORT-timeout
and fsync-bound checks, unchanged) and adds `integrity` (PASS/FAIL from the
rule alone), `accounted_failures` and `checks.accounting` (injected losses,
failed writes, accounted abandons by kind, failures without an abandon, the
explained hole interval, mismatches). Applied to the counts the
[K15 live-gate page](k15-live-gates-20261022.md) records for the intel2 `lost-1` run (its raw logs are gone): 29 failed writes, 4 injected
losses, 5 holes, 2 BUSY abandons, no lost record → integrity PASS with 25
accounted failures, instead of a mismatch; that run's `verdict` still FAILs
on its 12 REPORT receive timeouts (the W52 check).

`tests/test_append_failover_accounting.py` (`make test`): synthetic files and
client-log lines. Positive: a load stall with three real failures (one
retiring abandon, one that retired nothing, one refused before reserving);
the intel2 shape (two injected losses plus a BUSY abandon), with and without
cleanup resolving the BUSY one as a hole; a repeated identity; abandons on
the peer mount. Negative controls (all refused): an acknowledged record
overwritten by a hole; one missing without a hole; a hole with no abandon; a
retiring abandon whose hole vanished; a failed write that landed; an
injected loss with a BUSY abandon; a duplicate; more abandons than failures;
broken per-writer order.

### Harness ports to the K33 CLI

`append_failover.py` and `append_oracle_cluster.sh` still started efsd with
`--node-id` (refused since K33 increment 2), so neither could start a
cluster. Both now start node 1 as `efsd ADDR:PORT STORE`, nodes 2 and 3 on
their first start as `efsd --join SEED --listen ADDR:PORT STORE`, every
restart by endpoint, with `--discovery-port 0`, and wait for formation
(`raft-getattr` of the root) instead of `raft-mkfs`. `--rel` is resolved to an
absolute path (the `--rel .` problem in the K15 live-gate page).
Smokes on amd (not gates): `append_failover.py --records 400 --writers 4`
**PASS** (400 records, three identical views, 27.9 s); with the fault client
as `efs-fuse` (`--rel` a directory of symlinks), `--records 1000 --writers 4
--peer-writers 1 --lose-reserve-every 37 --lose-reserve-max 4` **PASS**
(4 injected losses, 996 acknowledged, 4 holes, three identical views, 78 s).

## Gate

`EFS_TEST_JOBS=4 scripts/make_test_summary.sh` (= `make test-all`) on amd, on
the branch tree and on the untouched base `19313ab06`, both from `git archive`:

| Tree | Result |
| --- | --- |
| branch `fcb47f2bd` | 445 commands, FAIL 10, NOTRUN 5 (161 s, build 12 s) |
| base `19313ab06` | 442 commands, FAIL 10, NOTRUN 5 (177 s, build 13 s) |

- **FAIL set identical in both trees** (amd host limits, the same 10 the K32
  evidence recorded): `tests/faults/test_create_retry_fault.py` (30 s case
  timeout), `test_declaration_gate`, `test_rdma_xprt` and seven
  `test_rdma_segmented` cases (no usable RDMA CQ / soft-RoCE).
- NOTRUN in both: `test_cluster_stop.py` (no `~/git/cluster`),
  `test_follower_bound` stalled compactor (needs `EFS_FAULTS=1`),
  `test_leak_gate.py` (no valgrind), `test_store_durability` async case
  (TMPDIR not XFS), `test_make_test_summary_runner.py` (its own NOTRUN marks).
- The three new commands pass in the branch tree:
  `./tests/test_append_interleave` (0.1 s), `tests/test_append_failover_accounting.py`
  (0.0 s), `tests/test_append_interleave_live.py` (16.0 s; 23–26 s standalone,
  up to about 85 s when the cold restart waits out the K33 rejoin attempt).
- `cd docs && python3 gen-architecture-full.py` and
  `python3 docs/check-architecture.py`: all 5 checks passed.

## NOTRUN gates (parent session)

- **K9 oracle soak** under the original load: `tests/live/gate_k9_append_load.sh`
  / `append_oracle_cluster.sh` rounds with churn, GC, a 4 MiB client cache and
  POSIX load, warm and cold, on the real clusters. W87's gate is unchanged:
  repeated original-load buffered acceptance with exact records.
- **K15 live failover gates**: W52 20k, W64 `step-down` and `kill`, lost reply,
  lost reply + step-down, second runs, W52 A/B. Expect `--failover kill` to
  hit observation 2.
- K29 dedicated-host rerun; RDMA; multi-host; orcd-mini.

## Reproduction

```
git archive <commit> | ssh amd 'mkdir -p ~/k9w/t && tar -C ~/k9w/t -xf -'
cd ~/k9w/t && make -j32 all tests/append_faults/efs-fuse
./tests/test_append_interleave 50000              # model, about 14 s
python3 tests/test_append_failover_accounting.py
python3 tests/test_append_interleave_live.py      # about 85 s; keeps logs under $TMPDIR on failure
python3 tests/live/append_failover.py --rel . --root /tmp/k9af-plain --port 18760 --records 400 --writers 4
EFS_TEST_JOBS=4 scripts/make_test_summary.sh      # make test-all
```

Cleanup: `edelete --force --delete` the tree, `/tmp/k9af-*` and any
`$TMPDIR/efs-k9il-*` left by a failing live run.

## Raw outputs

Outside Git on amd under `~/efs-artifacts/k9-20261024/`, listed with their
sha256 in `SHA256SUMS` there:

| File | sha256 |
| --- | --- |
| `branch-make-test-all-summary.txt` | `6eb430a91af4988368f57aba2b882c750bc49bf17e9198f68521b71bbb972dab` |
| `branch-make-test-all-timings.tsv` | `9b0c11802ba1e65928becc9ce19d0a192ec330f7a0cdc298769c2c0cbb9e14a5` |
| `base-make-test-all-summary.txt` | `46384a57bdea871bf976e60e8e47ecf5a7b0efb97cf16cdc422c912af0a248cb` |
| `base-make-test-all-timings.tsv` | `ced30aaf0c867753c43d05704786bf662658c25a4497b9b67dacfa674a92f291` |
| `model-50000-seeds.out` | `82591f83c759277682ddb319de02d843ceb7e208a4063adbfa6a17aaf74e2648` |
| `live-interleave-run10.out` | `c4d2c0776c7a2f044f1a3ae4402b55b724cc2818ee1ba09e4fa01df8e8360d53` |
| `live-interleave-gate-tree.out` | `71da80bf8cb710e08669ab79141ed096c955f4466a79dd4016e1d3a6dcee6c87` |
| `failover-smoke-plain.out` | `5ee60fc9f918bbf382e38c46c158b37ea04e7aec9ec7e5542bf4ac8e06adcf4a` |
| `failover-smoke-lost.out` | `f59dd2b64f9ec01e35b686f6a0cb7e0a7e291cd3b1b5e20fdbae3c1cd9e7fccd` |
| `s5-sticky-evidence/{mnt-a,mnt-b,n1,n2,n3}.log` (observation 2; client and daemon logs only, stores not kept) | see `SHA256SUMS` |

`SHA256SUMS` itself: `96e8918983aca5277c5bf7f2f6e7b786b6219b62b28e9192982f160119418962`.
The per-command failure logs of both `make test-all` runs were in the deleted
work trees; the FAIL lines are in the two summaries. The build trees, smoke
roots and loopback stores were deleted after the runs.
