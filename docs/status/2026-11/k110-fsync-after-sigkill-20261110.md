# K110 · the final fsync returns EIO after sequential SIGKILLs (Task FH, Nov 10 2026)

**Verdict: reproduced 14 of 14 private runs; class B (persistent). No product
code changed. K110 stays OPEN; the contract and the proof model need a
decision from the coordinator and the user.** Branch
`claude/determined-mclean-7c3b6f` on devel `9403d0666`, not merged, not
pushed. The only repository changes are the observation-only `--post-fsync`
option of `tests/live/append_failover.py` (Task EY's `8a34adbe5`,
cherry-picked), three scripts under `scripts/evidence/k110/` and this page.

Two sentences for the K110 row. The final `fsync` fails because a stripe's
fragments were acknowledged by member incarnations that were SIGKILLed before
any COMMIT, and the client, by the rule of
[K35](../2026-10/k35-fsync-degraded-20261024.md) and
[K36](../2026-10/k36-verified-recovery-20261024.md) ("never counts it
durable"), proves a fragment only through a COMMIT answered by the
incarnation that acknowledged it. The record of such a fragment (`L`) can
never become durable again, the client no longer holds the bytes to re-send,
and so every later `fsync` of the file fails until the client unmounts, while
every byte reads back exact.

## Class (step 4)

| Class | Verdict | Evidence |
| --- | --- | --- |
| A transient, correct by D42 | **No** | A second `fsync` after every member was back failed in 14 of 14 runs: 2.6 to 6.4 s after the last restart, and again 14 to 30 s after it, with a stable refusal list. |
| **B persistent** | **Yes** | As above. The fold reads only `P` fragments (`data_commit.c:505`), so an `L` fragment is terminal; waiting cannot change a restarted member's verifier. |
| C durable fragment not counted by a proof defect | **No** (see "Why the `L` fragment does not count") | An `L` fragment has no durable record anywhere that the client could count: the acknowledging incarnation never answered a COMMIT, and the server keeps no per-fragment barrier record. The rule that excludes it is the written rule, not a mis-implemented fold. One sub-case (a rebuilt `M` slot) has a durable server-side proof the client cannot see; it is 27 to 40 % of the refused stripes and not a fix on its own (option 4). |
| D something else | partly | The finding is a contract gap: [D42](../decisions.md) F22 says `fsync` may succeed with a member down "only with proof", and F23 says an earlier ambiguous write "is not thereby proven"; neither says what an application can do when two members of a stripe each restarted uncleanly between the write and the `fsync`. |

The test is `final_fsync_zero` of `tests/live/append_failover.py --failover
kill`: one `fsync` per writer after the last write, behind 4 to 7 SIGKILLs of
all three members. A control with `--fsync-every 100` (every writer fsyncs
every 100 records) **passed 3 of 3**, with every mid-run fsync, the final
fsync and both post fsyncs returning 0 during and after the same kills. So
D40/D42 availability with one member down holds; the failing case is a
stripe that stays unproven across the unclean restart of its members.

## Source and environment

- Source: devel `9403d0666` plus the cherry-pick of `8a34adbe5` (branch head
  `983d70659`, the test option only; no C or header change). The tree was
  copied with `git archive HEAD`, so the binaries report `build=unknown`.
- Build on node9900 (Linux 5.14.0-687.39.1.el9_8 x86_64, 192 cores, gcc 11.5.0,
  Python 3.9.25, `/tmp` on xfs `/dev/md0`): `make -j8
  EFS_FUSE_PREFIX=/data1/efs/deps/libfuse efsd efs-fuse efs-mgmt`. SHA256:
  `efsd` `94861b34839f5b3f3e03775b23854f97bdeee674e49214e6fe470613e0374075`,
  `efs-fuse` `f085dc457f135cdb9fc95e9634e35887217ddb5351c739093f2f4ff67644d085`,
  `efs-mgmt` `ab015aeff1020455334a353baa4b179fec9bb7993338e9cbf52c7a13e0898416`.
- Instrumented client (obs runs only): the same `efs-fuse` with the two
  `< 8` caps of `stripe_prove()` raised to 1,000,000 so every refused stripe
  is named; SHA256 `4cb6baeba0cd645f6797e8cc8a46b9195ffa9ea21445d3c45f0e2a7b93f974c1`.
- Environment: three loopback members, TCP, direct I/O (the only mode),
  4 writers appending 4 KiB records to one shared file through one mount,
  8,000 records (32 MiB, about 250 chunks), quota 8 G, private ports 38101 to
  38103, `TMPDIR` under `/tmp`. The leader of group 0 or 2 is SIGKILLed about
  every 12 s and restarted after 5 s (`--interval 4 --first-event 3 --down 5`,
  Task EY's setting). Node9900 was shared: before each series three checks 20 s
  apart found no other task's `make_test_summary`; a TSan lane and other
  tasks' loopback probes ran beside it.
- Runs: `a-1`, `a-2`, `b-1` to `b-12` (14, the table below); `obs-1`, `obs-2`
  (instrumented client, 2); `fe-1` to `fe-3` (control, 3). A first series was
  stopped by hand during its third run (`a-3`, to keep the run stores) and is
  not counted.

## The letters (step 1)

The per-stripe record is `struct stripe` in `src/client/data_commit.c:33`;
the log line is `stripe_log()` (`:341`), the letters `frag_letter()` (`:335`).
One record per PUT object (a chunk base or a span); fragment `i` is on
`nodes[i]`; `seq[i]` is the client's count of that node's acknowledgements and
`epoch[i]` the PUT verifier (boot epoch) of the incarnation that acknowledged.

| Letter | State | Meaning | Assigned by |
| --- | --- | --- | --- |
| `P` | `FRAG_PENDING` (`:27`) | acknowledged by an incarnation, not yet proven; the only state a COMMIT is sent for (`:439` to `:443`) | `stripe_note_locked()` (`:174`) for a new record whose `seq[i]` is non-zero (`:201`), and for a repeat acknowledgement of the same object, which resets a `P`, `M` or `L` fragment to `P` (`:183` to `:188`) |
| `D` | `FRAG_DURABLE` (`:28`) | a COMMIT sent after the acknowledgement was answered `OK` with the same boot epoch | the fold in `stripe_prove()` (`:508`), `p->epoch[i] == jobs[s].epoch`, from `commit_one()` (`:288`) |
| `L` | `FRAG_LOST` (`:29`) | that node's COMMIT answered `OK` with another boot epoch: the acknowledging incarnation is gone | the same fold (`:508`), epoch differs |
| `M` | `FRAG_MISSING` (`:30`) | never acknowledged: the object was published degraded (W154) on two fragments; the publication carries the degraded mark | `stripe_note_locked()` (`:201`), `seq[i] == 0` |
| `K` | `FRAG_MARKED` (`:31`) | marked degraded now by this `fsync` after the Raft `EFS_MD_CMD_DEGRADED_MARK` returned OK | `stripe_prove()` (`:556`) |

The proof (`stripe_prove()` `:532` to `:588`): a stripe leaves the table when
it has three `D`, or two `D` and the third covered by a mark (`M` from the
publication, or `K` written now). With fewer than two `D` the call asks
whether the object is gone (`stripe_close_for_absence()` `:353`, then
`efs_client_rpc_object_live()` `inode_rpc.c:2711`); a live object is
refused, and the message at `:583` to `:586` ("a member of another
incarnation lost a fragment") is printed when any fragment is `L`.

Server side: `DATA_COMMIT` (`handler.c:907`) answers `put_boot_epoch`. The
epoch is random per boot (`store.c:1950`) and is kept only after a proven clean
stop of every root (`put_sync_reuse_epoch()` `:1696`, applied at `:1956`), so
SIGKILL always changes it. The barrier is `syncfs` of the root
(`put_sync_barrier_locked()`, `:1729`) followed by an fdatasync'd record of the
barrier seq (`put_sync_record_write()` `:1675`); a COMMIT waits for it
(`put_sync_commit_root()` `:1827`). K36's recovery reads every local
fragment whole and checks its digest, then clears the fence; it does not
tell any client which acknowledged fragments it holds.

## Runs (step 2)

Per run: the final `fsync` of all four writers returned `EIO` (`-5`) in
every run; integrity was exact in every run (8,000 of 8,000, no duplicate,
missing, torn or out-of-order record, warm primary, warm peer and cold mount
identical). "Down" is the member that was inside its 5 s down window at the
first refusal; "up" means all three members were running. Post-fsync delays
are in seconds after the restart of the last killed member; both post fsyncs
returned `EIO` in all 14 runs (the first right after the failover loop ended,
the second after the warm reads).

| Run | Kills (member@s from start) | At first refusal | Final fsync max s | Refused stripes | Post 1 s | Post 2 s |
| --- | --- | --- | --- | --- | --- | --- |
| a-1 | n1@9 n3@21 n2@33 n3@45 | up, +5.6 s after restart | 0.15 | 252 | 5.7 | 21.8 |
| a-2 | n1@9 n2@22 n1@34 n3@46 n2@58 | up, +6.2 | 0.11 | 313 | 6.4 | 30.3 |
| b-1 | n1@10 n3@22 n1@34 n3@46 n2@59 | up, +2.2 | 0.16 | 356 | 2.7 | 26.8 |
| b-2 | n1@8 n3@21 n2@33 n1@45 n3@57 n2@70 | n2 killed during the fsync; two writers waited 6.3 s for it | 6.37 | 329 | 2.6 | 26.5 |
| b-3 | n1@9 n3@22 n2@34 n1@46 n2@58 | up, +0.8 (`P`: a member's COMMIT not yet answered `OK`, probably still recovering) | 0.10 | 361 | 2.7 | 25.5 |
| b-4 | n1@9 n2@21 n3@33 n1@45 n2@58 | n2 down | 0.16 | 285 | 2.6 | 14.0 |
| b-5 | n1@9 n2@21 n3@33 n1@45 n2@58 n3@70 | n3 down | 0.18 | 412 | 2.6 | 25.6 |
| b-6 | n1@9 n3@22 n2@34 n1@46 n3@58 n1@71 | n1 down | 0.23 | 397 | 2.6 | 25.8 |
| b-7 | n1@9 n3@22 n2@34 n1@46 n3@58 n1@71 | up, +0.6 (`P`) | 0.18 | 378 | 2.6 | 25.4 |
| b-8 | n1@9 n3@21 n2@33 n1@45 n3@58 | up, +4.5 | 0.12 | 336 | 4.8 | 27.5 |
| b-9 | n1@9 n3@21 n1@33 n2@45 n3@58 | up, +3.2 | 0.11 | 340 | 3.4 | 26.5 |
| b-10 | n3@9 n1@22 n3@34 n2@46 n3@58 | n3 down | 0.17 | 361 | 2.6 | 20.3 |
| b-11 | n1@8 n3@21 n1@33 n2@45 n1@57 n3@70 n2@82 | n2 down | 0.12 | 402 | 2.6 | 26.3 |
| b-12 | n1@9 n3@21 n1@33 n3@45 n1@58 n2@70 | up, +0.9 (`P`) | 0.14 | 376 | 2.6 | 25.8 |

- **Task EY's reading needs one correction.** The final fsync fell inside the
  last kill's down window in 5 of 14 runs (and during a kill in `b-2`), and
  after every member was back in the other 8. The result is the same either
  way, so the down window is not the cause; the earlier kills are.
- **The refused stripes are most of the file.** `Refused stripes` is the 8
  named stripes plus the "N more stripes without proof" count of the first
  `fsync` call: 252 to 412 for a file of about 250 chunks, because a chunk has a base object and spans, all
  of them live. The instrumented client names all of them:

| Run | Refused | `LLL` | `DLL` | `LLM` | `DLM` | Degraded successes `DDK` |
| --- | --- | --- | --- | --- | --- | --- |
| obs-1 | 310 | 174 | 53 | 42 | 41 | 74 |
| obs-2 | 388 | 165 | 68 | 115 | 40 | 56 |

  Letters are shown as a multiset (node order dropped). 70 and 72 % of the
  refused stripes have no durable fragment at all (`LLL`, `LLM`); the rest
  have one (`DLL`, `DLM`). Only `LLM` and `DLM` (27 % and 40 % of them) are
  stripes a W154 rebuild repaired.
  Stripes with one restarted member (`DDL` plus a mark) passed as `DDK`.
- **No re-send.** The log line is `stripe_log()`'s
  `fsync ino=… chunk=… gen=… fragments=…` (`data_commit.c:344`): one
  refusal line per stripe per `fsync` call (four writers' fsyncs plus the
  post fsyncs). Across those calls no stripe ever gained a `D` and none went
  from `L` back to `P` (a re-acknowledgement would reset it to `P`,
  `stripe_note_locked()` `:183`); `scripts/evidence/k110/analyze_runs.py`
  counts 0 such transitions in 14 of 14 runs. The only `put_fragments` lines
  after a first refusal (runs `b-5`, `b-6`, `b-10`, `b-11`) are new
  generations of the file's tail chunk 249 written by the fsync's own flush
  (the member was still down: "degraded acknowledgement"), never a refused
  `(chunk, gen)`.
- **Control.** `fe-1` to `fe-3` (`--fsync-every 100`, same kills, no
  `need 2` line, `PASS`): final fsync 0 for all writers, post fsyncs 0,
  mid-run fsyncs OK while a member was down (`fe-1` logged 30 "marked degraded for
  rebuild" lines, `K`).

## Trace of one refused stripe (step 3)

Run `b-1`, chunk 187, object `2c5fded8eb66ce47`, `nodes=3,1,2` (fragment 0
on n3, 1 on n1, 2 on n2). Times are UTC from `fuse-mnt.log` and the
`efsd-n*.log` files; the kill times are from `report.json`.

| Time | Event |
| --- | --- |
| about 06:56:22 | n3 SIGKILLed (its boot 2; its record says durable seq 61). |
| 06:56:27.669 | The client PUTs the object while n3 is down: n1 and n2 acknowledge, n3 does not: `degraded acknowledgement, fragment 0 for node 3 not acknowledged; published degraded for rebuild on return`. The record is `M P P` in node order (n3, n1, n2): n1 and n2 pending, each with the boot epoch of that moment. The publication carries the W154 mark for n3. |
| 06:56:27.752 | n3 restarts (boot 3); verified recovery clears its fence 06:56:29.250. |
| 06:56:30.142 | n1, leader of the group, rebuilds fragment 0 on n3 from n1 and n2 (`raft-host: rebuilt … fragment=0 on node 3`): it reads both sources, checks the rebuilt fragment against the published digest, PUTs it, sends a COMMIT to n3 and requires the same boot epoch (`dg_put_fragment()` `raft_host.c:12066`), then proposes the mark's clearing. The client is not told. |
| about 06:56:35 | n2 SIGKILLed (its boot 1; its record says durable seq 48). It restarts 06:56:40.035 as boot 2; recovery clears its fence 06:56:41.765, `files checked=420 dropped=0`. |
| 06:56:42.840 | The first `fsync`: COMMIT to n1: epoch unchanged, fragment 1 becomes `D`. COMMIT to n2: `OK` with boot 2, but the fragment's recorded epoch is boot 1: fragment 2 becomes `L`. n3 is not asked (`M`). Result `M D L`: 1 of 3 durable. The proof branch needs two `D`; the object is live (the current published version of chunk 187), so `stripe_close_for_absence()` and `efs_client_rpc_object_live()` do not retire it. `EIO`. |
| 06:57:07.405 | The second `fsync` (post fsync): the same line, the same letters. |

**On disk** (listing taken from the kept store, `b-1.traced-fragments.txt`):
all three fragment files of chunk 187 exist (`187.1.…` on n1, `187.2.…` on n2,
`187.0.…` on n3, 69,632 bytes each, mtimes 06:56:27 and 06:56:30), as do the
three of chunk 186; the cold mount read all 8,000 records exact. The
fragments' digests were not recomputed independently; the cold read and the
rebuild's checksum comparison are the evidence.

**Why the `L` fragment does not count while its member is up.** The member is
up, but n2's boot 2 never acknowledged the fragment: boot 1 did, and boot 1
died. The only proof the client accepts is a COMMIT answered after the
acknowledgement by the same boot epoch. That proof can never exist for boot 1
again. Nothing on n2 records the fragment as barriered: the put-sync record
holds only the last barrier seq of the whole root (61, 48 in the log lines),
the PUT reply carries no seq, so the client cannot compare. (A fragment
acknowledged inside the last barrier window before a kill may be lost under
power loss; one acknowledged earlier was flushed by the background `syncfs`
within its 200 ms window, but nothing the client can read says which it was.)

**What would make it count today:** nothing.

- The fold reads only `P` fragments (`:505`); `L` is terminal.
- `P` is restored only by a new acknowledgement of the same object
  (`stripe_note_locked()` `:183`), i.e. a re-PUT of the object's fragment.
- `stripe_close_for_absence()` / `efs_client_rpc_object_live()` retire a
  stripe only when its object is no longer live (rewritten, truncated, packed,
  unlinked). Here it is live.
- The degraded mark path needs two `D`.
- K36's recovery proves fragments to the member itself, not to a client.
- No RPC re-verifies a named fragment under the current incarnation.

**Does the client still hold the stripe's bytes?** No. `struct stripe` keeps
identity, nodes, ack seq and epochs only. The dirty-chunk cache entry is
dropped as soon as the PUT succeeds (`efs_dcache_drop()` after a successful
`efs_client_put_fragments_parallel()`, `write.c:9381` and `:9434`), and a
published, clean object is not an obligation the table references; the header
of `data_commit.c` says so (`:16` to `:18`: "the payload may have been
released already: never resent, never forgotten"). A re-send is therefore not
possible from the client's own state; it would have to read the chunk back (a
new object, a new REPORT, racing the other writers' appends) or keep every
unproven body (up to `2^18` records of 128 KiB, 32 GiB).

## Why this is not simply a bug

The written rules produce this result. D42 F22 asks for proof per stripe
that includes "no unresolved publication or storage-incarnation ambiguity";
F23 says recovery does not prove an earlier ambiguous write; the K36 page
states that a fragment the crashed run acknowledged and did not COMMIT "stays
lost in its client's per-stripe `fsync`". The code does exactly that. What the
rules do not say is that the cost is permanent for every stripe written
before the second of its members restarted, nor what an application should do.
In the failing runs every member was killed at least once and a stripe written
early has been through three; a single member's crash is tolerated (`DDL`
passes with a mark), two are not.

## Options (step 5: none is unambiguous, so no code was changed)

1. **Contract and test criterion (no code).** State that `fsync` can fail
   permanently for stripes whose members restarted uncleanly between the
   write and the `fsync`, and that the application recovers by rewriting or by
   fsyncing often enough (the control passed 3 of 3 with a fsync about every
   2.5 s). The kill-mode criterion becomes "every writer fsyncs periodically
   (`--fsync-every`), the final fsync and a post fsync after the last member
   returns are 0". Cost: the application-visible hazard stays; a user must
   accept that D40's "available with one member down" has this edge.
2. **Cross-incarnation barrier proof (recommended technical fix).** The
   server already makes a durable statement per boot: after each `syncfs` it
   fdatasyncs the barrier seq. Extend it so a client can use it: the PUT reply
   carries the root and the barrier-window seq of the acknowledgement; the
   durability record keeps a short history of (verifier, last durable seq)
   per root; the COMMIT reply returns it; the client turns an `L` fragment
   into `D` when its epoch is in the history and its ack seq is not above that
   epoch's durable seq. Proof stated: the predecessor's record, fdatasync'd
   on that member after a `syncfs` of the root that covers the fragment's
   write. Fail closed: an acknowledgement inside the last unfinished window,
   a missing history entry or a root-count mismatch stays `L`. It handles
   `LLL`, `DLL` and the `L` of `LLM`/`DLM`. Needs a wire change (PUT reply,
   COMMIT reply), a new durability-record version (no compatibility needed),
   a client fold change and tests (fast unit tests of the fold and of the
   record; one live kill run); **and a decision that amends the sentence in
   D42 F23 and the K36 page** ("never counts it durable"). Power-loss scope
   (K23/W71) is unchanged: this proves what a normal COMMIT proves.
3. **Member-side re-verification.** After an unclean boot the member's
   recovery already reads every fragment and checks its digest; add a
   `syncfs` after that and let the client verify a named `L` fragment (a read
   with the digest from the chunk row), followed by a COMMIT whose epoch
   equals the reply it saw before the read. No server record format change,
   but a 64 KiB read per `L` fragment inside the fsync (300 to 400 per run),
   a new client path, and the same amendment of F23 as option 2.
4. **Count a rebuilt `M` slot** (the cleared W154 mark is Raft-durable
   evidence of an epoch-checked COMMIT by the rebuild, `dg_put_fragment()`).
   Needs the chunk reply to expose the mark state. It repairs only
   `LLM` and `DLM` (27 and 40 % of the refused stripes) and nothing else;
   keep it as a refinement of option 2, not a fix.
5. **Client rewrite or retained bodies** (read back and write a new object;
   or keep every unproven body). Rejected above: races with concurrent
   appends, contradicts the "never resent" rule, unbounded memory.

Waiting for the restarted member and re-sending from held bytes are the two
retry paths class B names; neither is possible (the epoch is permanent; the
bytes are gone).

## Proposed K110 wording (the K tables are not edited here)

Register row, `Item | Kind | Disposition`:

````text
Task EY (batch D H1, candidate `9bb50fe79`, Nov 10), diagnosed by Task FH ([evidence](2026-11/k110-fsync-after-sigkill-20261110.md)): `tests/live/append_failover.py --failover kill` (three loopback members, four writers on one shared file, the current leader SIGKILLed about every 12 s and restarted after 5 s, one `fsync` at the end) fails its final `fsync` with `EIO` in 14 of 14 private runs (3 of 4 on Task EY's). Not a transient: a second `fsync` after every member is back fails too (2.6 to 6.4 s and 14 to 30 s after the last restart), and so does every later one until the client unmounts. Every byte reads back exact. Cause: a fragment acknowledged by an incarnation that was SIGKILLed before any COMMIT (`L`) is never counted (K35/K36 rule), the client no longer holds the bytes, and the server publishes no per-fragment barrier record; 70 to 72 % of the refused stripes have no durable fragment at all. A control with `--fsync-every 100` passes 3 of 3 | durability proof / fsync availability across member restarts, alpha blocker | OPEN, **blocks alpha: yes**: decision owed (user): option 2 (cross-incarnation barrier proof, amends D42 F23) or option 1 (documented limit and a periodic-fsync test); the H1 kill modes rerun on the outcome (TCP and RDMA).
````

Status row:

````text
OPEN, **blocks alpha: yes** (Nov 10, Task FH): kill-mode final `fsync` `EIO` is permanent after two members of a stripe restart uncleanly between its write and the `fsync`; data exact, no code defect found in the fold, proof model/contract decision owed; periodic-fsync control 3 of 3 PASS ([evidence](2026-11/k110-fsync-after-sigkill-20261110.md)).
````

Reason for `yes`: as the code stands an application that sees two different
members crash, one after the other, between a write and its `fsync` gets `EIO`
from every later `fsync` of that file with all data intact and no remedy
except rewriting, and the project's own kill gate cannot pass. If the user
picks option 1, the row becomes a documented limitation and `blocks alpha: no`.

## Reproduction

Prerequisites: a Linux host with the fuse prefix, a tree built with
`make EFS_FUSE_PREFIX=<prefix> efsd efs-fuse efs-mgmt`, free ports 38101 to
38103, Python 3. From the tree root (`WORK` defaults to `/tmp/efs-k110`):

```bash
WORK=/tmp/efs-k110 scripts/evidence/k110/run_series.sh 1 12 b "$PWD"
```

```bash
python3 scripts/evidence/k110/analyze_runs.py /tmp/efs-k110/out
```

Control: `scripts/evidence/k110/run_series.sh 1 3 fe "$PWD" --fsync-every 100`.
Full stripe histogram: build `efs-fuse` with the two `< 8` caps in
`stripe_prove()` raised, run `run_series.sh 1 2 obs <that tree>` and
`python3 scripts/evidence/k110/stripe_histogram.py /tmp/efs-k110/out obs-1 obs-2`.
Each run takes about two minutes and keeps a 1 GB store under
`$WORK/runs`; delete the tree when done. Expected: `rc=1` in the summary,
`final_fsync_zero` false, `post_recovery_fsync` errno 5 twice; with
`--fsync-every 100`, `rc=0`.

## Not checked

- RDMA, distinct hosts and the combined faults of Task EY's `combined-2k`
  (its one pass of two was not repeated); one host, loopback only.
- Power loss (K23/W71 stay deferred); no run tests what an application
  can do after the failure beyond the post fsyncs.
- The fragments' digests on disk were not recomputed for the refused stripes;
  readability was shown by the cold read of all 8,000 records.
- A `fsync` from a freshly mounted client (it finds an empty table and
  returns 0 without proof, by the existing design) was not run.
- None of the options was implemented or sized beyond the description above,
  so their effect on the kill gate (for example the unbarriered tail window)
  is predicted, not measured.
- `make test`, the sanitizer lanes and the K35/K36 tests were not run: no
  product code changed.
- The node was shared; run durations (93 to 138 s) vary with its load.

## Raw captures

Outside Git on node9900: `~erbmi1/efs-artifacts/taskFH-20261110/out/` (per
run `*.out`, `*.report.json`, `*.logs/` with `fuse-mnt.log` and
`efsd-n*.log`; `b-1.traced-fragments.txt`; `host-and-binaries.txt`; the three
series scripts and the build logs) with
`~erbmi1/efs-artifacts/taskFH-20261110/SHA256SUMS` (182 files; SHA256 of the
manifest `c68cfb866781800af0fc64ac17646bd16527a62450c099059cf16b7217317020`).
The run stores (28 GB) and `/tmp/efs-taskFH*` were deleted after the logs were
copied; they are recreated by the commands above.
