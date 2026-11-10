# K110 · the final fsync returns EIO after sequential SIGKILLs (Task FH, Nov 10 2026)

**Status (Nov 10):** merged into devel in wave 73; K110 entered the register as VERIFY (blocks alpha until the distinct-host H1 kill rerun passes), the residual stripe of answer (b) as K111 (OPEN, blocks alpha: no), and the D42 F23, K35 and K36 texts below were applied at the merge.

**Verdict: FIXED on branch `claude/determined-mclean-7c3b6f` (barrier proof, user:
"Build the fix", option 2; D42 F23 amended in "Text to apply at merge"). Not
merged, not pushed; the live distinct-host H1 kill rerun is owed and VERIFIED
waits for the coordinator's merge gates.** Before the fix the final `fsync`
failed in 14 of 14 private runs and was permanent (class B). After it, in 28
kill-mode runs on nuc with the final `fsync` taken once the failover
controller has stopped and every restarted member has finished verified
recovery, every writer's final `fsync`, both post fsyncs and the integrity
check passed in 28 of 28; 19 of the 28 runs PASS and the other 9 fail only on
a REPORT latency criterion that also fails in 3 of 14 pre-fix runs on another
host (see "Other kill-gate failures"). An earlier build of the same design
(without the root identity in the record) passed 28 of 28 final fsyncs on
node9900.

Proof in three sentences. A fragment is acknowledged with the root's *open*
barrier epoch, read under the lock that a barrier takes (and then increments)
before it calls `syncfs`, after the fragment's write and close completed, so
the barrier the acknowledgement names always begins after the write. That
barrier's record is written after `syncfs` returned and `fdatasync`ed (and is
reverted if that sync fails, and never raised after a barrier failed in that
boot), so a record that names seq `N` proves barrier `N` finished and every
fragment acknowledged at `N` or earlier is on stable storage. The successor
offers that record only after a verified recovery that dropped no file the
barrier could cover and found no published fragment missing without a mark
(either forgets the history at once), for the same root layout, and the
client counts a fragment durable only when verifier, root and position match;
every other case stays lost.

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
failed; (3) verified recovery (K36): `ring_ok` set; the ring is forgotten at the
moment a covered file is dropped or a published fragment is found lost
without a mark (`put_sync_void`, also after the fence cleared), and the
verdict write keeps the seq the record already names (`PUT_SYNC_KEEP_SEQ`), so
it cannot lower a barrier's seq; `ring_n`, `ring_ok` and the named seq change
and are read only under `rec_mu`, so a barrier racing the verdict never writes
voided entries as validated; (4) orderly stop: final barrier, then `clean`. The record
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
   when the record sync failed. **A later barrier does not cover fragments of
   a failed one** (corrected after review: Linux reports a writeback error
   once, so a `syncfs` after a failed one can return 0 although the pages of
   the failed one were dropped). Once a barrier failed in a boot, no later
   barrier of that boot may raise the record's seq or `durable_seq`
   (`tainted` in `put_sync_barrier_locked`); the record keeps the last barrier
   before the failure, and the successor offers nothing later.
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
| recovery dropped a covered file (also after the fence cleared), or a boot killed before its recovery finished | ring forgotten at the drop, `L` | `test_store_durability` (drop rule, after the fence) |
| recovery dropped or found a file under no known root | every root forgets, `L` | `test_store_durability` |
| a published fragment missing with no mark at publication (recovery, `rv_mark`) | every root forgets before the mark is written, `L` | `test_k36_recovery.py` step 3 (log line) |
| a barrier failed earlier in this boot, a later `syncfs` succeeded | record and `durable_seq` keep the last barrier before the failure; successor offers nothing later | `test_store_durability` (`test_failed_barrier_not_named`, back-off case) |
| verdict written while a barrier raised the seq | seq kept | `test_store_durability` (`test_recovered_keeps_record_seq`) |
| COMMIT reply entry with zero verifier, a position over 56 bits or a root out of range | whole reply rejected, fragment unproven | `test_data_commit` (modes 3 to 5) |
| incarnation older than the ring | `L` | `test_store_durability` (bound) |
| reply without history, ack 0, boot above 65535 | `L` | `test_data_commit`, `test_store_durability` |

**Bound.** The ring keeps the 7 newest predecessors per root. Each unclean
restart of a member pushes one entry. A fragment needs its own incarnation to
be among the 7 newest at the time of the `fsync`, so a write can be proven
across up to 7 unclean restarts of the member that holds it, between the write
and the `fsync` that covers it. That is above what the kill gate produces on
the reference host (4 to 7 kills in total over 90 s, spread over three members)
and above what an application should leave without an `fsync` while members
crash that often; a longer exposure is what periodic `fsync` is for (the
`--fsync-every 100` control). It was exceeded once on a slow host (22
restarts per member in 13 minutes), where the `fsync` failed closed with 650
fragments proven and 1,325 lost, as designed. An entry past the bound is not
offered and the fragment stays `L`: a loss of availability, not of safety. A
larger ring would only move that point; it was not raised. The ring costs 112
bytes in the record and at most 168 bytes per root in a COMMIT reply, only for
a member with unclean predecessors.

**No compatibility.** The wire and record versions are new, old records and
old replies are refused (a PUT reply of 10 bytes, a COMMIT reply without the
history fields); a freshly formatted cluster is assumed.

### Review round (coordinator's independent review, head `252cdebd5`)

The review found no path to a wrong `fsync` 0 under the stated assumptions and
three places weaker than fail closed. Each is fixed on the branch:

1. **A failed barrier stayed failed only for COMMIT, not for the record.**
   `put_sync_barrier_locked` kept running barriers after one failed and a later
   success wrote a higher seq and raised `durable_seq`. Fixed as in proof step 4
   (`tainted`): after a failure in the boot, a successful retry still drains the
   accounting and resets the back-off but names nothing. The existing back-off
   test now asserts the record and `durable_seq` stay at 0, and
   `test_failed_barrier_not_named` shows a good barrier, a failed one, a
   successful retry, then a restart whose ring holds the good barrier and
   nothing later. With the guard removed the back-off case fails.
2. **A record could pair `ring_ok=1` with a ring recovery had just voided.**
   `put_sync_record_put` loaded `ring_n` and `ring_ok`, `server_put_sync_recovered`
   stored them under `mu` while the barrier thread wrote records without it.
   Now every store of the pair (`put_sync_void`, the verdict) and the writer's
   loads happen under `rec_mu`, so the writer sees one of the states
   (entries, unvalidated), (none, unvalidated), (none, validated) and never
   (entries, validated). This is an argument, not a deterministic test: the
   interleaving cannot be forced without a hook inside the writer, so
   `test_record_ring_pair` is a stress check (a writer thread rewriting the
   record while the ring is re-armed, voided and validated 100 times; no record
   may say validated with entries). It also passed with the lock removed from
   the stores in three trial runs, so it does not demonstrate the defect.
3. **"Recovery dropped no covered file" was narrower in the code than here.**
   (a) A published fragment that is missing was marked for rebuild
   (`refs_missing`) without voiding the history. `rv_mark` now forgets every
   root's ring, durably, before it writes a new mark (a crash right after the
   mark cannot keep it); a slot already marked when the stripe was published (a
   fragment never acknowledged here) takes the other branch and voids nothing.
   (b) `covered_drops` was read once at the verdict; a drop after the fence
   cleared never voided. A covered drop now voids the ring at once, whenever it
   happens. (c) An unknown root did not count; it now voids every root (as the
   code comment already said). (d) The comments in `recovery.c` that said the
   client's `fsync` fails "because the verifier changed" were rewritten: the
   client decides from the barrier history. Packed segment fragments take no
   part in the client's `fsync` proof and still void nothing.
4. **Small.** The client's reply check (`efs_data_commit_reply_ok`) refuses a
   reply with a history entry whose verifier is zero, whose position has bits
   above the 56 an ack holds, or whose root is not below `EFS_MAX_STORAGE_PATHS`
   (three rows in `test_history_table`, each fails without the check). The
   verdict write that could lower the on-disk seq by one barrier is fixed by
   `PUT_SYNC_KEEP_SEQ` (`test_recovered_keeps_record_seq`, fails with the old
   snapshot).

Voiding is coarse on purpose: a real loss on one root forgets the history of
every root of that node (a lost fragment is rare; the cost is that an `fsync`
of data the node's earlier incarnations acknowledged then fails with `EIO`, as
it did before the fix, and nothing re-sends it).

## Results after the fix

Hosts. All runs on `nuc` (Linux 6.12.0-211.67.1.el10_2, 8 CPUs, i5-1135G7,
gcc 14.3.1, Python 3.12.15, system fuse3, user's other deployment untouched)
were made on the source of `3e9057df5` (`src/` and `include/` equal to this
branch's head), built with `make -j8 efsd efs-fuse efs-mgmt tests/faults/efsd`
(binaries report `build=unknown`; SHA256 in the capture manifest). The earlier
runs on `node9900` (the table's second block) used the first build of this
design, `8082fc2d2`, before the root identity, the version refusal and the
record-sync revert existed.

nuc is far slower for this workload than node9900: a 4 KiB append takes 0.39 s
at the median (the barrier's `syncfs` of a large shared `/home`), so the
8,000-record default runs 13 minutes and the failover loop kills 22 times. The
first such run (22 kills; node restarts 22, 22 and 19 times) failed its final
`fsync` with 650 fragments proven and 1,325 lost: more than 7 restarts between
a write and the `fsync` push the first incarnations out of the ring, which is
the documented bound at work (not kept as a capture). The nuc series therefore
use `--records 2000 --failover-window 45` (the controller stops after 45 s, 4
kills per run, 8 MiB file) with `--settle-before-fsync`; the node9900 runs used
8,000 records and the same kill pace for the whole write.

| Series | Host | Runs | Final `fsync` 0 (all writers) | Post fsyncs 0 | Integrity (three views identical) | Verdict PASS |
| --- | --- | --- | --- | --- | --- | --- |
| Before the fix (diagnosis) | node9900 | 14 | 0 of 14 | 0 of 14 | 14 of 14 | 0 of 14 |
| First build, final `fsync` concurrent with the failover | node9900 | 28 | 18 of 28 | 28 of 28 | 28 of 28 | 17 of 28 |
| First build, `--settle-before-fsync` | node9900 | 28 | 28 of 28 | 28 of 28 | 28 of 28 | 25 of 28 |
| **This build, `--settle-before-fsync`, 2000 records** | nuc | **28** | **28 of 28** | **28 of 28** | **28 of 28** | 19 of 28 |
| This build, control `--fsync-every 100` | nuc | 3 | 3 of 3 | 3 of 3 | 3 of 3 | 2 of 3 |

- nuc: `na-1..14`, `nb-1..14` and `nr-1` (the replacement of `nb-2`, whose mount
  failed at start, 6 s, "cluster not formed": a harness start flake under two
  parallel clusters, no events, no data). Fragments proven by the barrier
  history: 2,298; counted lost: 50, every one marked degraded for rebuild with
  the other two members durable (`fsync` returned 0 with a visible mark, K35);
  verified recovery dropped no file in any run.
- node9900 first build: 21,598 and 23,254 fragments proven, 532 and 703 lost
  (the 703 marked); one 0-byte file dropped in `sb-11`; 10 of the 28
  unsettled runs got a transient `EIO` when the `fsync` landed in a member's
  outage or recovery fence (class A, D42 F22, K36: two of three members
  unavailable; `PMD`, `DPM`, `MDP`; timed against the failover events for `fa-5`
  and `fb-1` only). `--settle-before-fsync` makes the writers wait for the
  controller to stop and every restarted member to log `recovery: verified`
  before the final `fsync`; without the flag the harness behaves as before.
- Control on nuc: `nc-1` failed `availability` because its first periodic
  `fsync` (record 100) returned `EIO` on all four writers inside a kill window
  (the same transient); its final and both post fsyncs returned 0. The control
  passed 3 of 3 on node9900 before the fix, which does not distinguish the
  fix; it shows periodic fsyncs avoid the loss without the barrier proof.
- Whether COMMIT should wait out the recovery fence instead of answering `EIO`
  is a product decision, not made here; K36 chose the `EIO` on purpose.

### Other kill-gate failures (not K110)

Nine of the 28 nuc runs and 5 of the 56 node9900 runs fail a REPORT criterion
of the same harness: a type-67 REPORT waited the 30 s receive bound
(`report_recv_timeouts`) or took more than 3 s per leader change (4.8 to 7.6 s).
The pre-fix series has the same failures in 3 of 14 runs, so the change did not
introduce them; they belong to the leader-failover latency family tracked as
K104 (Task FF). Data and `fsync` are exact in all of them (integrity PASS, post
fsyncs 0). They keep the kill gate from passing every run and need their own
fix.

### Task EY's probe (tcp-h01-k110): the D/M/L stripes and the later fsync

Task EY (branch `claude/kind-darwin-bbba2f`, commit `b35ecd129`, page
`batch-d-distinct-host-20261109.md`, "K110 capture", not merged here) ran H1
`sigkill-8k` with `--post-fsync` on distinct hosts: the final `fsync` of all
four writers returned `EIO` 6.5 to 6.9 s after the last member was back, two
more `fsync` calls returned `EIO` too, and the same 227 stripes stayed
unproven: three fragments D, M and L (chunk 178 on nodes 3, 1, 2: node 3 D,
node 1 M "never acknowledged, no degraded mark", node 2 L acknowledged before
the kill), nothing re-sent, `stripe_close_for_absence()` retired none in 24.7 s.
The questions, answered with this branch:

**(a) Does the fix change that outcome?** Yes for the L, and the M needs no
change. The L is a fragment of a killed incarnation; the fix proves it durable
when it was acknowledged at or before that incarnation's last completed barrier
(the history entry named by the node's COMMIT reply, condition above). In
Task EY's stripe the L was written during node 1's outage and node 2 was killed
at least 7 s later, so a barrier (every 200 ms or 256 MiB) covered it: the
stripe is then D, D, M, two durable plus a published-degraded fragment, and the
`fsync` returns 0. The M is not an open defect: a fragment that was never
acknowledged is, by design, a slot the publication marks degraded (the W154
mark rides on the publication that wrote the stripe, K35), so a stripe
D, D, M passes without a new mark; the fate table in `test_history_table`
covers it (U with two D: `OK`, no mark added). What Task EY read as "no
degraded mark" is the log line of the fold, which only counts marks it had to
add. The pattern is in `tests/test_k110_barrier_history.py` (above): fails
before, passes after.

**(b) Contract for a later `fsync` once every member is back.** It returns 0
whenever every unproven fragment on the stripe is either durable on its own
incarnation or covered by a barrier recorded by the node that now holds it, and
the stripe has two such fragments or one plus a mark. With the fix that holds
for Task EY's stripes (the two later `fsync` calls in the regression test, and
the two post `fsync` calls of the runs below, return 0). It still returns
`EIO`, permanently and without re-PUT, for one case: an L acknowledged after
its incarnation's last barrier (the 200 ms window, or 256 MiB), on a stripe
that also has an M, so only one fragment is durable (D, M, L-uncovered, "1 of 3
durable; need 2"). The client keeps no copy and the PUT is not re-sent, so no
later event changes it. Closing it needs either option 4 (a rebuilt M slot
proven by a cleared mark, the chunk reply exposing the mark state) or the
client re-sending or repairing the stripe; both are a task of their own, and no
wait is widened for it. That residual stripe was not seen in any run listed
here (nor in Task EY's capture, whose L was covered).

**(c) Task EY's command on the fixed tree.** The H1 `sigkill-8k` arguments of
`scripts/evidence/k110/run_series.sh` plus `--post-fsync` (8,000 records, four
writers, interval 4, first event 3, down 5), with `--failover-window 62` because
nuc takes 0.39 s per append, no settle flag, two runs in parallel on nuc from
the source of `d4a6876d2` (`~/efs-artifacts/taskFH-20261110/series-ey-probe/`):

| Run | Verdict | Final `fsync` (4 writers) | Post `fsync` | Unproven stripes at any `fsync` | Proven / lost (marked) | Kills | Integrity |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `eya-1` | FAIL, only `report_within_latency_bound` | 0, 0, 0, 0 (max 0.69 s) | 0, 0 | 0 | 91 / 4 (4) | 5 | PASS |
| `eyb-1` | PASS | 0, 0, 0, 0 | 0, 0 | 0 | 133 / 3 (3) | 5 | PASS |

The client logs have no `need 2` or "without proof" line in either run, so the
count of unproven stripes is 0 where Task EY's capture had 227 (one `fsync` of
the four writers takes 0.19 to 0.85 s here). Each run took about 1,580 s. These
runs are on one host, loopback, not on Task EY's distinct hosts, so they show
that the fix removes the D/M/L stripe on this tree; the distinct-host H1 rerun
is still owed. The `eya-1` failure is the REPORT latency criterion of the K104
family above, with data and `fsync` exact.

### Tests

- `tests/test_k110_barrier_history.py` (new, `make test-long`; budget 10 s):
  three members and one mount, a 3 MiB file written without `fsync`, every
  node's barrier covers it, node 3 then node 2 SIGKILLed and restarted, then
  `fsync` must return 0 with the client's line `restarted members: N fragments
  proven by their barrier history, 0 lost` (N at least 2) and no degraded mark.
  **Fails on the pre-fix tree** (`4a24916a1`, built on node9900): `OSError:
  [Errno 5] Input/output error` at the `fsync`. **Passes on the fix** (5.5 to
  7.7 s on node9900, 6 to 6.4 s on nuc). A second part reproduces Task EY's
  stripes (see below): file X is written while node 3 is down, node 2 is
  SIGKILLed after the barrier, and two `fsync` calls of X must return 0 with at
  least one fragment proven, none lost and no `need 2`. On the pre-fix tree a
  DML-only variant fails with `Errno 5` and the client logs `fragments=MDL` and
  `LMD`, "1 of 3 durable; need 2", the stripes Task EY saw. On the fix the whole
  test passes (7.7 s and 8.7 s on nuc).
- `tests/test_data_commit.c`, fast suite (4.6 s on nuc, budget 30 s): the
  fate table of the client fold, `test_history_table`: each fragment of a stripe
  is F (acknowledged by the answering incarnation), C (replaced, covered by the
  history), N (replaced, acknowledged after its last barrier) or U (never
  acknowledged); all 54 combinations the table accepts (a record needs two
  acknowledged fragments), run **before** (no history in the replies, C behaves
  as N) and **after**, assert the result (`OK` iff two are durable, one mark
  exactly when the third is N), the marks and that failed stripes are retired
  cleanly; plus a truncated and a padded COMMIT reply (the covered fragment on
  that node stays unproven and is marked).
- `tests/test_store_durability.c`, fast suite (1.9 s on nuc, budget 5 s): the
  record (write, reread, ring across unclean boots, validation by verified
  recovery, forgetting after a covered drop, a boot killed before recovery,
  clean stop keeping the ring, the drop rule, ack positions) and
  `test_barrier_history_fail_closed`: wiped store (no record, no ring), layout
  change (root count or index), CRC failure on two fields, `EFSDUR02` and
  `EFSDUR04` magic, short and long file, a record refused at attach (startup
  refused), the 7-entry bound with the 8th to 10th oldest not offered, a root
  in error or still fenced offering nothing, an open-window ack above the
  durable seq, boot above 65535 giving no ack, and the record never naming a
  barrier whose sync failed.
- Arity and format updates in `test_put_retry_budget.py`,
  `test_put_reply_deadline.py`, `test_put_tcp_frame.py` and the live drivers.
- `tests/test_k36_recovery.py` (27 s) gained one assertion in step 3: after the
  crash at the first mark, the log has `barrier history forgotten ... a
  published fragment is missing without a mark` before the crash line (the
  deleted fragment of a published stripe; the truncated file is not "covered"
  because the truncation refreshed its mtime). `tests/test_k35_fsync_degraded.py`
  (128 s on nuc, 129.6 s on node9900) is unchanged and passes.
- Review round: `test_store_durability` gained the drop rule after the fence,
  the unknown-root drop, `server_put_sync_void_all`, `test_failed_barrier_not_named`,
  `test_recovered_keeps_record_seq` and `test_record_ring_pair` (stress, see the
  review round); the back-off case asserts the record and `durable_seq` stay
  put after a later success; `test_data_commit`'s malformed-reply loop has three
  more rows. Mutations checked on nuc: with `tainted` forced to 0 the back-off
  case fails; with the verdict write using `durable_seq` again
  `test_recovered_keeps_record_seq` fails; with the entry check removed from
  `efs_data_commit_reply_ok` the first new row fails; with `rec_mu` removed from
  the stores, `test_record_ring_pair` still passed (no demonstration).

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

## Text to apply at merge (the coordinator applies it; none of it is edited on this branch)

**D42 F23** (`docs/status/decisions.md`): keep the F23 row and add after the
table, as in the Oct 24 text for other amendments:

````text
**F23 amended Nov 10 (K110, user: "Build the fix").** F23 said an earlier
ambiguous write is not thereby proven. That stays true for a write the
restarted member cannot show was made durable, and no longer for one it can: a
fragment acknowledged by incarnation V at a position at or before V's last
*completed* durability barrier (`syncfs` of the root, then the fdatasynced
durability record) is durable once the successor's verified recovery (K36)
dropped no file that barrier could cover and found no published fragment
missing without a mark; a barrier that failed in the incarnation ends its
history at the last good barrier. The successor reports its
predecessors' last barriers in its COMMIT reply (the 7 newest per root, newest
first) and the client counts such a fragment durable instead of lost. A
fragment acknowledged after that barrier, one on a root whose history
recovery could not validate or whose layout (root count or place in the list)
changed, one from a wiped or replaced store, one older than the 7 newest
predecessors, and any reply without or with a malformed history stay lost, and
`fsync` stays `EIO` when the stripe needs them. Scope: the same process-kill
and OS-crash assumptions as the asynchronous acknowledgement; full power-loss
qualification stays a separate scope decision. Design, proof and evidence:
[K110](2026-11/k110-fsync-after-sigkill-20261110.md).
````

**K36 page** (`docs/status/2026-10/k36-verified-recovery-20261024.md`, lines
43 to 48): replace "`EIO` when it needs that fragment, and never counts it
durable. Test step 4 checks both." with:

````text
`EIO` when it needs that fragment. *Amended Nov 10 (K110, D42 F23): a fragment
acknowledged at or before the crashed run's last completed barrier is proven by
the barrier history the recovered member reports in its COMMIT reply; only the
rest stays lost.* Test step 4 checks the lost remainder.
````

**K35 page** (`docs/status/2026-10/k35-fsync-degraded-20261024.md`): after "never
forgotten)" in step 2 add " unless the node's COMMIT reply carries a barrier
history that covers the fragment's root and position (K110, D42 F23 as amended
Nov 10)" before "A refusal"; and in the unclean-stop bullet replace "fails EIO
for good. Not exercised by a daemon test here." with "fails EIO for good, except
where the restarted member's barrier history covers the fragment (K110, D42 F23
as amended Nov 10). Not exercised by a daemon test here."

**K110 rows** (the K tables are not edited here).

Register row, `Item | Kind | Disposition`:

````text
Task EY (batch D H1, candidate `9bb50fe79`, Nov 10), diagnosed by Task FH ([evidence](2026-11/k110-fsync-after-sigkill-20261110.md)): `tests/live/append_failover.py --failover kill` failed its final `fsync` with `EIO` in 14 of 14 private runs, permanently, with every byte reading back exact. Cause: a fragment acknowledged by an incarnation SIGKILLed before any COMMIT was counted lost forever (K35/K36 rule) although that incarnation's own durability barrier had covered it | durability proof / fsync availability across member restarts | FIXED on branch `claude/determined-mclean-7c3b6f` (Task FH, user: "Build the fix", D42 F23 amended; not merged): the PUT reply carries the acknowledgement's barrier position, the durability record v3 keeps a ring of the 7 newest predecessors' last completed barriers (offered only after a verified recovery that dropped no covered file, only for the same root layout), the COMMIT reply returns it and the client counts a covered fragment durable; everything else fails closed. Regression `tests/test_k110_barrier_history.py` fails before and passes after; unit tables in `test_data_commit` and `test_store_durability`. Live distinct-host H1 kill rerun owed (TCP and RDMA); VERIFIED on the merge gates. `blocks alpha: no`
````

Status row:

````text
FIXED on branch `claude/determined-mclean-7c3b6f` (Nov 10, Task FH, **blocks alpha: no**): barrier-proof `fsync` after sequential SIGKILLs; settled kill-mode runs pass the final `fsync` and integrity (table in the evidence page); an `fsync` taken inside a member's outage or recovery fence is `EIO` by design; live distinct-host H1 kill rerun owed, VERIFIED on the merge gates ([evidence](2026-11/k110-fsync-after-sigkill-20261110.md)).
````

## Reproduction

Prerequisites: a Linux host with fuse3 (a private prefix via `EFS_FUSE_PREFIX`
if the system one is not usable), a tree built with
`make efsd efs-fuse efs-mgmt tests/faults/efsd`
(the last for the unit and live tests), free ports 38101 to 38103, Python 3.
From the tree root (`WORK` defaults to `/tmp/efs-k110`):

```bash
WORK=/tmp/efs-k110 scripts/evidence/k110/run_series.sh 1 14 sa "$PWD" --settle-before-fsync
```

On a slow host (nuc: 0.39 s per append) bound the run:
`run_series.sh 1 14 na "$PWD" --settle-before-fsync --failover-window 45 --records 2000`.

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

On nuc (8 CPUs, `git archive` of `b65a8365b`, no other work of mine running;
nuc's own services idle):

- `make -j8 test`: rc 0 (220 s, fast units).
- `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` (the `make test-all`
  gate): `TOTAL 597 FAIL 2 NOTRUN 12 STATUS FAIL (1321s) OVERBUDGET 55`. The
  two failures are load-induced, not K110: `test_d4_progress.py` (one
  `write+fsync 1 MiB` took 10.11 s against the 10 s bound at load 23 on 8 CPUs)
  and `test_pump_logging.py` (34 writes against 50 at 0.9 writes/s, the nuc
  barrier latency above). Three solo runs of each passed (115, 82 and 84 s; 90,
  87 and 90 s). The NOTRUN are host limits (no RDMA device, no valgrind, no
  cluster scripts). OVERBUDGET lists load-induced overruns on this slower host;
  no budget was raised.
- `bash tests/sanitizer_check.sh tsan`: `FAIL (32 run, 1 failed, 802s)`:
  `k96_status_slots_live` took 258 s under load with a 35 s REPORT; three solo
  TSan runs of it passed (22 to 25 s). Everything else passed, including
  `k110_barrier_history`. On node9900 the lane passed 32 of 32 earlier (628 s).
- `bash tests/sanitizer_check.sh asan`: `VERDICT: PASS (60 run, 0 failed,
  438s)`, including `k110_barrier_history`.
- `make docs-check` and `python3 scripts/check_doc_links.py` pass.

The node9900 gate of the first build (`TOTAL 595 FAIL 0`, 620 s, TSan 32 of 32,
ASan 60 of 60) stands for that build only.

**Review round** (nuc, `git archive` of `1774dde0b`, tests edited after it and
re-run; the `make test-all` gate and the 28-run series were *not* repeated):

- `make test`: rc 0 (145 s) on the final tree. Before the last two test edits
  (the k36 assertion and a smaller ring stress) it was rc 0 (208 s).
- `test_store_durability` 2.2 to 3.3 s (budget 5 s), `test_data_commit` 4.6 s
  (budget 30 s), three solo runs each, all pass.
- `tests/test_k110_barrier_history.py` 8 to 9 s, `tests/test_k36_recovery.py`
  27 s (with the new step 3 assertion; its first run failed on an assertion
  that wrongly expected the truncated file to count as covered, see the test
  comment), `tests/test_k35_fsync_degraded.py` 130.6 s: all pass.
- `bash tests/sanitizer_check.sh asan`: PASS (60 run, 0 failed, 394 s).
  `bash tests/sanitizer_check.sh tsan`: PASS (32 run, 0 failed, 570 s), the new
  `test_store_durability` threads included.
- The proven path did change (record seq handling, void on drop, reply check),
  so a short kill series was repeated: 8 settled runs of `run_series.sh`
  (`--settle-before-fsync --failover-window 45 --records 2000`, two series in
  parallel plus one replacement), tags `ra-1..4`, `rb-1..4`, `rr-1`. Final
  `fsync` 0 and the three views identical in 8 of 8; 7 PASS, `rb-4` failed only
  the REPORT latency criterion (6.7 s, K104 family). `ra-3` died at setup (`efs-fuse mnt
  exited` after 7 s, the same harness start flake as `nb-2`) and was replaced
  by `rr-1`. Last `fsync` of each run: 667 fragments proven by their barrier
  history, 18 lost, each marked degraded; no `need 2` line; no run logged
  `barrier history forgotten` (no covered file was dropped or lost).

## Not checked

- RDMA (the PUT and COMMIT reply changes are in the RDMA path and the buffered
  path too, built and exercised only through TCP), distinct hosts and the
  combined faults of Task EY's `combined-2k`; one host, loopback only. The live
  distinct-host H1 kill rerun is owed.
- `tests/live/rdma_async_commit.py` needs an RDMA device and was not run. It
  asserts `EIO` for a node killed right after a write; a write older than the
  200 ms barrier window now fsyncs 0 after the kill, so its expectation (the
  magic `EFSDUR03` is already updated) must be revisited by whoever runs it.
- Power loss (K23/W71 stay deferred): the proof is the same kind as a normal
  COMMIT, `syncfs` plus an fdatasynced record, and says nothing beyond it.
- The wall-clock assumption of the validity rule (a restart does not step the
  clock back past file modification times) is not checked at run time.
- A fragment file removed by hand under a retained record is detected by the
  history only at the next recovery, and only when a published object places a
  fragment there (`rv_mark`); an unpublished file removed that way, and packed
  segment fragments, void nothing. A whole wiped store has no record.
- Voiding rewrites the record best effort (a failed write leaves the entries on
  disk until the next barrier writes the in-memory state); `test_record_ring_pair`
  is a stress check that does not demonstrate the race it guards (argument in
  the review round).
- No log line states whether a ring was kept or forgotten at a recovery.
- The failed-record-sync revert is a best effort (a second failure can leave
  the file naming the failed barrier); it has no fault-injection test beyond the
  record round trip.
- The ring bound (7) was exceeded once on nuc (22 restarts of a member), where
  the `fsync` fails closed as designed; the capture was not kept.
- Task EY's D/M/L stripes were reproduced and removed on one host only; the
  residual "uncovered L plus M" stripe (answer (b)) has no test of its own on a
  live cluster, only the fate table (`N` with `U`: `EIO`).
- The REPORT failures of the kill gate (K104 family) were not investigated; the
  unsettled `fsync` during an outage or recovery fence stays `EIO` by design.
- nuc's per-write latency (0.39 s) makes its timings unlike node9900's; its
  load-induced gate failures were attributed by solo runs, not by changing a
  bound.

## Raw captures

Outside Git on nuc: `~/efs-artifacts/taskFH-20261110/` (user `maik`; per series
directory `series-settled-na`, `-nb`, `series-replacement`, `series-control`:
each run's `*.out`, `*.report.json`, `*.logs/` with `fuse-mnt.log` and
`efsd-n*.log`; `tests/` with the test outputs and the driver scripts `run1.sh`
to `run4.sh` and logs; `gate/` with `summary.txt`, `timings.tsv`, the two
failure logs; `sanitizers/`; `solo/` with the nine solo reruns;
`host-and-binaries.txt`; `series-ey-probe/` with `eya-1` and `eyb-1`; `dml/` with
the pre-fix DML-only run) with `SHA256SUMS` and, for the review round, `review-round/` (`series-settled/`
with the 9 kill runs, `lanes/` with the test and sanitizer logs and the driver
scripts `run6.sh` to `run8.sh`, `binaries.txt`); `SHA256SUMS` (437 files;
SHA256 of the manifest
`0a7541c6cc360311d91a74c420cd32f321aace5e5b22d9d0dd02d44b9214c4d1`; the
Task EY runs used a second build of the same source, whose `efsd`, `efs-fuse`
and `efs-mgmt` hashes are at the end of `host-and-binaries.txt`, and the review
round a third, in `review-round/binaries.txt`). nuc
binaries of the tested tree: `efsd`
`908c9a2f37dc737da7a7a740c52c293ad639c674a0901f589260610b38e653e7`, `efs-fuse`
`ec07217ccce8b155a24d8316a19cf9dd3f6e41a4a4205e3e5e96baff8e639a02`, `efs-mgmt`
`8a93642f30b54c09918d9e1ed409dd57be48c1eaf1cb6f2f1faab4f9b4d149d1`.

On node9900 (first build and diagnosis, runs made before the host was moved):
`~erbmi1/efs-artifacts/taskFH-barrier-20261110/` (manifest SHA256
`887d7d88cb5c447e26afdc8adecd29d0a7dabd67d8a746aa84f7dc11e6ed009d`, 532 files;
`efsd` `f16934e90c697b29249a18685c071a13ce889800d2e3b8c173b16a530c9ecdc5`) and
`~erbmi1/efs-artifacts/taskFH-20261110/` (the diagnosis; manifest
`c68cfb866781800af0fc64ac17646bd16527a62450c099059cf16b7217317020`). All run
stores and work trees (`/tmp/efs-taskFH*` on node9900, `~/efs-taskFH` on nuc)
were deleted after the logs were copied; the commands above recreate them.
