# K110 · the final fsync returns EIO after sequential SIGKILLs (Task FH, Nov 10 2026)

**Verdict: FIXED on branch `claude/determined-mclean-7c3b6f` (barrier proof,
user decision "Barrier proof" = option 2 below; D42 F23 amended). Not merged,
not pushed; VERIFIED waits for the coordinator's merge gates.** Before the fix
the final `fsync` failed in 14 of 14 private runs and was permanent (class B).
After it, in 28 kill-mode runs with the final `fsync` taken once the failover
controller has stopped and every restarted member has finished verified
recovery, all four writers' final `fsync`, both post fsyncs and the integrity
check passed in 28 of 28; 25 of the 28 runs PASS, and the other 3 fail only on
a REPORT latency criterion that fails in 3 of 14 pre-fix runs too (see "Other
kill-gate failures").

Two sentences for the K110 row. The final `fsync` failed because a stripe's
fragments were acknowledged by member incarnations that were SIGKILLed before
any COMMIT, and the client proved a fragment only through a COMMIT answered by
the incarnation that acknowledged it, so such a fragment was lost forever
although the dead incarnation's own durability barrier had covered it. Each PUT
reply now carries the acknowledgement's barrier position, a restarted member
reports its predecessors' last completed barriers in its COMMIT reply (only
when verified recovery dropped no file those barriers cover), and the client
counts a fragment durable when a reported barrier is at or after its position.

## Design (option 2)

Decision: user, Nov 10, "Build the fix" / "Barrier proof" (option 2 below).
Written before the second round of code changes (root identity in the record,
old record versions refused, a failed record sync reverted); sections "Wire",
"Record" and "Proof" describe the code after them.

**Record.** One file per storage root, `<root>/.efs-durable-epoch`, inside the
store it describes (so a wiped or replaced store takes its history with it).
Fixed 200 bytes, big-endian `u64` fields, magic `EFSDUR03`: `0` magic, `8`
boot, `16` clean, `24` seq (the last barrier of this boot that completed, 0
before the first), `32` wall time of the last write, `40` verifier, `48` root
count, `56` ring_ok, `64` ring_n, `72 + 16 i` ring entry `{verifier, position}`
for `i < 7` (position = `efs_put_pos(boot, seq)`, boot 16 bits, seq 40 bits,
non-zero), `184` root index, `192` CRC-32 of bytes 0 to 191. A record that is
short, fails its CRC, has a field out of range or any other magic (the v1 text
and `EFSDUR02` records included) makes `efsd` refuse to start with a clear
error; nothing is migrated or skipped.

**Writes** (`store.c`). The record changes in four places, each a `pwrite` of the
whole 200 bytes followed by `fdatasync`: (1) attach, a new boot: seq 0, and the
predecessor's `(verifier, efs_put_pos(boot, seq))` pushed to the ring front
with the predecessor's own ring behind it when that was valid, only when the
predecessor stopped unclean or had another verifier, and only when its root
count and root index equal the current ones (otherwise the ring starts empty);
(2) each barrier: `syncfs(root)` first, then the record with seq = the
barrier's epoch, then `fdatasync`; `durable_seq` is raised only after that
returned, and when the `fdatasync` fails the record is rewritten (no sync) with
the previous durable seq so the file never names a barrier whose record sync
failed; (3) verified recovery (K36): `ring_ok`, or the ring forgotten, before
the fence clears; (4) orderly stop: final barrier, then `clean`. The record
costs nothing extra on the PUT path: it is the file the barrier already wrote.

**Wire.** PUT reply (`EFS_MSG_PUT_CHUNK_REPLY`): 18 bytes (was 10), the status,
root index, verifier and an *ack* `u64`: root index (top byte) | `efs_put_pos`
(boot 16 bits, seq 40 bits). +8 bytes per PUT, no round trip. 0 means no
barrier (benchmark build, boot above 65535, seq above 2^40): fail closed.
COMMIT reply: the existing header, `history` and `reserved` (+8 bytes), then
`history` entries of 24 bytes `{verifier, position, root}`: each validated
predecessor of each root, newest first; none from a root still fenced, in a
barrier error, or whose ring recovery forgot. A reply whose length is not
exactly `sizeof(header) + 24 history` is rejected (`efs_data_commit_reply_ok`)
and counts as no reply.

**Condition (client, `data_commit.c`).** A fragment with ack `a` (root `r`,
position `p`) acknowledged by verifier `V` is `P`. The COMMIT answer carries
verifier `V'` and a history `H`. If `V' == V` the fragment is `D` (unchanged). If
`V' != V` it is `D` iff `H` has an entry with verifier `V`, root `r` and
position `>= p`; otherwise `L` (unchanged). `D` is counted exactly as before:
three `D`, or two `D` and the third covered by a mark (published `M` or a mark
written now), else `EIO`. Option 4 (a rebuilt `M` slot) is not built and stays
a follow-up. No path returns 0 from `fsync` without two proven fragments per
stripe.

**Proof.** Claim: a fragment acknowledged with position `(E, N)` by
incarnation `E` on root `r` is on stable storage whenever a record of `E` read
back names seq `>= N` for that boot.
1. A fragment file is written and closed (`finish_shard_write`) before
   `put_sync_note` and before the handler reads the position
   (`handler.c:550`, after `server_put_upload` returned; fragments are written
   in place, there is no rename).
2. The position is read under `r->mu` as `r->seq`, the *open* epoch. A barrier
   takes `target = r->seq` and increments `r->seq` under the same mutex
   (`put_sync_barrier_locked`) before it calls `syncfs`. So when the read
   returns `N`, no barrier with target `N` had started, and its `syncfs`
   starts after the fragment's write completed. The tag is therefore already
   "the next barrier that has not started"; no change to the tagging is needed
   (the earlier comment in `handler.c`, "can only over-state", is the safe
   direction: a later position is harder to cover).
3. `syncfs` returns only after all data and metadata written before it began
   are written and the journal committed; the following record `fdatasync`
   issues the device flush that also covers `O_DIRECT` overwrites (the existing
   design of the barrier).
4. The record names seq `N` only after step 3 returned OK for target `N` (the
   `pwrite` is after `syncfs`), and with the revert the file does not keep `N`
   when the record sync failed. A later barrier covers fragments of a failed
   one, so a record naming `N' >= N` is sound.
5. The successor reads that record at attach and offers `(E, position)` only
   after verified recovery (K36) dropped no file the earlier barriers could
   have covered (mtime not after the predecessor's last record), and the
   history is dropped when the root count or index differ. A file recovery
   dropped is a fragment the history would have claimed.

Assumptions that are not checked at run time: the wall clock does not step back
across a restart by more than the file times compared in 5; the file system
honours `syncfs`; a store is not partially wiped while its record is kept (a
wiped store has no record, hence no history). Scope: process kill and OS crash;
power-loss qualification stays a separate scope decision.

**Fail closed, with a test each.**

| Case | Result | Test |
| --- | --- | --- |
| ack inside the last unfinished window (position after the predecessor's last barrier) | `L` | `test_data_commit` (`p101`), `test_store_durability` (position vs durable seq) |
| history entry missing (no entry, none for that verifier or root, empty history) | `L` | `test_data_commit` fold table |
| entry truncated (reply length is not header plus entries) | reply rejected, fragment unproven | `test_data_commit` malformed reply |
| root count or root index mismatch at attach | ring dropped, nothing offered | `test_store_durability` |
| record fails its CRC, is short, has another magic or version | `efsd` refuses to start | `test_store_durability` |
| store wiped (no record) or another store's record (no matching verifier) | ring empty, `L` | `test_store_durability` |
| recovery dropped a covered file, or a boot killed before its recovery finished | ring forgotten, `L` | `test_store_durability` |
| incarnation older than the ring | `L` | `test_store_durability` (bound) |
| reply without history, ack 0, boot above 65535 | `L` | `test_data_commit`, `test_store_durability` |

**Bound.** The ring keeps the 7 newest predecessors per root. Each unclean
restart pushes one; a fragment is `L` only until `fsync` or the next COMMIT, and
`fsync` is the only caller. Seven unclean restarts of one member between a
write and the `fsync` that covers it is far above the 4 to 7 kills of a whole
cluster over 90 s in the kill gate (a member took at most 3), and above any
period an application leaves data without `fsync` while members crash that
often. An entry past the bound is not offered and the fragment stays `L`: a
loss of availability, not of safety. The ring costs 112 bytes in the record and
at most 168 bytes per root in a COMMIT reply, only for a member with unclean
predecessors.

**No compatibility.** The wire and record versions are new, old records and
old replies are refused (a PUT reply of 10 bytes, a COMMIT reply without the
history fields); a freshly formatted cluster is assumed.

## Results after the fix

Build: source `8082fc2d2` (no `src/` or `include/` change since; the harness
option `--settle-before-fsync` and the test files are those of `f57c8fca3`),
copied with `git archive`, built on node9900
(Linux 5.14.0-687.39.1.el9_8, gcc 11.5.0) with `make -j8
EFS_FUSE_PREFIX=/data1/efs/deps/libfuse efsd efs-fuse efs-mgmt`; binaries
report `build=unknown`. Same environment as the diagnosis runs below (three
loopback members, TCP, direct I/O, 4 writers, 8,000 records, leader kills about
every 12 s, down 5 s, quota 8 G, private ports 38101 to 38103 and 38401 to
38403, two series in parallel on a shared host).

| Series | Runs | Final `fsync` 0 for all four writers | Post fsyncs 0 | Integrity (8,000 of 8,000, three views identical) | Verdict PASS |
| --- | --- | --- | --- | --- | --- |
| Before the fix (diagnosis, `a`, `b`) | 14 | 0 of 14 | 0 of 14 | 14 of 14 | 0 of 14 |
| After, final `fsync` concurrent with the failover (`fa`, `fb`) | 28 | 18 of 28 | 28 of 28 | 28 of 28 | 17 of 28 |
| After, `--settle-before-fsync` (`sa`, `sb`) | 28 | **28 of 28** | **28 of 28** | **28 of 28** | 25 of 28 |

- The fragments the barrier history proved: 21,598 (`fa`, `fb`) and 23,254
  (`sa`, `sb`); fragments counted lost: 532 and 703 (acknowledged after their
  incarnation's last barrier or not covered). In the settled series all 703
  were marked degraded for rebuild (`K`) with the other two members durable, so
  `fsync` returned 0 with a visible degraded mark, as K35 prescribes, and no
  lost fragment was counted durable.
- Verified recovery dropped no file in 55 of the 56 runs and one 0-byte file in
  `sb-11` (a PUT file still in flight when the member was killed, modified
  after the predecessor's last record; the proof counts of that run
  (861 proven, 38 lost) are the same as its neighbours', so the ring stayed
  valid; no log line states the ring verdict, which is a gap).
- Why two series after the fix. In the unsettled `fa`/`fb` series 10 runs failed
  `final_fsync_zero` with `EIO` on all or some writers while both post fsyncs
  returned 0: the harness takes the final `fsync` while its failover controller
  is still killing and restarting members, and an `fsync` that lands in the 5 s
  down window or inside a restarted member's recovery fence (about 0.2 to 1 s,
  K36) has two of its three members unavailable (stripes `PMD`, `DPM`, `MDP`:
  one durable, one missing, one pending) and fails closed. Checked on `fa-5`
  (last kill 4.6 s before the writers ended, the node restarting during the
  fsync) and `fb-1` (the fsync at 08:20:12.77 inside node 1's recovery, which
  verified at 08:20:12.888); the other transient runs were not timed
  individually. That is class A (transient, correct by D42). The new
  `append_failover.py --settle-before-fsync` makes every writer wait for its
  final `fsync` until the controller has stopped and every restarted member
  logged `recovery: verified`; the gate then tests what K110 is about (the
  `fsync` after the kills), not an `fsync` taken during an outage.
  Unchanged default: without the flag the harness behaves as before.
- Whether COMMIT should wait out the recovery fence instead of answering `EIO`
  (so the unsettled `fsync` also succeeds) is a product decision and is not
  made here; K36 chose the `EIO` on purpose.

### Other kill-gate failures (not K110)

Five of the 56 runs (`fa-5`, `fa-8`, `sa-1`, `sa-11`, `sb-14`) fail a REPORT
criterion of the same harness: a type-67 REPORT waited the 30 s receive bound
(`report_recv_timeouts`, 4 runs) or took 4.8 s across one leader change
(bound 3 s per change, `sa-1`). The pre-fix series has the same failures in 3 of
14 runs (`b-9` recv timeout, `b-3` and `b-11` over the latency bound with 6.4 s),
so the change did not introduce them; they belong to the leader-failover latency
family tracked as K104 (Task FF). Data and `fsync` are exact in all five runs
(integrity PASS, post fsyncs 0). They keep the kill gate from passing every run
and need their own fix.

### Tests

- `tests/test_k110_barrier_history.py` (new, `make test-long`; budget 10 s):
  three members and one mount, a 3 MiB file written without `fsync`, every
  node's barrier covers it, node 3 then node 2 SIGKILLed and restarted, then
  `fsync` must return 0 with the client's line `restarted members: N fragments
  proven by their barrier history, 0 lost` (N at least 2) and no degraded mark.
  **Fails on the pre-fix tree** (`4a24916a1`, built on node9900): `OSError:
  [Errno 5] Input/output error` at the `fsync`. **Passes on the fix**
  (5.5 s and 7 s, node9900).
- `tests/test_data_commit.c` (client fold: two restarted members covered by
  their history; one restarted member whose fragment was acknowledged past the
  barrier, on another root, without a position, by another incarnation, or
  whose history is for another root: lost, the mark covers it),
  `tests/test_store_durability.c` (ring across unclean boots, validation by a
  verified recovery without a covered drop, forgetting after a covered drop,
  a boot killed before its recovery finished, clean stop, the drop rule and the
  position encoding),
  and the arity updates in `test_put_retry_budget.py`,
  `test_put_reply_deadline.py`, `test_put_tcp_frame.py` and the live drivers.
- `tests/test_k36_recovery.py` (its step 4 still asserts the fragments written
  after the last barrier are lost) and `tests/test_k35_fsync_degraded.py` pass
  on the fix (24 s and 129 s).
- The full gate: see "Gate" below.

## Diagnosis before the fix

The sections up to "Options" are the diagnosis made on devel `9403d0666`
before any change; their present tense describes that tree, not the fixed one.

### Class (step 4)

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

### Source and environment

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

### The letters (step 1)

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

### Runs (step 2)

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

### Trace of one refused stripe (step 3)

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

### Why this is not simply a bug

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

## Options (step 5; the user chose option 2)

At diagnosis time none was unambiguous, so no code was changed; on Nov 10 the
user answered "Barrier proof", which is option 2. Options 1, 3, 4 and 5 were not
built; option 4 (counting a rebuilt `M` slot) stays a possible refinement.

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
Task EY (batch D H1, candidate `9bb50fe79`, Nov 10), diagnosed by Task FH ([evidence](2026-11/k110-fsync-after-sigkill-20261110.md)): `tests/live/append_failover.py --failover kill` failed its final `fsync` with `EIO` in 14 of 14 private runs, permanently (a second `fsync` after every member was back failed too), with every byte reading back exact. Cause: a fragment acknowledged by an incarnation that was SIGKILLed before any COMMIT was counted lost forever (K35/K36 rule), although that incarnation's own durability barrier had covered it | durability proof / fsync availability across member restarts | FIXED on branch `claude/determined-mclean-7c3b6f` (Task FH, not merged): the user chose the cross-incarnation barrier proof ("Barrier proof", amends D42 F23). The PUT reply carries the acknowledgement's barrier position, the durability record v3 keeps a ring of predecessors' last completed barriers (valid only after a verified recovery that dropped no covered file), the COMMIT reply returns it, and the client counts a covered fragment durable. After the fix 28 of 28 kill-mode runs with the final `fsync` taken after the failover controller stopped and the members' recovery verified have every final `fsync`, both post fsyncs and the integrity check pass (25 of 28 runs PASS; the other 3 fail only the REPORT latency criterion that fails in 3 of 14 pre-fix runs too); an `fsync` landing inside a member's outage or recovery fence still returns `EIO` by design (28 unsettled runs: 10 transient). New `tests/test_k110_barrier_history.py` fails before and passes after. VERIFIED on the merge gates; not covered: RDMA, power loss, `tests/live/rdma_async_commit.py` still expects the old `EIO` after a restart
````

Status row:

````text
FIXED, **blocks alpha: no** (Nov 10, Task FH): barrier-proof `fsync` after sequential SIGKILLs, 28 of 28 settled kill-mode runs with every final `fsync` 0 and data exact, new regression test fails before and passes after; VERIFIED awaits merge gates; the kill gate's remaining failures are REPORT latency (3 of 28 after, 3 of 14 before; K104 family), and `fsync` during a member outage or recovery fence is `EIO` by design ([evidence](2026-11/k110-fsync-after-sigkill-20261110.md)).
````

`blocks alpha: no`: the persistent loss of an `fsync` after two members of a
stripe restarted is gone; what remains is an `EIO` while a member is down or
recovering, which D42 F22 and K36 allow and the application can retry. It turns
to `yes` if the merge gate shows a persistent `fsync` failure after the fix, or
if the user wants `fsync` to wait out the recovery fence (a separate change).

## Reproduction

Prerequisites: a Linux host with the fuse prefix, a tree built with
`make EFS_FUSE_PREFIX=<prefix> efsd efs-fuse efs-mgmt tests/faults/efsd`
(the last for the unit and live tests), free ports 38101 to 38103, Python 3.
From the tree root (`WORK` defaults to `/tmp/efs-k110`):

```bash
WORK=/tmp/efs-k110 scripts/evidence/k110/run_series.sh 1 14 sa "$PWD" --settle-before-fsync
```

```bash
python3 scripts/evidence/k110/summarize_fix.py /tmp/efs-k110/out
```

Expected on the fix: `final_fsync_errnos 0000`, `post_fsync_errnos 00`,
`recovery_dropped` 0 or a single 0-byte file, a positive `proven_by_history`
column; a run may still fail a REPORT criterion (see above). Without
`--settle-before-fsync` the final `fsync` may land in an outage and return
`EIO` while both post fsyncs return 0. On the pre-fix tree (`4a24916a1`)
`python3 scripts/evidence/k110/analyze_runs.py /tmp/efs-k110/out` shows the
permanent failure; control: `run_series.sh 1 3 fe "$PWD" --fsync-every 100`.
Regression test: `python3 tests/test_k110_barrier_history.py` (needs
`/dev/fuse` and `tests/faults/efsd`; about 6 s). Full stripe histogram of the
diagnosis: build `efs-fuse` with the two `< 8` caps in `stripe_prove()` raised,
run `run_series.sh 1 2 obs <that tree>` and
`python3 scripts/evidence/k110/stripe_histogram.py /tmp/efs-k110/out obs-1 obs-2`.
Each run takes about two minutes and keeps a 1 GB store under `$WORK/runs`;
delete the tree when done (`~/bin/edelete --force --delete`).

## Gate

Run on node9900 from `git archive` of `f57c8fca3` (the code of `8082fc2d2`),
three quiet checks 20 s apart first, other tasks' work not running:

- `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` (the `make test-all`
  gate): `TOTAL 595 FAIL 0 NOTRUN 4 STATUS COMPLETE (620s) HOSTLIMIT 4 BUILD
  14s`. The four NOTRUN are host limits (no `EFS_CLUSTER_SCRIPTS`, no
  valgrind, no RDMA hardware). The K110 test took 7.7 s in the pool
  (budget 10 s), `test_k36_recovery.py` 22.2 s, `test_k35_fsync_degraded.py`
  129.6 s, `test_data_commit` 4.6 s, `test_store_durability` 1.5 s.
- `bash tests/sanitizer_check.sh tsan` (after adding the K110 test to both
  lists): `VERDICT: PASS (32 run, 0 failed, 628s)`, including
  `k110_barrier_history`.
- `bash tests/sanitizer_check.sh asan`: `VERDICT: PASS (60 run, 0 failed,
  370s)`, including `k110_barrier_history` (LeakSanitizer ran only where
  ptrace allows it; the gate prints which).
- `make docs-check` and `python3 scripts/check_doc_links.py` pass.

## Not checked

- RDMA (the PUT and COMMIT reply changes are in the RDMA path and the buffered
  path too, built and exercised only through the TCP tests), distinct hosts and
  the combined faults of Task EY's `combined-2k`; one host, loopback only.
- `tests/live/rdma_async_commit.py` needs an RDMA device and was not run. It
  asserts `EIO` for a node killed right after a write; with the barrier proof a
  write older than the 200 ms barrier window now fsyncs 0 after the kill, so
  its expectation (and the magic `EFSDUR03`, already updated) must be revisited
  by whoever runs it.
- Power loss (K23/W71 stay deferred): the proof is the same kind as a normal
  COMMIT, `syncfs` plus an fdatasynced record, and says nothing beyond it.
- The wall-clock assumption of the validity rule (a restart does not step the
  clock back past file modification times) is not checked at run time.
- No log line states whether a ring was kept or forgotten at a recovery
  (inferred from the proof counts in `sb-11`).
- The 10 transient final-fsync failures of the unsettled series were timed
  against the failover events for `fa-5` and `fb-1` only.
- Whether COMMIT should wait out the recovery fence instead of `EIO` was not
  decided or built; the REPORT failures of the kill gate (K104 family) were not
  investigated here.
- A `fsync` from a freshly mounted client (empty table, returns 0 by design)
  was not run.
- The node was shared; run durations (74 to 138 s) vary with its load.

## Raw captures

Outside Git on node9900:
`~erbmi1/efs-artifacts/taskFH-barrier-20261110/` (per series directory
`series-unsettled-fa`, `-fb`, `series-settled-sa`, `-sb`: each run's `*.out`,
`*.report.json`, `*.logs/` with `fuse-mnt.log` and `efsd-n*.log`; `tests/` with
the test outputs, the pre-fix K110 test failure `base-k110.out`, build logs and
the driver scripts; `gate/` with `summary.txt` and `timings.tsv`; `sanitizers/`;
`host-and-binaries.txt`) with `SHA256SUMS` (532 files; SHA256 of the manifest
`887d7d88cb5c447e26afdc8adecd29d0a7dabd67d8a746aa84f7dc11e6ed009d`). Binary
SHA256 of the tested tree: `efsd`
`f16934e90c697b29249a18685c071a13ce889800d2e3b8c173b16a530c9ecdc5`, `efs-fuse`
`2c475bb73008a9453a513bc5693be4f8f8bd6ea8befa704b808451a4b63fe101`, `efs-mgmt`
`a37cd448a95243702bf21aa3d8508804e34c5c46c958f2736e42afcd1cad9e09`; the pre-fix
tree's `efsd` is `0e551f316ca4dc733b87a97de45c1b3e5e7fafb6950420ed554b88164e9a66aa`.
The diagnosis captures remain in `~erbmi1/efs-artifacts/taskFH-20261110/`
(manifest `c68cfb866781800af0fc64ac17646bd16527a62450c099059cf16b7217317020`).
The run stores and every `/tmp/efs-taskFH*` tree were deleted after the logs were
copied; the commands above recreate them.
