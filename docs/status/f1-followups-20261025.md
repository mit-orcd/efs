# F1 follow-ups: K36 under TSan, the RSS plateau on a fixed working set, packer put errors under D4 load (Oct 25)

**Purpose.** The F1 memory and concurrency gates
([evidence](f1-memory-concurrency-gates-20261025.md), merged into devel
`8b25c36fd`) left three items open: `tests/test_k36_recovery.py` was
intermittent under `make tsan-check` (3 of 5), the 20-minute D4 RSS runs were
bounded but not flat because the load creates files monotonically, and the
same runs counted 7 and 3 packer put errors. This page works the three
for release checks F1 (memory/concurrency gates) and D4 (bounded progress)
in the [alpha 1.0 checklist](alpha-1.0-2plus1-checklist.md). Item 1 is
closed. Items 2 and 3 are closed for the standard 20-minute D4 load but
not for the long fixed-working-set load. The fixed working set also found
a fourth problem, a product defect in W77 publication units that turned
rewrites into EIO; it is fixed here.

**Verdict.** On xorinox, branch `claude/devel-f1-followups-ae2deb` (devel
`5a7463440` merged). Last code commit `758dd144c`; last commit `04408a3e4`,
which changes only tests and docs (the gate list):

| Item | Result |
| --- | --- |
| 1. `test_k36_recovery.py` under TSan | **PASS 10 of 10** (before: 7 of 10). Three causes: the crash hooks' `_exit` under libtsan, a data race in a test hook, and a test defect in case 11 |
| 2. RSS plateau, fixed working set | **FAIL: bounded, not flat.** Both efs-fuse plateau (except the load mount at the highest rate, +6.2 %). efsd grows 8–13 % over minutes 40–60, with throughput. New allocator bound in efsd (arena cap 8, fixed 256 KiB mmap threshold, trim every 15 s) cuts its 60-minute peak by about a third (101.8 against 137.9–148.7 MiB). The remaining growth tracks the connection-thread high-water mark; see [Item 2](#item-2-rss-plateau-on-a-fixed-working-set) |
| 3. Packer put errors | **20-minute D4 load: before 3, after 0** (follower `NOT_PRIMARY`, covered by the flake-sweep fix `fc79fe086`). **Not closed at higher throughput:** the 60-minute fixed-working-set run counted 5 with the fix, because the refusal outlasts its 4 × 50 ms retry; see [Open](#open) |
| 4. New: rewrites EIO under the fixed working set | **FIXED** (`d7a6eed7e`). A publication unit whose forwarded PREPARE batch was refused skipped its ABORT; new regression test fails 5 of 5 with the fix reverted and passes 6 of 6 with it |
| `make test-all` (`04408a3e4`, 8 jobs) | **PASS with host limits:** 480 commands, 0 FAIL, 14 NOTRUN (RDMA device, `~/git/cluster` scripts, an `EFS_FAULTS=1`-only case, runner self-test markers), 653 s; exit 3 (INCOMPLETE) for the NOTRUN lines. The run before it failed 1, `test_k33_autoid.py` (a log read 11 ms before the restarted daemon printed its line; 5 of 5 reruns pass). At `758dd144c` it failed 2: `test_stop_progress.py` (Open 6) and `test_d4_plateau.py` (now out of the gate) |
| `make tsan-check` (`758dd144c`) | **PASS 18/18** (627 s), `k36_recovery` among them |
| `make asan-check` (`758dd144c`) | **PASS 47/47** (483 s) |

## Item 1: `test_k36_recovery.py` under TSan

Reproduction: the tsan entry alone in a loop on the TSan tree the gate
builds (`EFS_SAN_BUILD`, `EFS_SAN_ONLY=k36_recovery`, `EFS_K36_KEEP=1`, a
private `TMPDIR` per run, daemon and mount logs kept). Ten runs on the base
(devel `5a7463440` content), then ten after each round of fixes. Each
failure had a distinct cause in the kept logs:

1. **Step 4 counts: crash hooks were not crashes under TSan** (2 of 10
   on the base; the F1 session saw 2 of 5). The test saw
   `missing=1 marked=1 already_marked=0 checked=62` where it wanted
   `missing >= 2` and `marked + already_marked >= 2`. Node 3's log of the
   step-3 run shows why. After `fault: injected crash at recovery-marked`
   the process kept logging and serving for 0.9 s: a query publish, then
   `store: put O_DIRECT bounce copy ... len=65536`, one 64 KiB disk write.
   Node 1 logged `rebuild ino=4097 ci=0 ... node=3: PUT failed (rc=-1);
   mark kept` only when the process ended. libtsan intercepts `_exit` and
   finalises its runtime first, so the SIGKILL emulation of
   `EFS_FAULT_CRASH` leaked: a W154 rebuild PUT landed on the "crashed"
   store. The next recovery found 62 files where the crashed run had left
   61, so one of the two lost fragments was proven present.
   **Fix (fault hooks):** every crash point (`EFS_FAULT_CRASH`,
   `EFS_FAULT_PACK_CRASH`, `EFS_FAULT_COMPACT_DIE`, `EFS_FAULT_MEMBER_EXIT`)
   exits through `include/efs/fault_exit.h`, the raw `exit_group` system
   call. No sanitizer, atexit or stdio path runs after it.
   **Fix (test):** the rebuild can also land legitimately between node 3
   listening and its inventory reaching the stripe. The heartbeat sees
   node 3 up and the GC pass rebuilds. The PUT takes the fragment lock the
   scan takes and creates `O_EXCL`, so the scan never reads a torn file.
   The fixed loops showed this in 4 of 20 runs (`rebuilt_before_fence=1`).
   Step 4 now counts each lost fragment as either found missing (marked,
   or the step-3 mark found) or already back on disk at full length, and
   expects that many fewer rebuilds afterwards.
2. **A ThreadSanitizer data race** (1 of 10 on the base).
   `put_test_busy` in `src/server/put_upload.c`, the
   `EFS_FAULT_PUT_BUSY_FILE` hook of EFS_FAULTS builds, read its env path
   once behind an atomic init flag. Two first connection threads both
   wrote and read the static `path` plainly. **Fix:** the path itself is
   the atomic (`""` when unset), the pattern `server_test_dest_fault`
   already used.
3. **Case 11** (1 of 10 after the first round). The segment chosen for a
   fragment deletion had already been written degraded by node 2's packer
   during case 10's restart of node 1 (`fragment 1 for node 1 not
   acknowledged (rc=-6)`, `EFS_ERR_NET`). Deleting its fragment 2 was
   therefore a correct UNRECOVERABLE. **Fix (test):** case 11 waits for
   `Degraded segments: 0` before choosing. This is the same change as on
   the flake-sweep branch (`fc79fe086`, natively 1 of 10 there), so the
   branches merge clean.

| Loop | Tree | Result |
| --- | --- | --- |
| before | devel `5a7463440` content (F1 merged) | 7 of 10: runs 6 and 9 step-4 counts (cause 1), run 3 TSan report (cause 2) |
| after | fixes 1 and 2 | 9 of 10: run 7 case 11 (cause 3); `rebuilt_before_fence=1` in 3 runs |
| after2 | fixes 1–3 (`df9433407`) | **10 of 10** (75–143 s each; `rebuilt_before_fence=1` in 2 runs) |

## Item 4: rewrites EIO, a W77 publication unit that skipped its ABORT

The first 30-minute fixed-working-set run (item 2) failed three load
rewrites with EIO. Each client log shows the same pattern. A REPORT loop
ran 8 s with STALE rounds, then BUSY rounds, `report-resolve ...
outcome=partial`, and the error went to the writer. Each one coincided
with a `raft-host: txn-recover shard=<the inode's shard> ... age=6.2-13.2s
-> ABORT` on the coordinator. A transaction had held PREPARE intents on
the inode with no decision until the 5 s recovery scan aborted it.

A diagnostic build logged the transaction id, `k` and the abort decision
on every unit failure. It showed the chain:

```
14:51:34.495 DIAG pub-unit fail ino=134309 txid=561b3288… k=0 rc=-14 verdict=0 abort=0
14:51:35.290 DIAG pub-unit fail ino=134309 txid=60b2e5c6… k=0 rc=-13 verdict=0 abort=0   (next id, BUSY on the leftover intents)
14:51:40.223 DIAG txn-recover txid=561b3288…
14:51:40.223 raft-host: txn-recover shard=3237 coord=3237 parts=4 age=6.3s -> ABORT
```

**Cause.** A multi-chunk write publishes as one cross-group transaction
(W77, K32 sets). The node running the REPORT submits each group's PREPAREs
as one TXN_BATCH entry. Its own group's verdict comes back through a
counted ref (`k`). The other group's batch is forwarded to that group's
leader, whose answer is the batch's verdict. When the leader's apply wait
runs out, or one PREPARE of the batch conflicts, that answer is BUSY or
STALE. A TXN_BATCH applies each PREPARE on its own, so the refused batch
can still have installed intents. But the refusal arrives as the submit's
return code, so the batch holds no ref. When it was the first batch
submitted, `k` was 0, and `host_pub_unit_commit` gated its ABORT on `k`.
Nothing decided the transaction until the recovery scan. Meanwhile the
client, having received STALE, rebuilt the publication with a new id,
whose PREPAREs were BUSY against the leftover intents until its 8 s
budget ran out.

**Fix** (`src/server/raft_host.c`, `d7a6eed7e`): the TXN_BATCH builder
records that an entry was submitted, whatever the answer. The unit aborts
on any failure after that. ABORT is decide-once and resolves every
participant shard, so an abort with nothing applied costs one DECIDE and
its RESOLVEs. The append path already aborted this way.

**Regression test:** `tests/test_pub_unit_forward_abort.py` (`make
test-long`, about 22 s) uses a new hook, `EFS_FAULT_FWD_TXN_STALE=<dir>`
(EFS_FAULTS builds, A6 row). With it, a leader answers the next forwarded
TXN_BATCH STALE after it applied, once per `<dir>/stale-<node id>`. The test
gives the two groups different leaders (`raft-step-down` when one member
leads both). It then runs 12 rounds, each a fresh directory and a 1 MiB
(8-chunk) write+fsync. It requires every write+fsync under 3 s, every file
exact, the hook firing in at least half the rounds, and no `txn-recover
... -> ABORT` within a recovery period after the last round.

| Tree | Result |
| --- | --- |
| fix reverted (`if (k)`) | **5 of 5 FAIL**: four fsync EIO, one with writes of 6.4–10.6 s (the recovery-scan wait) |
| `d7a6eed7e` | **6 of 6 PASS**: hook fired in 12 of 12 rounds, write+fsync max 0.72–0.89 s |

K38 (W161, ior EIO after a STALE REPORT on orcd-mini) ends in the same 8 s
budget EIO, but its investigation points at a different mechanism: a stale
span base that the replay never rebases.

## Item 2: RSS plateau on a fixed working set

`scripts/d4_progress.py --working-set N --fg-set M --plateau-pct P` and
`tests/test_d4_plateau.py` (in `make test-long`, 75 s; budget row in
`tests/budgets.tsv`). Before the timed load each of the 8 writers creates
its N files (every eighth 192–512 KiB, two to four 128 KiB chunks, never
packed; the rest 1 B–8 KiB, packed). During the load it rewrites a random
one of them with a new version: an overwrite (`O_TRUNC`, new length), a
truncate (to half, then the new content from 0), or an in-place rewrite
(same length). The foreground rewrites a ring of 32 1 MiB files and reads
them back. Nothing is unlinked or created during the load. The verdict
needs `created == live == threads × N` and every operation kind.

So the packer unindexes and repacks, segments get holes and compaction, old
generations go to reclaim, the `.efs` index refreshes every 2 s, and the
256 KiB memtables flush and compact, all with a fixed namespace. The driver
samples every efs-fuse and efsd RSS once a second. `scripts/rss_plateau.py`
(`evaluate()`, also the CLI on a `ps` series) gives the verdict per process:
PLATEAU when the last third of the load grows by at most 5 % of the peak.
The test prints the table on every run and enforces it from a 600 s load
(`EFS_D4_PLATEAU=1` forces it). `tests/test_d4_driver.py` (`make test`) has
a negative control for net creation, a missing operation kind and a growing
process. A load file whose last rewrite failed is unspecified, not
acknowledged state: it leaves the exactness check until a rewrite succeeds
(the failure itself fails the run).

### What grew

With the namespace fixed, both efs-fuse plateau in every 30-minute run and
in the 60-minute run without the bound. efsd does not plateau in any run.
Over 60 minutes its growth does not flatten. Four measurements located it:

- **Live heap is nearly flat.** An LD_PRELOAD shim counted live bytes per
  3-frame allocation site in every efsd (diagnostic run B). From minute 10
  to minute 34 live heap rose from 57.3 to 58.3, 58.0 to 58.9 and 57.1 to
  59.8 MiB. The largest live sites are fixed allocations: reclaim journal
  buffers (30 MiB), the Raft disk buffer (8 MiB), the in-memory Raft log
  tail (4 MiB, stable) and the query catalog build (4 MiB). The sites that
  grew were KV segment block caches (`kv_seg_read`, `kv_seg_iter_seek`,
  `block_hash_build`, about 2 MiB), bounded at 256 slots per open segment.
- **Mappings.** Sampled every 60 s from `/proc/<pid>/smaps`, efsd RSS is
  over 90 % anonymous memory. The other notable mapping is the Raft log
  index files, bounded at a 2 MiB resident window per group
  (`RAFT_INDEX_RESIDENT_BYTES`). They rose from 0.2 to 3.8 MiB and then
  stopped.
- **The allocator held free memory.** `malloc_info` taken through gdb
  showed 59–73 glibc arenas per efsd, one per thread of its 60–80. Of
  44–93 MiB of arena memory, 27–70 MiB was free but resident, and the arena
  count rose with thread turnover. glibc's dynamic mmap threshold made it
  worse: once a large block was freed, later block reads and payload
  buffers came from the arenas and fragmented them.
- **Connection threads.** efsd runs one thread per TCP connection. The
  pools grow on demand up to 34 connections per peer pair and 256 per
  efs-fuse per server, and do not shrink. Each new high in concurrency
  adds threads, stacks and per-thread buffers. In the full-bound 60-minute
  run the thread count stepped from 68 to 83 at about 40 minutes, just as
  RSS accelerated.

### Allocator bound (`934864b6d`, `758dd144c`)

efsd now bounds the allocator, all glibc only, internal and not knobs:

- at most 8 arenas (`M_ARENA_MAX`), set before any thread exists;
- a fixed 256 KiB mmap threshold (`M_MMAP_THRESHOLD`), so a large
  transient buffer is its own mapping and is returned on free;
- `malloc_trim(0)` from the heartbeat thread every 15 s, logged only past
  100 ms; no trim was logged in any run.

A paired 10-minute run measured the effect of the threshold. With the cap
and trim alone the arenas held 35–64 MiB, 13–41 MiB of it free, at RSS
56–76 MiB. With the fixed threshold added: 20–26 MiB of arenas, 5–11 MiB
free, RSS 50–55 MiB. In use stayed near 15–22 MiB plus 42–47 MiB of large
mappings.

### Runs

The 30- and 60-minute variant: `EFS_D4_SET=256` (2048 files, 256 of them
192–512 KiB), 8 writers, a 32-file foreground ring, an 8G quota. The growth
is the least-squares slope over the last third of the load times its span,
as a share of the peak (`scripts/rss_plateau.py`; criterion 5 %).

| # | Tree | Load, host | Rewrites (errors) | efs-fuse load / fg | efsd 1 / 2 / 3 | Peak efsd / efs-fuse | Put errors | Drain |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | devel `5a7463440` content | 30 min, shared | 43 953 (**3 EIO**, item 4) | +1.2 / +0.0 % | +4.8 / **+7.0 / +6.4 %** | 129.0 / 176.5 MiB | 0 | not drained in 150 s |
| 2 | + abort fix and `fc79fe086` | 30 min, alone | 63 024 (0) | +3.5 / +0.1 % | +3.4 / **+6.1 / +5.1 %** | 127.8 / 182.8 MiB | 0 | 394 s |
| 3 | + abort fix and `fc79fe086` | 60 min, shared | 74 419 (0) | +1.7 / +0.0 % | **+8.8 / +8.8 / +8.9 %** | 137.9 / 185.4 MiB | 0 | 391 s |
| 4 | + arena cap and trim | 30 min, shared with 3 | 33 748 (1 EIO, Open 1) | +2.6 / +1.9 % | **+5.3** / +3.8 / **+6.6 %** | 93.8 / 175.8 MiB | 2 | 415 s |
| 5 | + arena cap and trim | 60 min, alone | 1 EBUSY (Open 3) | +3.7 / +0.0 % | **+32.2** / **+11.2** / −0.6 % (a gdb probe at 46 min disturbed it) | 140.3 / 196.5 MiB | — | — |
| 6 | + fixed mmap threshold (`758dd144c`) | 60 min, alone | 119 827 (0) | **+6.2** / +0.9 % | **+8.1 / +9.8 / +13.3 %** | 101.8 / 196.6 MiB | **5** | 397 s |

Every run kept every file exact and every foreground operation within
10 s, and stayed far inside the D4 RSS bounds (efs-fuse 1024 MiB, efsd
2048 MiB). Run 6 did 61 % more rewrites per minute than run 3 on the same
host.

**Verdict for item 2: FAIL, bounded but not flat.** Under a fixed working
set the clients meet the 5 % plateau criterion over 30 minutes and over 60
minutes at run 3's rate. efsd does not, in any run. The allocator bound cut
efsd's peak by about a third (101.8 against 137.9–148.7 MiB at 60 minutes)
and removed the arena growth. What remains grows with throughput: in run 6
efsd gained 8–13 % over minutes 40–60, and the load mount's efs-fuse 6.2 %.
The thread count rose with it. Live heap, the Raft index and the KV block
caches are flat or bounded, so the candidates are the connection-thread
high-water mark (threads, stacks and per-thread buffers; bounded only by
the pool caps of 34 per peer and 256 per client per server, which never
shrink) and residual fragmentation in the 8 arenas. The next measurement is
a run that holds connection counts fixed, or an idle-connection reap,
under the same load. Bounds held: efs-fuse 196.6 MiB of 1024, efsd
101.8 MiB of 2048 (run 6).

### Drain after a long rewrite load

The drain after a 30- or 60-minute rewrite load took 391–415 s, against
3.5 s after the create-only load. Two things kept it open:

- **Index changes.** The index's pending changes hover between 4 and 30
  for minutes, at an age under 2 s, while the packer compacts the segments
  the rewrites holed. The index is keeping up, but "0 pending in two
  consecutive samples" waits for that compaction to finish.
- **Pack candidates left for minutes.** In run 2, 28–31 candidates stayed
  past the load. In diagnostic run A, 28 stayed with a lag of 346 s. In the
  75 s gate run beside the 8-job pool, the lag reached 158 s, so the drain
  missed its 150 s bound. The packer skips a candidate while any lease on
  the file exists (its selection rule). That a client keeps leases on
  rewritten files after close is the leading hypothesis; it is not proven.

The long runs pass `EFS_D4_DRAIN_S=900`. Because the stall is unexplained,
`tests/test_d4_plateau.py` is a measurement driver, not part of `make
test-long`; raising its bound to make the gate pass would hide a progress
stall (D4). Its verdict logic stays in `make test` (`tests/test_d4_driver.py`).

## Item 3: packer put errors under the 20-minute D4 load

Before (devel `5a7463440` content, `tests/test_d4_progress.py`,
`EFS_D4_LOAD_S=1200 EFS_D4_QUOTA=8G EFS_D4_KEEP=1`, logs kept): node 3,
leader of group 2, counted `put_errors=3` over 53 268 files packed during
the load. They came at 739, 785 and 914 s; none in the first 12 minutes.
Its log names each one:

```
12:58:18 packer: segment g2 id=e1c7272836e5e003 gen=1 written degraded: fragment 2 for node 1 not acknowledged (rc=-15); marked for rebuild on return
12:59:03 packer: segment g2 id=e1c731abff4fe703 gen=1 written degraded: fragment 2 for node 2 not acknowledged (rc=-15); marked for rebuild on return
13:01:13 packer: segment g2 id=e1c74fe8ca62d703 gen=1 written degraded: fragment 0 for node 2 not acknowledged (rc=-15); marked for rebuild on return
```

`rc=-15` is `EFS_ERR_NOT_PRIMARY`. The follower's linearizable authority
read of the just-admitted segment object (`server_segment_put`) was refused
under load. The packer took the refusal for a missing fragment, wrote the
segment degraded, and K37 rebuilt it. This is the defect the flake-sweep
branch fixed in `fc79fe086`: a fragment refused `BUSY`, `NOT_PRIMARY` or
`AGAIN` is sent again, 4 × 50 ms, before the segment is written degraded
(`EFS_FAULT_SEGMENT_PUT_TRANSIENT` forces it in K37 case 1b). The flake
sweep saw it once in ten gate runs on a 45 s load.

After: the same test and settings on this branch with
`claude/devel-flake-sweep-1505b6` merged into a temporary tree (the branch
was not on devel yet).

| Run | Load | Files packed in the load | `put_errors` | Resends | Verdict |
| --- | --- | --- | --- | --- | --- |
| before | 1200 s | 53 206 | **3** (all `rc=-15`) | — | FAIL (packer errors) |
| after | 1200 s | 40 838 | **0** | 9 × `resent after rc=-15: rc=0` | **PASS** (3253 foreground ops within 10 s, drain 3.5 s, 36 601 files exact) |

The before-run's other bounds passed: client RSS 227 MiB ≤ 1024, server
130.8 MiB ≤ 2048, drain 3.4 s. The after-run packed fewer files because
another 30-minute run shared the host.

The fix holds on the standard load and on the fixed-working-set runs 2
and 3 (0 put errors each). It does not hold at the highest rate: run 6
counted 5 and run 4 counted 2 (see [Open](#open), item 2).

## Open

Seen on the long fixed-working-set runs (numbers refer to the item-2 run
table).

1. **One rewrite EIO in 33 748: a late finisher abort.** In the 8 s REPORT
   window of inode 919786, publication 80872 ran on two nodes. Node 2's
   attempt failed STALE with intents applied, so with the item-4 fix it
   aborted. Its DECIDE ABORT then hit the 400 ms apply wait during an
   apply stall, and the finisher decided the abort only 3.4 s later
   (`txn-finish coord=2282 abort decided`). The next publication was BUSY
   on those intents until the client's budget ran out. The finisher
   handles one transaction at a time, and transactions waiting up to 1 s
   for a decision (`HOST_FIN_DECIDE_NS`) sit ahead of ABORT items. The
   recovery scan was not involved; that is the item-4 fix working. The
   direction is to let ABORT items pass items that are waiting for a
   decision, or to add a second finisher thread. Owner: W77 transactions.
2. **Packer put errors past the retry: 2 in run 4, 5 in run 6.** In each,
   a follower answered `NOT_PRIMARY` to all of the `fc79fe086` retries
   (4 × 50 ms), so its local ReadIndex failed for more than 200 ms. Run 4
   shared the host; run 6 ran alone at the highest rate. The other 90
   resends in run 4 succeeded. The 20-minute D4 load and runs 2 and 3
   counted 0. **Fixed** on `claude/interesting-mclean-65362a`
   (`dd0abe363`): the resend is bounded by 2 s, not 4 tries; 0 put errors
   in the 60-minute runs ([evidence](d4-pack-drain-20261025.md)).

3. **One foreground EBUSY in run 5.** A ring rewrite's truncate
   (`INODE_SETATTR`, from `O_TRUNC`) to node 3 failed on receive five times
   in about 8.7 s (`inode-rpc: retry type=63 attempt=5 why=recv rc=-6
   nid=3`), while node 3 ran 1.4–3.1 s GC passes. The write+fsync returned
   EBUSY: one in 13 173 foreground operations. Owner: client metadata RPC
   retry and server GC pass length.
4. **efsd RSS growth with throughput** (item 2 verdict). Owner: F1/K22.
5. **Pack candidates left for minutes after a rewrite load** (drain,
   item 2). **Fixed** on `claude/interesting-mclean-65362a`: not leases,
   but two candidate keys of one inode in one segment (`dd0abe363`), then
   the truncate-sweep backlog holding the index drain (`b7bf18bdd`); the
   60-minute run drains in 54.5 s and `test_d4_plateau.py` is back in
   `make test-long` ([evidence](d4-pack-drain-20261025.md)).
6. **`tests/test_stop_progress.py`: one mkdir EIO** in the 8-job gate at
   `758dd144c` (its logs were not kept); 5 of 5 reruns passed beside the
   TSan gate. Not attributed.

## Environment

xorinox: AMD Ryzen 9 7940HS (16 CPUs), 60 GB RAM, Rocky Linux 10, gcc
14.3.1, libtsan/libasan, gdb. Private trees under `$HOME`, each a
`git archive` of a commit copied over ssh: `~/efs-close*` (base and fix
rounds), `~/efs-red` and `~/efs-green` (item 4), `~/efs-diag*` (the
diagnostic build), `~/efs-trim*`, `~/efs-plat*` and `~/efs-final*`. The
`~/efs-plat*` trees and the item-3 after-run tree carry `fc79fe086` merged
into a temporary commit that is not on the branch. `TMPDIR=$HOME/efs-tmp`
(XFS); loopback ports the tests choose. The host's live cluster mount, the
17xxx ports and other users' processes were not touched. The private
trees and `~/efs-tmp` were deleted at the end of the session. Some runs shared
the host, and the run table says which (load average 4–19 when they
overlapped).

The heap profile used an LD_PRELOAD shim. It counts live bytes per 3-frame
allocation site in efsd and writes a dump every 120 s; `heapdiff.py` diffs
two dumps and symbolizes them. It ran from xorinox
`~/efs-artifacts/f1-close-20261025/heapsite.c` and `heapdiff.py`, beside
the raw outputs; it is a diagnostic tool, not part of the tree.

## Reproduction

```bash
git archive 04408a3e4 | ssh xorinox 'mkdir -p ~/efs-x && tar -xm -C ~/efs-x && cd ~/efs-x && find . -type f -exec touch {} + && make -j16 test-bins'
```

K36 under TSan, 10 runs (the TSan tree is built by the first gate run and
reused through `EFS_SAN_BUILD`):

```bash
ssh xorinox 'cd ~/efs-x && export TMPDIR=$HOME/efs-tmp EFS_SAN_LOGDIR=$HOME/efs-x-tsan EFS_SAN_ONLY=k36_recovery EFS_K36_KEEP=1 && make tsan-check && for i in $(seq 1 9); do EFS_SAN_BUILD=$HOME/efs-x-tsan/build bash tests/sanitizer_check.sh tsan; done'
```

The abort regression, then the plateau measurement and the put-error run:

```bash
ssh xorinox 'cd ~/efs-x && export TMPDIR=$HOME/efs-tmp && python3 tests/test_pub_unit_forward_abort.py && EFS_D4_LOAD_S=3600 EFS_D4_SET=256 EFS_D4_DRAIN_S=900 EFS_D4_QUOTA=8G EFS_D4_ARTIFACTS=$PWD/out-plateau python3 tests/test_d4_plateau.py && EFS_D4_LOAD_S=1200 EFS_D4_QUOTA=8G EFS_D4_KEEP=1 EFS_D4_ARTIFACTS=$PWD/out-d4 python3 tests/test_d4_progress.py'
```

Expected results:

- `PASS k36_recovery` ten times.
- `publication unit forward abort: PASS`.
- The plateau test's table. On this commit efsd reads GROWING (item 2
  verdict), so the test fails on the RSS criterion while every other check
  passes.
- `D4 bounded progress ...: PASS`, with `put_errors=0` in the last
  `pack-compact` sample.

Raw outputs of this session are on xorinox under
`~/efs-artifacts/f1-close-20261025/` (995 files, 2.9 GB; `SHA256SUMS` sha256 `eae97cf1829993ac0a6d41721cea6efac2e86b229ba94fcd3461eb0e4a5101de`):

- The K36 loop logs, with every run's gate output, TSan report and daemon
  logs: `tsan-k36-before/`, `tsan-k36-after/`, `tsan-k36-after2/`.
- The D4 runs' driver JSON, `ps` RSS series, plateau tables and daemon
  logs: `d4-before-1200/`, `d4-after-1200/`, `d4p-val-90/`, `d4p-1800/`,
  `d4p-diag-A/`, `d4p-diag-B/` and `d4p-plat/`.
- The heap dumps (`heap-d4p-diag-B/`) and smaps samples
  (`d4p-plat-smaps.txt`).
- The red/green regression runs: `pufa/`.
- The gates: `final3/` (test-all, tsan-check and asan-check at
  `758dd144c`), `final4/` and `final5/` (test-all at `04408a3e4`); `final/`
  is an earlier test-all at `fd8b6637a`.
- The sanitized build trees inside the gate directories were pruned
  (`PRUNED.txt`).

## Limitations

- The plateau is measured on loopback with 256 KiB memtables and a 2 s
  index refresh, the D4 test configuration; K22 covers the production-scale
  follower bound and plateau.
- The put-error after-run and the plateau measurement use the flake-sweep
  packer fix merged into a temporary tree. The fix reaches devel with
  branch `claude/devel-flake-sweep-1505b6`.
