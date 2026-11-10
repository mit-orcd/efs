# K106: the waits one halted member causes (Task FL)

**Status (Nov 10):** on branch `claude/stoic-feynman-fae89a` (devel `cff8eb424`), not merged. K106 stays OPEN in both
tables of [the checklist](../alpha-1.0-2plus1-checklist.md); the coordinator rules at the merge.

Task FL, Nov 10 2026, intel1 (private clusters, ports 18700 to 18799, `~/efs-taskFL/`). Purpose: remove the three waits of
row K106 (one member halted with SIGSTOP: its kernel accepts TCP, nothing replies), one at a time, each measured before and
after. Linked row: K106 (and the Task EY check in its text: ten `close()` calls returning `EIO` with one member stopped).

**Verdict: (b) FIXED and (c) FIXED, each with a regression test that fails on devel's source and a measurement before and
after; (a) NOT FIXED: no safe structural fix, analysis and one concrete proposal below for the coordinator's decision.**

| wait | measurement (intel1, three efsd + one efs-fuse, node 3 halted) | devel `cff8eb424` | fixed `522b2bc40` |
|---|---|---|---|
| (a) request in flight to the halted leader | 3 `mkdir` right after SIGSTOP, node 3 leads both groups; first `mkdir` | 30.12, 30.20, 30.45, 30.22 s (4 runs); no `EBUSY` | 30.15, 30.45 s (2 runs; 3 more on earlier builds 30.15 to 30.55); **unchanged** |
| (b) PUTs placed on the halted member | 30 sequential 256 KiB writes after the halt | 239.3 s total: 7.04 s, then 8.0 s with `EIO` for 29 of 30 writes | 13.1 s total: 7.04 s, 5.03 s, then 0.02 to 0.04 s, 0 errors |
| (c) GC recovery of a stranded transaction | seconds from the `DECIDE_LOST` fault to `txn-recover ... ABORT` (finisher disabled, see below) | 32.7, 32.0, 11.0 s (3 runs; longest unlink 30.0 s with `EBUSY` twice, 11.1 s once) | 5.2, 6.9, 5.1, 6.4 s (4 runs; longest unlink 0.09 to 6.7 s, no error) |

Controls (node 3 not halted): (b) 0.8 s for 30 writes on devel, 1.1 s fixed; (a) three `mkdir` of 0.00 to 0.01 s; (c) 6.7 s on
devel, 7.1 s fixed (the 5 s record age plus the scan cadence): with the fix the halted case equals the control. After SIGCONT (mode `r`) no write leaves a fragment for node 3 unacknowledged: the mark ends
within the first write after the resume (devel: same).

## Source, builds and environment

- Devel `cff8eb424` (before) and branch commits `193211b65` and `522b2bc40` (after: the second only moves
  `host_recover_thread` below `host_gc_thread` so `tests/test_put_drain.py`'s source slice is unchanged, and adds mode `r` to
  the script). All after-rows in the table are from `522b2bc40`, built from `git archive`, `make -j8 all tests/faults/efsd`,
  gcc 14.3.1 `-O3`, TCP; rows from earlier builds are labelled. Binary identity (SHA-256 prefix): devel `efsd` 05291b12,
  `efs-fuse` b98ed939, `tests/faults/efsd` 66488313; fixed `efsd` 845818f6, `efs-fuse` cbca3817, `tests/faults/efsd` f86f2861.
- intel1: 8 CPUs, 15 GB, Rocky/EL10 kernel 6.12.0-211.67.1, fuse3 3.16.2. Three `efsd` of the `EFS_FAULTS` build (quota 1 GiB,
  tcp), one `efs-fuse` through node 1; ports 18700 to 18799 only (`scripts/evidence/ff/port_range.py`, 12 per slot); up to
  seven runs in parallel on the 8 CPUs.
- Method (c): the finisher thread (`host_fin_thread`) resolves a deposed coordinator's transaction in about 1 s and hides the
  wait, so devel and fixed trees for (c) carry one extra edit, `sed -i 's/h->fin_running = h->fin != NULL;/h->fin_running = 0;/' src/server/raft_host.c`
  before the build (devel `efsd` f41bd418, fixed `efsd` 16c3b2a4). That is the case the K106 text names: recovery is the only
  path left (the finisher drops an item after `HOST_FIN_MAX_NS`, 10 s, or a restarted coordinator has none).

## Reproduction

```bash
git archive <commit> | ssh intel1 'mkdir -p ~/efs-taskFL/src && tar -x -C ~/efs-taskFL/src'
cd ~/efs-taskFL/src && make -j8 all tests/faults/efsd         # (c) tree: apply the sed above first
export TMPDIR=~/efs-taskFL/tmp
# one command per row, SLOT 0 to 7 picks the 12-port range (run long ones in a screen):
python3 scripts/evidence/ff/port_range.py "$PWD" SLOT scripts/evidence/fl/halted_waits.py "$PWD" b halt      # (b) ~4 min on devel
python3 scripts/evidence/ff/port_range.py "$PWD" SLOT scripts/evidence/fl/halted_waits.py "$PWD" a halt      # (a) ~50 s
python3 scripts/evidence/ff/port_range.py "$PWD" SLOT scripts/evidence/fl/halted_waits.py "$PWD" c halt 60   # (c) finisher-off tree
python3 scripts/evidence/ff/port_range.py "$PWD" SLOT scripts/evidence/fl/halted_waits.py "$PWD" r halt      # resume
# controls: replace halt by nohalt.  Last line: ROW {json}.  Cleanup: stop by pid, rm -rf the tree and $TMPDIR/efs-fl-*.
```

`b`: both groups led by node 1, 8 warm writes, SIGSTOP node 3, 30 writes timed. `a`: both groups led by node 3 (the client
has noted it), 5 warm `mkdir`, SIGSTOP node 3, 3 timed `mkdir`. `c`: directory spread at 64, churn create/unlink, SIGSTOP
node 3, `EFS_FAULT_DECIDE_LOST`, listings for 60 s, recovery time from the daemon logs. `r`: as `b`, SIGCONT after 6 writes,
8 s of writes, time of the last degraded acknowledgement against the SIGCONT.

## (b) Every PUT on the halted member paid the declaration wait

Cause, in the order the measurements showed it. The K106 text named the bare-TCP reprobe; that was part of it, not the whole.
Intermediate figures below come from uncommitted overlay builds, the final ones from `522b2bc40`.

1. Devel: 239.3 s for 30 writes, and the writes **failed**: 29 of 30 `close()` returned `EIO` after 8.0 s, each a client
   `dcache-flush-fail stage=put-window rc=-13` (another flusher's PUT window still open after `put_win_wait(ino, 8000)`).
   This is Task EY's batch D H15 symptom (ten `EIO`, seven at `stage=put-window`) reproduced on one machine: the
   coordinator's reading (a PUT paying wait (b) outlasts the 8 s window wait) holds.
2. Real exchange in the reprobe alone (`node_probe_run` and `efs_client_node_reprobe` connect, wrap and `efs_conn_declare`
   instead of a bare connect): **no change** (239.3 s, same 29 `EIO`). The down mark was never set at all: a checkout whose
   declaration waits out the 5 s local bound fails at `now == admission_end`, and `efs_client_conn_get` treats that expiry as
   "cancellation, not a peer-health verdict" (`now < admission_end`), so the failure streak never reached
   `EFS_NODE_DOWN_FAILS`. Every PUT paid 5 s again, forever.
3. Plus: an unanswered declaration at the **local** bound (not the caller's deadline) counts as the peer's failure
   (`silent`): 155.9 s, no `EIO`, 5.03 s per write. The 4 failures mark the member down, but `efs_client_node_reprobe`, called
   once per PUT from `put_fragments_parallel_run` (`write.c:4022`), now waited the whole 5 s on the mute member.
4. Plus: the caller's reprobe does no I/O when the member was last seen mute (`g_client.node_mute`: TCP accepted, declaration
   unanswered, cleared by an answer or a checkout that succeeds); it starts the background probe and answers "still down".
   A member that refuses or answers (a restart inside the cooldown) is still probed by the caller itself, in milliseconds, as
   before. **13.1 s for 30 writes** (the first two writes pay the discovery: 7.04 s and 5.03 s, four failed checkouts).

Changes, all in `src/client/node_cache.c` (plus two fields in `client_internal.h`):

- `node_exchange_ok`: connect, wrap and `efs_conn_declare`, bounded by the caller's deadline and `EFS_NODE_ADMISSION_MS`
  (5000, the checkout's own literal, now a name); the probe thread registers its descriptor.
- `node_probe_run` clears the mark only when the declaration is answered; a failed probe sets the mark to
  `now + EFS_NODE_DOWN_MS` (the existing 30 s) so it no longer expires under a mute member and re-opens the 4-failure
  discovery every 30 s.
- `efs_client_conn_get`: an unanswered declaration at the local bound is a peer failure and marks the member mute.
- **Lifetime bug** (Task FD's TSan run, `node_cache.c:624` and `:632` against `efs_client_shutdown` at `:949`): the detached
  probe took `conn_lock` after shutdown destroyed it. `node_probes_stop` (called first in the pool teardown) sets
  `node_probe_stop` (atomic), shuts down the running probe's descriptor, and waits on `conn_cv` until `node_probe_inflight`
  is zero for every node; `node_probe_maybe_start` starts nothing afterwards, and a cancelled probe is no verdict.

Callers checked: `efs_client_node_reprobe` (`write.c` 3794 spare probe and 4023 resend, `read.c` 466 and 528,
`data_commit.c:323`), `efs_client_node_is_down` (about twenty callers, unchanged), `node_probe_maybe_start` (two). Test
`tests/test_node_probe_exchange.py` (Makefile and `tests/budgets.tsv` rows) runs the production `node_exchange_ok`,
`efs_client_node_reprobe` and `node_probe_run` with a peer that accepts and never answers: the mark is held and re-armed, a
caller never waits on a mute peer, a refusing peer is probed by the caller, an answer ends the mark, and a probe blocked in
its declaration is cut short and waited for before the locks are destroyed (TSan clean on intel1). Against devel's
`node_cache.c` it fails at its first assertion. `tests/test_conn_checkout_budget.py` gained the local-bound case and
`tests/test_node_reprobe.py` follows the new function names (its "peer still down" assertion now expects the re-armed mark,
an intended change).

## (c) Recovery queued behind the reclaim acceptance

`host_gc_thread` ended every pass with `host_txn_recover_pass`, after `host_gc_reap_pass` whose step 2
(`host_gc_inode_nodes_batch`, "Storage acceptance by every member") waits for each member's reply (`reap_fence` 10 to 30 s
in the daemon's `gc-pass` lines with node 3 halted, 0 to 40 ms otherwise). Recovery of a transaction older than
`HOST_REC_AGE_NS` (5 s) therefore ran at most once per such pass: 32.7 s after the fault (`txn-recover ... age=32.7s`); the
third devel run, 11.0 s, fell between two fences.

Fix (`src/server/raft_host.c`): `host_recover_thread` (defined after `host_gc_thread`) calls `host_txn_recover_pass` every `GC_LOOP_MS` (the cadence the GC
loop kept for it), started beside the GC thread, woken and joined at stop; the GC pass no longer calls it (its `recover=`
field is gone from the `gc-pass` log line; no script reads it). The recovery state (`rec_*`, `dec_scan`, cursors) is touched
only by this thread now, as before only by the GC thread; the proposals it makes go through `host_bg_propose`, which the
finisher thread already uses concurrently. The reclaim fence itself still waits 10 to 30 s with the member halted (the
`reap_fence` column is unchanged): REAP completion for those inodes is withheld, as it must be, only recovery no longer
waits behind it. Test `tests/test_txn_recover_thread.py` asserts the GC thread source has no recovery call and runs the
production `host_recover_thread` (one pass per `GC_LOOP_MS`, a stop that does not wait out the cadence); on devel it fails
on its first assertion.

## (a) A request already in flight to the halted member: analysis, not fixed

Measured: the first `mkdir` after SIGSTOP of the leader waits 30.1 to 30.6 s in every run (9 runs, devel and fixed), then
succeeds on the retry; `EBUSY` did not occur in these runs (it needs the retry to find the budget spent, as in Task FF's
gate runs under load). The thread sits in `efs_recv_all` (`poll` with the remaining operation deadline) on the noted
leader's connection; nothing else in the client can wake it. The only inputs to the down mark are failures that have
finished (`efs_client_node_note_fail`, failed checkouts), and the one waiting request produces its failure at the same
moment its 30 s budget is gone; the servers mark the member down after about 12 s (`heartbeat: ... marked down`) but the
client has no channel for that.

Every fix needs a client-side failure detector with a threshold, because "halted" is only ever learned from silence. The
smallest one I see, and the one I propose:

- A request that has waited `EFS_NODE_PROBE_MS` (1 s, existing) on a node's connection runs `node_exchange_ok` against that
  node (5 s bound, existing, on the waiter's own thread: it is waiting anyway). Answered: keep waiting (a slow request on a
  live member). Not answered: mark the member down (streak to `EFS_NODE_DOWN_FAILS`, mute) and `shutdown()` the
  connections it has checked out, so the waiters fail with a transport error and take the existing retry path
  (`rpc_forget_leader`, next peer).
- Expected wait: about 1 s + 5 s + the election, 6 to 8 s instead of 30 s. Not zero, and no timeout, budget or bound is raised.
- Where: `efs_client_conn_get` stamps the checkout time; the hook sits where the client's RPC receive loops block
  (`efs_conn_recv_msg` callers, or a slice of the `poll` in `efs_recv_all`, which is shared with the servers).
- Risk, the reason this is the coordinator's call: a live member that does not answer a declaration within 5 s (a long
  stall under load) would have its requests cut and retried; mutations are replay-safe by operation id, reads and PUTs are
  retried, but the retry load lands during the stall. Needs a loaded-cluster run (the wave 70 gate load) before it ships.

## Verification (intel1, tree `522b2bc40`)

All on intel1 against the final tree (`git archive` of `522b2bc40`, plus the two test-only fixes of `baf054b37` and
`6b4941822` copied in; the source under test is identical to `522b2bc40`), raw output in the artifacts directory below.

| check | result |
|---|---|
| `make test` | rc 0. The first two runs failed in `tests/test_resolver_deadline.py`, a test of mine that splices `efs_client_conn_get` and still shortened the old literal `+ 5000`; fixed in `baf054b37` and `6b4941822`, then all 27 tests that splice `node_cache.c` or `efs_client_conn_get` pass on intel1 |
| `tests_for.py client/node_cache --run`, `client/read --run`, `server/raft_host.txn --run` | rc 0 each |
| live `tests/test_k35_fsync_degraded.py` (twice) and `tests/test_k34_stop_put.py` | PASS: K35 cases 1 to 9 incl. 7 and 7b (126.5 s), K34 PASS |
| `bash tests/sanitizer_check.sh asan` | VERDICT PASS (59 run, 0 failed) |
| `bash tests/sanitizer_check.sh tsan` | VERDICT FAIL (31 run, 1 failed): `k37_segment_rebuild`, `AssertionError: refused files once node 3 is back: packer moved 0 of 4`. **Not caused by this change:** the same assertion fails on devel `cff8eb424` in 3 of 9 TSan runs of the entry alone (fixed tree: 3 of 7, counting the full gate run). The test reads its `base` packed count after `up(c, 3)`; under TSan the packer has already packed the four files by then (the log shows `packed_total` 52 to 56 at the restart), so it measures a delta of 0. Test race, left alone, to be recorded as its own K row by the coordinator. `k96_status_slots_live` (K107) passed in this run |
| `python3 scripts/check_k_register.py` | PASS (107 rows in each table) |
| `python3 scripts/check_doc_links.py`, `make docs-check` | PASS with this page committed (the architecture pages were regenerated, they changed with the source) |
| `sh scripts/rebuild-code-graph.sh`, `python3 tests/test_code_graph.py` | PASS (18685 nodes; 10 tests OK) |

## What is not verified

- (a) is unchanged and unfixed; the proposal is untested.
- The discovery cost remains: the first two writes after a halt still take 7.0 s and 5.0 s (four failed checkouts, 5 s each,
  two placements in parallel) before the member is marked down. It is the cost of learning silence, the same detector question as (a).
- Mute members are probed only while something asks (`is_down`, a checkout, a reprobe). After 30 s without any caller the
  mark can expire and the next checkouts re-learn it (up to 4 x 5 s). Making a single failure re-mark after an expired mark
  would remove it; not done (it changes `conn_get`'s documented "clean probe" after a cooldown).
- A member back from SIGSTOP is noticed by the background probe: measured at once in mode `r` (the probe was blocked in the
  declaration and the resumed server answered it). A member that returns after a long silence waits for the next probe
  (at most `EFS_NODE_PROBE_MS` plus the exchange). K35 case 7 (a 0.3 s race) passed in three runs; the live K34 test ran once
  (in verification), K36 not run.
- (c) was measured with the finisher disabled; with it enabled recovery is not needed in that scenario (Task FF, about 1 s). No
  measurement on a real hung host, on RDMA, or with more than three members. The recovery thread's cost when the cluster is
  idle (one pass per second, as before) was not measured separately.
- Raw outputs stay outside Git: intel1 `~maik/efs-artifacts/taskFL-20261110/` (loop outputs `before-*.txt`, `after-*.txt`,
  `iter-*` for the intermediate builds, verification outputs `v-*.txt`, build logs, binary hashes, and `fixtures.tar.gz` with the
  daemon and client logs of the final runs); `SHA256SUMS` SHA-256 `6b49f4182596003f458e095d59863862aa65987c62f292df4c5dc05f80561ac0`.
