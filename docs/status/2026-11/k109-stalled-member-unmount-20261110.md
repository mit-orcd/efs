# K109: a clean unmount past a stalled storage member (Task FO)

**Merge state (Nov 11):** merged into devel in wave 80 together with the K113 fix; it was held out of devel since wave 76 because with it the K22 snapshot test met K113. K109 is VERIFY: the S2 method gate with clean unmounts during the stall is owed.

*The paragraph above is the current state. Everything below is the task's report as it was written before the merge: where it says "unmerged", "not merged" or names a next action, the paragraph above replaces it.*

Task FO, Nov 10 2026. Purpose: fix the finding Task EZ's batch S soak recorded as
K109 (drafted on Task EZ's branch as K121; the coordinator rules at the merge): while
one of three members was alive and reachable but its apply was stalled, 6,832
clean client unmounts failed their session revoke after 15 s although the other
two members sealed. The task changes no K row and not the counts line.

**Verdict: PASS on xorinox (loopback, three private members).** Before the fix
20 of 20 clean unmounts with one stalled member failed after 15.2 to 15.3 s;
after it 20 of 20 exit 0 in 0.3 s (the revoke itself 35 to 53 ms). Two stalled
members still fail closed. **The multi-host check (Task EZ's S2 method with clean
unmounts during the stall) is still owed.** The K109 row stays the
coordinator's to rule.

**Second round (review of `889ea7e7d`).** Five findings were fixed; each is in the
section named: (1) a worker parked at daemon stop used a pooled connection the stop had
already destroyed: workers now use their own connections and the stop wakes and drains them
first ("Fence workers and the daemon stop"); (2) the late-refusal argument and a test that
pins it ("A refusal is seen only if it arrives before a majority sealed"); (3) exact call
counts are checked again and the inline fallback has its own pass ("Tests"); (4) the guide,
`operations.md` and release note L29 now say what the code does ("Documents"); (5) the
budget row follows the rule, the wrapper has no timeout of its own and the driver polls no
clock ("Budget and waits").

**Third round (review of `3a984be56`).** Round 2's private connections had no cap. (1) At most
`HOST_FENCE_MAX_INFLIGHT` (32, the peer pool's size per member) exchanges wait on one member; past
it the member counts unreached at once and no worker starts ("Bounding the workers"). It is 32 and
not 1 on purpose, with a test that shows why. (2) Healthy-path cost measured: no difference on loopback
("The healthy path"). (3) The stop now wakes the workers when the handler drain begins, so an
unmount handler waiting on two silent members no longer holds the stop for the 30 s receive bound:
0.2 s instead of 30.4 s on devel ("Fence workers and the daemon stop"). (4) Unit coverage for arm,
disarm, wake, drain and resume on real sockets; `--sigstop` is gated; the `stop woke` count is
exact; the RDMA statement on this page was wrong and is corrected ("Tests", "Not verified").
(5) The budget row was re-measured under three 8-job gates: 200 s ("Budget and waits").

## Source and build

* Base: devel `cff8eb424`. Branch `claude/sad-brahmagupta-5d44a8`. Round 1: code
  commit `935c39e36` plus test commit `a8001b6f8` (the unit harness waits for its
  workers after TSan found a race in the harness; no product source differs). The
  20-run before/after table, the `SIGSTOP`, split-leader and two-stalled rows below
  ran on the round 1 tree (`935c39e36`). Round 2: code commit `02618815e`
  (worker connections, stop drain, tests, documents), then `1ab6d0180` (registry placement),
  `8d88c13e3` (comments, budget text, the client-visible rc in the documents), `06bb0e396` and
  `12c515461` (`settings.md`, `settings.h` for the new fault variable); the round 2 checks in
  "Verification on xorinox" ran on `12c515461`, and the page, ledger and graph commits that
  follow change no source. Round 3: devel merged first (`1605178cb`, `ledger.md` the only
  conflict), code and tests `a42a5cb85`, the driver's `--healthy` mode `adb479839`; the round 3
  checks ran on that tree (the page, budget text and graph commits after it change no source).
  Binaries `efsd`, `tests/faults/efsd`, `efs-fuse`, `efs-mgmt`, `efs-bench` from
  `make all tests/faults/efsd` on the host; build id `unknown` (no `.git` there).
  The "before" runs used the unmodified `git archive cff8eb424` tree built the
  same way.
* Changes: `src/server/raft_host.c` (`host_storage_fence_members` and its
  helpers, the worker connection registry `host_fence_rpc` / `host_fence_drain`,
  the drain call at the top of `server_raft_host_stop`;
  `server_raft_host_storage_session_fence` answers BUSY, not INVAL, while the host
  is not running, and in fault builds parks on `EFS_FAULT_FENCE_PARK`),
  `src/server/peer_pool.c` and `server_internal.h` (`server_peer_conn_new_armed`:
  the same private connection, with a hook that publishes the socket during the
  handshake), `src/server/handler.c` (round 3: `server_handlers_begin_stop` wakes the
  fence workers), `src/common/session_revoke.c` (comment),
  `tests/test_storage_fence_members.py`, new `tests/live/k109_stalled_member_unmount.py`
  and `tests/test_k109_stalled_member_unmount.py`, `Makefile`, `tests/budgets.tsv`,
  `tests/sanitizer_check.sh` (both new tests join the gate), and the three documents
  named under "Documents". `host_inode_rpc_peer` is as on devel (round 1 split it;
  round 2 reverted that, so `tests/test_readdir_reply_boundary.py` is unchanged).

## The fence path and its reply codes

Path: `efs_session_revoke` (`src/common/session_revoke.c`) sends BEGIN, then
`EFS_MD_SESS_STORAGE_FENCE` through `efs_session_storage_barrier`, which resends
for at most 15 s while the reply is BUSY or NOT_PRIMARY (any other code ends it),
then FENCE_LOC/ACK per touched shard, then FINISH (which repeats the fence in
`check_only` mode) and the cleanup. The group leader's `server_raft_host_submit`
reads the session authority, then calls `host_storage_fence_members`, which asks
every configured member with `EFS_MSG_STORAGE_SESSION_FENCE`; a member runs
`server_raft_host_storage_session_fence`.

**What "sealed" means.** The member's certificate
`meta/put-session/<uuid>` holds the floor (the new epoch), written, fsynced and
its parent directories synced (`storage_fence_seal`, `src/server/put_fence.c`).
From then on the member refuses a guarded PUT of an older epoch (STALE), and the
seal waits for guarded PUTs already in progress. In `check_only` mode (FINISH)
the member sealed nothing new: it proves the certificate already holds the floor
(a begin at floor-1 must be STALE).

**The rule kept.** A session is revoked only when a strict majority of the
configured members sealed it (K54, D46 item 6: `sealed * 2 > members`), the
membership is unchanged during the call, and the revoked epoch is committed in
the session shard log by BEGIN/FENCE_LOC/FINISH (the epoch-bound
`efs_session_finish_fence_epoch`, `src/meta/session.c`, rejects a session that has
moved on). The code and the K54 decision say strict majority, not every member;
the alpha guide's mount section, `operations.md` and release-note row L29 still
described the pre-K54 "every configured member" behaviour and are corrected.

| Reply | Source | Proves | Counts as |
| --- | --- | --- | --- |
| rc `0` | seal or proof done | sealed | **sealed** |
| exchange failed (no address, connect, send, recv; the receive bound is 30 s) | leader side | nothing | **unreached** |
| `NOT_PRIMARY` -15, `NO_QUORUM` -8, `NET` -6, `NOT_FOUND` -3 | the member's session read (`server_raft_host_submit`: ReadIndex or forward to a voter fails, or no record) | did not reach the seal | **not sealed** |
| `IO` -1 | certificate read, write, fsync or directory sync | may have written the file, not proven durable | **not sealed** |
| `BUSY` -13 | `check_only` and the certificate is below the floor; host not running; session shard busy | not sealed | **not sealed** |
| `STALE` -14 | the member's authoritative read shows another epoch than the floor, or a state other than ACTIVE or FENCING | the revoke is for a session that moved on | **refusal** |
| `INVAL` -5 | request invalid (floor below 2, `check_only` above 1, zero UUID, wrong payload length; no storage path configured) | the request is wrong | **refusal** |
| `PROTO` -7 | reply names another UUID or floor (leader-side check) | a protocol fault | **refusal** |

"Not sealed" and "unreached" never count toward the majority, so they cannot
weaken the rule; the only question is whether to fail the call. `NOT_FOUND` comes
after a successful read like `STALE` does, but it says the record is absent, not
that the epoch moved: the leader read the record a moment earlier, so it is a
member lagging a removal. Counting it as not sealed is the safe direction (a
majority is still needed), and below a majority it is returned. Unknown codes
default to "not sealed" for the same reason; only the three refusals fail early.

**A refusal is seen only if it arrives before a majority sealed.** The call
returns as soon as the majority is in (`raft_host.c:15454`, the wait loop leaves on
`refused`, on `sealed * 2 > count` or when no answer is pending), so a STALE, INVAL or
PROTO that arrives after the majority sealed is recorded and logged by its worker but
changes nothing. Why that is safe, with the lines that carry it:

1. *The majority is what the rule needs, and a refusal cannot lower it.* Only an `rc 0`
   reply that names this UUID and floor counts as sealed (`host_fence_class`,
   `raft_host.c:15375`; a mismatching reply is PROTO, not sealed). A late refusal is
   a member that did not seal; the count of members that did is unchanged by it.
2. *Stopping at the first refusal is an early exit, not the guard.* STALE (the member
   read another epoch or a state other than ACTIVE or FENCING) says the session moved
   on. That is also what the leader checks, itself, before it asks any member: the
   authority read at `raft_host.c:15819-15821` fails the call with STALE when the epoch is
   below 2, when the state is not FENCING (or ACTIVE, for FINISH), or when the epoch in
   the command differs from the authority's. INVAL (floor below 2, `check_only` above 1,
   zero UUID, wrong length) describes the request, which the leader built from that same
   authority read, or a member without a storage path (that member did not seal and
   the majority did). PROTO is one member's unusable reply.
3. *FINISH is epoch-bound and repeats the fence.* After the storage-fence call the client
   sends FINISH. The leader runs `host_storage_fence_members` again with `check_only=1`
   (a second majority has to prove the certificate already holds the floor), re-reads the
   authority (`raft_host.c:15819`), freezes the verified epoch in the command
   (`storage_finish_cmd`), and the applied command is `efs_session_finish_fence_epoch`
   (`raft_host.c:2942`, `src/meta/session.c:338`), which returns STALE when the session's
   epoch is not the one the fence verified (`session.c:308`). A session that moved on after
   the majority sealed therefore cannot be finished at the old epoch, and a late STALE is
   re-derived there.
4. *Sealing is monotonic.* A certificate only moves up, so a late seal at an older floor is
   harmless (see the stalled-member argument below).

The unit table pins both sides: its refusal rows hold the sealing members back until the
refusal is recorded (the call then must fail), and its `late` rows hold the refusing member
back until the call returned (the call must return OK, the refusal must still be unanswered at
return, and must arrive and be logged afterwards). The product does not promise which of the
two orders a real cluster shows.

## Defect and fix

`host_storage_fence_members` walked the members in order and returned at the
first reply whose rc was not OK. A member that is alive but cannot read the
session shard (its apply is deferred; the read fails with NOT_PRIMARY after the
400 ms ReadIndex bound) therefore failed the call, and the client's barrier
retried the same fence for 15 s on NOT_PRIMARY or BUSY until the bound. The
`rc=-6` of the soak is the same failure: the 15 s bound expires inside a client
RPC and the submit returns NET (`ms=15003` to `15020` in every failing line).
A member that never answers (SIGSTOP) also held the serial loop for the 30 s
receive bound.

The fix (`src/server/raft_host.c`):

* every member is asked at once; a remote answer is awaited by a detached worker
  that owns a copy of the endpoint and shares only a refcounted round, so the
  call can return while a slow member's worker is still waiting. The worker asks
  over its own connection (`host_fence_rpc`), not the shared pool, and the daemon
  stop wakes and joins it (next section). If a thread cannot be created the member
  is asked inline, as before; this node's own member is always asked inline;
* at most `HOST_FENCE_MAX_INFLIGHT` exchanges wait on one member (round 3, next section);
* the call returns when a majority sealed, when a refusal arrived, or when every
  member has answered; it never waits for the rest, and no new timeout, sleep
  or bound was added or changed (the 15 s barrier is untouched);
* below a majority it returns the first non-sealing reply code in member order,
  else BUSY: the error today's code returned for the same members;
* the success log line `sealed N of M members, unreached mask 0x..` is kept;
  its mask now means "not proven sealed when the majority was" (unreached, did not
  seal, or still answering);
* `server_raft_host_storage_session_fence` answers BUSY, with the UUID and floor
  echoed, when the host is not running. Before, it answered INVAL with a zero
  UUID, which the leader turned into PROTO: a restarting member would have been
  a refusal.

Callers: `host_storage_fence_members` has one caller (`server_raft_host_submit`,
STORAGE_FENCE and FINISH). `server_raft_host_storage_session_fence` is called by the daemon
handler (`EFS_MSG_STORAGE_SESSION_FENCE`) and by the fence itself for this node.
`server_peer_conn_new` keeps its behaviour (it now calls `server_peer_conn_new_armed`
with no hook); `peer_connect` has no other change.

## Bounding the workers

Round 2 started one detached thread per remote member per round and opened a private socket
in each, with no limit; the pool's 32-connection cap and its 3-failures-in-10-s cooldown no
longer applied. A member that never answers held one thread and one socket per round until the 30 s
receive bound, two rounds per unmount, so a burst of unmounts would pile up about 2 x unmounts x stalled
members within 30 to 60 s, and at the member a hung handler pins a connection per round against
`EFS_SERVER_MAX_CONNS`.

The fix (`raft_host.c`, `host_fence_claim` / `host_fence_release`): a counter per member id under
one mutex. A round claims a slot for each remote member before it starts the worker; the worker gives
it back when its exchange ended (before it records the answer, so a round that returns on that answer
never sees its own slot held). At `HOST_FENCE_MAX_INFLIGHT` waiting exchanges the round starts no
worker and counts that member unreached at once (`FENCE_CAPPED`, logged as `storage-fence:
member=N ... not asked: 32 exchanges already wait on it`, the first ten and every thousandth). Threads
and sockets are therefore bounded by 32 per remote member, whatever the unmount rate. No timeout or sleep
was added; the counter is released by the existing receive bound or by the stop's wake.

**Why 32 and not the one in-flight exchange the review suggested.** Unmounts on one node run
concurrently, and each round asks every remote member. With one exchange per member, a healthy
member that is answering a neighbour's round would be counted unreached for every other concurrent
round, and with this node plus one answering member being the only way to a majority of three, those
unmounts would fail closed (BUSY) on a healthy cluster. The unit test below runs 8 threads x 200
concurrent rounds against two healthy members and requires every round to succeed; with the cap
changed to 1 it fails (mutant below). 32 is the pool's size per peer (`EFS_PEER_DATA_CONNS`), so a stalled member
costs at most what a busy one cost before. The pool's down-cooldown is not restored (it needs
a clock, and the cap already bounds the cost): a member that refuses connections costs one failed
connect per round (a TCP refusal is immediate; a black-holed address is bounded at 2 s by
`EFS_CONNECT_TIMEOUT_SEC`, inside a worker, never inside the caller once a majority sealed).

**Past the cap on a healthy cluster.** When more than 32 rounds are waiting on the same healthy member at once, a
round that finds both remote members at the cap returns BUSY (only this node sealed); one at the cap is
covered by the other remote member. `efs_session_storage_barrier` (`session_revoke.c:29`) repeats the exact
request on BUSY and NOT_PRIMARY every 100 ms for up to 15 s, and both client calls that make the server run the fence use it:
STORAGE_FENCE (`session_revoke.c:346`) and FINISH (`:388`, the `check_only=1` round; the server runs the fence only for these two,
`raft_host.c:15884`). A burst
therefore waits for slots, bounded by the 15 s, rather than failing at once. A round that finds one member at the cap still
succeeds through the other. The burst case itself (more than 32 concurrent unmounts on one node) was not run.

**Unit tests** (`tests/test_storage_fence_members.py`, both programs run in `make test`):

* 100 rounds against a member that never answers: every round returns OK (this node and member 2),
  exactly 32 workers were started for the silent member (and 100 for the healthy one), and
  after release every slot is back at 0;
* both remote members at the cap: no worker starts, the round returns BUSY at once, only this node's
  check ran; one member at the cap: the other and this node are a majority, OK;
* 8 threads x 200 concurrent rounds against healthy members: every round OK, every slot back at 0;
* mutants: cap 1 (the concurrency rows abort), no cap (the 100-round row aborts on the spawn count).

## Fence workers and the daemon stop

Round 1 let a worker ask over `server_peer_conn_get` (the shared pool). The review found
that a worker parked in `efs_conn_recv_msg` when the daemon stops uses a connection the
pool shutdown (`server_peer_pool_shutdown`, `peer_pool.c`, phase `peer-pool` after
`raft-host`) already destroyed, and that its `server_peer_conn_drop` then destroys it again.
**Reproduced under ASan on the round 1 code** (below): `heap-use-after-free` in
`efs_conn_recv_msg` called from `host_fence_worker`, freed by `server_peer_pool_shutdown`.

The fix:

* `host_fence_rpc` opens a private connection (`server_peer_conn_new_armed`, the same
  connect, declare and RDMA upgrade as the pool's) and never touches the pool, so the pool
  shutdown cannot free what a worker holds. The connection is destroyed by the worker.
* Every worker lists its socket from the connect to the end of the exchange (the `arm`
  hook runs after `connect`, before the declare, and `disarm` before any close; both and
  the drain run under one mutex, so a listed socket is open). A worker also counts itself
  active for the whole exchange.
* `server_raft_host_fence_wake` (round 3) refuses new exchanges and calls `shutdown(fd)` on every
  listed socket without waiting. `server_handlers_begin_stop` calls it, so it runs when the
  stop begins, before the handler drain: an unmount handler waits for its workers, and the drain would
  otherwise wait for that handler for as long as the workers wait (below).
* `host_fence_drain`, the first call of `server_raft_host_stop` (before the host's own
  teardown, and so before the `peer-pool` phase), wakes again (a no-op for sockets already woken)
  and waits for the active count to reach 0. A TCP wait (declare, send or receive) returns at once;
  the worker records "transport unavailable" and exits. It logs `storage-fence: stop woke N
  worker(s) waiting on a member`, N counting each worker once however many wakes saw it.
  `server_raft_host_stop` clears the flag at its end (`host_fence_resume`), so a host started again asks members again.
* No timeout, sleep or bound is added; the wait is on a condition variable.

**An unmount handler waiting on two silent members (review item 3).** With nodes 2 and 3 both
reachable and never answering a fence, an unmount on node 1 waits for both workers (the
majority cannot be reached) for the 30 s receive bound. In round 2 the wake ran only in the
raft-host phase, after the handler drain, so a SIGTERM waited for that handler. Measured on xorinox
(`--silent2`, SIGTERM sent once both nodes had logged the parked fence): node 1 exits orderly, rc 0, in
**0.2 s** on this tree; **30.5 s** with only the `server_handlers_begin_stop` call removed (so the test fails
without the fix); **30.4 s on devel** (`cff8eb424` plus the fault hook). That wait is not new:
devel's serial loop waited the same 30 s on a pooled connection that nothing woke. The measured wait
stays inside the 40 s handler-drain share and the 50 s total. The review's 62 s worst case needs a
worker whose declare handshake and then its exchange each run to their 30 s bound; it was not
produced here (the longest measured is 30.5 s), and the wake cuts both waits. The unmount
client exits 1 in all three runs (fails closed). A fence a healthy handler had in hand when the stop begins
is now refused too (the node is stopping); the client retries it against the new leader.

Limits, stated plainly: (a) the socket is not listed during `connect()` itself, so a member
that does not answer SYN holds the drain for the connect's own bound (`EFS_CONNECT_TIMEOUT_SEC`, 2 s);
(b) RDMA: round 2 said an RDMA exchange waits on the completion queue and `shutdown(fd)`
does not wake it. That was wrong for the exchange. Its three waits are `efs_rdma_send_quiesce` (`rdma.c:1594`), the credit
stall in `efs_rdma_send_frame` (`rdma.c:1618`) and the receive wait `efs_rdma_message_wait` (`rdma.c:1748`, called by
`efs_conn_recv_msg`, `protocol.c:461`); each pauses in `message_pause` (`rdma.c:1717`), which polls the control socket
(`rc->tcp_fd`, the same fd the registry lists) every millisecond, and `tcp_retired` (`rdma.c:1709`) marks the connection
broken on a zero peek, which is what a shut-down socket returns. `send_credit` (`rdma.c:1564`) only spins for
`EFS_RDMA_CREDIT_SPIN_US`. So the stop wakes an RDMA exchange the way it wakes a TCP one. This is read from the code and
**not run** (every run here uses TCP); the RDMA upgrade's own waits (`efs_rdma_client_upgrade`, `rdma.c:1222`, inside
the armed window) were not read; (c) a fence the stopping node itself issues during the drain is refused
(`transport unavailable`, counted as unreached), which is what a stopping node should do.
The first design (list only the established connection) was tried on xorinox and was
wrong: with the member frozen the worker waits in the declare handshake, the drain waited
the 30 s bound for it and the stop hit its 50 s deadline (`outcome=forced` in the node log);
listing the socket from the connect fixed it.

Safety for a member that is alive but stalled (K54's argument was written for a
member that is down): the epoch is committed in the session shard log without
that member, and a storage admission needs a fresh lane-leader STATUS proof; a
lagging storage member delegates the whole session and ticket read to the lane
leader instead of using its own frozen KV (`raft_host.c:15882`), so it cannot
admit a revoked epoch from stale state. **Not run:** a hand-built old-epoch PUT
against the stalled member after a revoke (K54's control, with stall instead of
kill). It is listed under "Not verified".

## The healthy path (review item 2)

Round 2's worker connects, declares (and on RDMA upgrades) per remote member per round, inside the
majority wait; the pooled path had these done. Measured with `--healthy --runs 20` (three private
members, no stall, TCP over loopback on xorinox, batches of 20 clean unmounts alternated between the
trees, all built the same way; devel is measured at its branch point `cff8eb424` and again at its tip `bbe93d148`, which
differs from this branch only by this task's change): the client has no separate timer for the storage-fence
barrier, so the table gives the whole revoke (`ms=`: BEGIN, the barrier, the bitmap reads, the shard
fence, cleanup and FINISH with its second barrier) and `fence_ms=` (the shard-fence stage only; it does not
contain the barrier).

| Tree | Batch | Revoke `ms` median | max | `fence_ms` median | max |
| --- | --- | ---: | ---: | ---: | ---: |
| devel `cff8eb424` | 1 | 62 | 80 | 4.5 | 9 |
| devel `cff8eb424` | 2 | 61 | 99 | 4.5 | 7 |
| devel tip `bbe93d148` | 1 | 63 | 72 | 5.0 | 12 |
| devel tip `bbe93d148` | 2 | 60 | 78 | 5.0 | 8 |
| this branch | 1 | **44** | 53 | 5.0 | 8 |
| this branch | 2 | **44** | 55 | 5.0 | 7 |
| this branch (rerun with the devel-tip batches) | 3 | **43** | 57 | 4.0 | 14 |
| this branch (rerun with the devel-tip batches) | 4 | **42.5** | 48 | 5.0 | 10 |

The whole revoke is 17 to 20 ms faster at the median, against the branch point and against the devel tip alike, and the
shard-fence stage is unchanged. The difference between the two trees is this task's change, so the gain is the change's; the
mechanism was not isolated here (the likely cause is that devel asks the members one after another in both barriers
and this branch asks them at once, which more than pays for the private handshake on loopback). **Limits:** loopback
cannot show the handshake cost the review asked about, because it has no network round trip; on a real network the connect and declare
add about two round trips per remote member per round, inside the majority wait (the first remote answer
decides, so the slower of the two members does not add), and on RDMA the upgrade adds more. That was
not measured here. **Choice:** each exchange opens its own connection and no persistent fence connection is kept. A
persistent connection per member would remove the handshake, but it needs reuse, an idle reap, a drop
on error and a drain that finds it between exchanges, which is the complexity round 1's pool use
failed on; with 2 rounds per unmount and the numbers above the handshake is not worth it. If the
multi-host check shows the handshake in the revoke time, a persistent connection per member is the
next step.

## Reproduction

Driver `tests/live/k109_stalled_member_unmount.py` (reproduction:
`--runs 20 --report-only`, one stalled member). Three private fault-build `efsd`
on 127.0.0.1 ports 18901 to 18903, group leaders on node 1 (`place_leaders`),
the client mounted on node 1. Node 3 starts with `EFS_FAULT_COMPACT_STALL=1`,
`EFS_FAULT_COMPACT_FILE`, `EFS_FAULT_L0_BYTES=1048576` and
`EFS_FAULT_MEMTABLE_BYTES=262144` (the K22 rehearsal caps; no hook was added).
With the marker file present, two `efs-bench --meta` loops fill its L0 until
`kv_l0_bytes >= kv_l0_cap` and `kv_apply_deferred > 0` (22 to 24 s), then stop.
Node 3 stays alive and reachable and answers the fence with `rc=-15`. Per run: mount, write
and fsync four files (64 KiB each), `scripts/client.sh stop`, read the client's
`session revoked at unmount` line. Raw output stays outside Git (below).

| Variant | Tree | Runs | Exit 0 | Wall per unmount | Revoke `ms=` | rc |
| --- | --- | ---: | ---: | --- | --- | --- |
| node 3 apply stalled | before (`cff8eb424`) | 20 | **0** | 15.2 to 15.3 s | 15002 to 15007 | -15 (20) |
| node 3 apply stalled | after (`935c39e36`) | 20 | **20** | 0.3 s | 35 to 53 | 0 (20) |
| node 3 `SIGSTOP`ped (never answers) | before | 3 | **0** | 15.2 to 15.3 s | 15010 to 15020 | -6 (3) |
| node 3 `SIGSTOP`ped | after | 5 | **5** | 0.3 s | 40 to 52 | 0 (5) |
| apply stalled, group 0 leader n1, group 2 leader n2, client on n2 | after | 5 | **5** | 0.3 to 0.5 s | 46 to 207 | 0 (5) |
| nodes 2 and 3 apply stalled (one of three sealed) | before | 1 | 0 | 15.3 s | 15003 | -6, `phase=storage-fence` |
| nodes 2 and 3 apply stalled | after | 1 | 0 (fails closed) | 15.3 s | 15005 | -6, `phase=storage-fence` |

Before, the leaders logged `storage-fence: member=3 floor=2 check=0 rc=-15` 340
times in the 20 runs (every 0.5 to 1 s per attempt). After, the leader still
logs the stalled member's answer (33 `check=0` and 22 `check=1` lines for
member 3 in the whole run, the rest from the two-stalled unmount) and the
unmount does not wait; the two-stalled unmount logs `reached 1 of 3 members, no
majority` 11 times.
The leaders-split run was added because the soak says "leaders"; the harness'
default puts both on node 1. The `SIGSTOP` variants stop node 3 after the
client's writes and `SIGCONT` it after the unmount, except the last run, where
node 1 is stopped cleanly with workers still waiting on the stopped node 3:
exit 0 in 30.1 s on the round 1 tree and 45.5 s on devel. In round 2 the same
run (`--sigstop --runs 1`, node 1's exit record parked so the process outlives
its teardown by 5 s) took 35.2 s: `elapsed_ms=30173` in the `shutdown:` line, of
which the drain is not a part (it woke 2 workers in 0.2 s after the stop began).
The rest is a wait on the frozen member elsewhere in the raft-host phase; it is
older than this task (45.5 s on devel), and was not attributed or changed here.
The `--park` run (below) has no frozen process and stops in 177 ms.

## Tests

| Test | Before | After |
| --- | --- | --- |
| `tests/test_storage_fence_members.py` (fast; 33 rows by two modes, each run with a thread per remote member and again with thread creation failing; slow-member, late-refusal, refusal-vs-slow, roster-change, duplicate-identity and short-roster cases; round 3: the per-member cap and concurrent healthy rounds, and a second program for the worker registry) | on devel `cff8eb424`: the nine did-not-seal rows with a majority sealed fail (`got -15 want 0`, `-13`, `-1`, `-3`); the adapter then hung, so later rows did not run on devel | PASS; plain x3 on xorinox, and in the TSan and ASan lanes |
| `tests/test_k109_stalled_member_unmount.py` (live; four clusters, below) | devel: 3 of 3 phase-1 unmounts fail after 15 s (`FAIL 1/4`); round 1 code under ASan: `FAIL`, heap-use-after-free in the stop (below); round 2 code: the `--silent2` stop takes 30.5 s (limit 12 s) | PASS 4/4, PASS 2/2 (`--park`), PASS 2/2 (`--sigstop`), PASS 1/1 (`--silent2`); in the ASan lane PASS |
| `tests/test_k54_member_down_fence.py`, `tests/test_k54_member2_unmount.py` (unchanged) | not run here | round 1: PASS, 25.6 s and 9.4 s; the log assertion `reached 2 of 4 members, no majority` holds. Round 2 changed the worker's connection, so these were rerun as part of `tests_for server/raft_host` (below) |

**Counts.** Every call asks each member once, and this node's own check runs once, whatever
the answers, so the unit table checks `calls == n` and `local_calls == 1` for every row
after the late workers settle. At return the count is exact where the call has to wait for
everyone: rows with no majority and no refusal (`done == n`), and every row of the inline pass
(`spawned == 0`, `done == n`). It cannot be exact at return for a row that returns on a majority
or on a refusal, because the workers still answering are the point of the design: the call is
required not to wait for them. The slow and late rows assert the opposite at return (not
all answered, `done < n` for the late rows) and the exact count afterwards. `local_calls == 1` is
checked at return for every row. Mutations run against the table: the local member not asked
(`local_calls==1` fails), the inline fallback not asking (the inline pass fails at its first row: `got -13 want 0`), and the wait loop
changed to wait for every member (the late and slow rows never finish, killed by hand: the table has
no timeout and none was added).

**Inline fallback.** The pass with `efsd_pthread_create` failing runs every table row without
workers: the result must equal the threaded result, no worker is spawned and every member has
answered at return. The inline path waits for each member in turn (which is why the guide says
the stop can wait on a slow member in that case).

**Registry unit test (round 3).** The test's second program compiles the worker registry
(`host_fence_arm`, `host_fence_disarm`, `host_fence_rpc`, `server_raft_host_fence_wake`, `host_fence_drain`,
`host_fence_resume`) with real socketpairs and stubs only for the connection setup and the message calls: a
completed exchange leaves nothing listed; a worker parked in an exchange and one parked in the
declare handshake are each woken by the drain (`rc -1`, nothing listed, nothing active, exactly one log line
`stop woke 1 worker(s)`); a new exchange after the stop began is refused before it connects; `resume` makes
the next exchange work; two parked workers woken by `wake` before the drain are counted once (`woke 2`),
with `wake` itself logging nothing; a socket opened after the wake is refused by `arm`. Mutants: the
wake not calling `shutdown` and the arm not listing the socket both hang the program (killed by hand; the
test has no timeout of its own, the gate's budget bounds it).

**Live test, four clusters.** (1) The L0-stall cluster of round 1: three clean unmounts with
node 3 stalled, then node 2 stalled and one refused unmount. (2) `--park --runs 1`: node 3 is a
fault build with `EFS_FAULT_FENCE_PARK=1` (a storage-fence request is never answered; the node
answers everything else) and node 1 has `EFS_FAULT_SHUTDOWN_RECORD_PARK=write`. One clean unmount
(its two fence rounds leave two workers connected to node 3 and waiting), then `SIGTERM` to
node 1; the test waits for node 1's `shutdown: outcome=orderly rc=0` line (all phases done, the
pool destroyed; the parked record keeps the process alive for up to 5 s) and only then kills
node 3. A worker the stop did not drain wakes on that kill and uses the freed connection.
It also requires node 1 to exit 0 and to log exactly `stop woke 2 worker(s)` (one worker per fence
round, two rounds per unmount; node 3's own log shows its two `parked by EFS_FAULT_FENCE_PARK` lines).
(3) `--sigstop --runs 1` (round 3, now gated): node 3 is `SIGSTOP`ped after the writes, so the workers wait in the declare
handshake; the same stop checks, `woke 2`. (4) `--silent2` (round 3): nodes 2 and 3 both park every
fence; the client's `stop` waits on node 1's handler, `SIGTERM` goes to node 1 once both nodes logged the
parked fence, and node 1 must exit 0 orderly in under 12 s (0.2 s measured; the limit is far below the 30 s
the workers would otherwise wait out) while the unmount fails closed. The hook,
`EFS_FAULT_FENCE_PARK` in `server_raft_host_storage_session_fence`, is a `for(;;)sleep(60)` park
under `#if EFS_FAULTS`, like `EFS_FAULT_SHUTDOWN_PARK`; it is product source but compiled only into
fault builds, inactive without the variable, and documented in `settings.md`. It is the one
`sleep` this round added to source and it is a test fault, not a wait on anything.

**The test fails without the fix.** Same driver and wrapper on the round 1 code (HEAD
`889ea7e7d` plus the fault hook), ASan lane (`EFS_SAN_ONLY=k109_stalled_member bash
tests/sanitizer_check.sh asan`): `FAIL`, `ERROR: AddressSanitizer: heap-use-after-free` in
`efs_conn_recv_msg` (`protocol.c:485`) called from `host_inode_rpc_addr` ← `host_fence_worker`,
freed by `efs_conn_destroy` ← `server_peer_pool_shutdown` ← `sd_peers`, allocated by
`peer_conn_get` in the same worker. On the round 2 code the same lane: PASS in 72 s.

## Budget and waits

`tests/budgets.tsv`: the unit test stays in `test` (30 s ceiling; 0 to 1 s measured). The live wrapper is
in `test-long`, slot 1, now **200 s** (round 2: 95 s). The wrapper runs four clusters since round 3
(`--sigstop` is gated and `--silent2` is new; the `--sigstop` stop alone takes about 35 s, 30 s of it a
wait on the frozen member that is older than this task). Measured on xorinox: three solo runs 124, 124, 126 s;
then three 8-job gates (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, the full test-all, each
TOTAL 603 FAIL 0, 887, 899 and 901 s) in which the row took **156.3, 165.5 and 152.2 s**. The file's
rule (tests/budgets.tsv header: the median of the host's three clean 8-job gates, times 1.25, rounded up to 5 s) gives
1.25 x 156.3 = 195.4, so **200 s**. It is one host, not the three the header names (intel1, xorinox, fstor007);
the slowest of the three gates, 165.5 s, is inside it. The first gate ran the interim 160 s row
and one gate (165.5 s) went over it, which is why the interim row was replaced. The three gates also report
`NOTRUN 11` (host limits: no cluster scripts, no RDMA hardware, no valgrind), so their status is INCOMPLETE as on
the earlier xorinox gates, and `OVERBUDGET 23 to 25` rows, listed in the gate summaries. None is K109's at 200 s. One is a stop-path test,
`test_stop_progress.py` (94 to 130 s against its 20 s row in the gates), so it was run solo twice on each of the devel tip and
this branch: 66 and 51 s on devel, 57 and 50 s here; the stop change adds no wait to it, and the rest of the rows are disk-bound
tests the change does not touch.

The wrapper no longer passes `timeout=900` to `subprocess.run`: a second limit besides the
row's budget, which the gate enforces. The drivers wait on log lines (`Cluster.wait_log`, the
harness' own 50 ms poll that fails at once if a daemon died) and the one fixed `time.sleep(2)` of
the first round (the L0-fill poll) is replaced by waiting for the node's own
`kv-bound: apply deferred` line, followed by the `raft-status` check that the stall holds.
The only remaining clock waits are the harness' poll loops and the 5 s the fault build
holds the exit record open (`EFS_SHUTDOWN_REPORT_MS`, product code, test-only variable).

## Documents

Corrected to what the code does (all three): the function returns the first non-sealing
member's code in member order, else BUSY when nobody answered. In the preserved two-stalled run
the leaders log members 2 and 3 answering `-15` (11 and 33 lines) and the call returned NOT_PRIMARY;
the client repeats the fence for 15 s on BUSY and NOT_PRIMARY only and then reports the call
its deadline cut, which is the `rc=-6` (NET) of that run (`ms=15005`). So the client-visible `-6` is
the deadline, not a member's code; IO and NOT_FOUND end the barrier at once with their own code (`efs_session_storage_barrier`, `session_revoke.c:46`); "the
stop does not wait for the slow member" is true for a remote member once a majority sealed, not
for the member on the node that runs the fence (asked inline) and not for any member when that
node cannot start a thread. The guide also states K54's residual window next to the claim that a
returning member refuses the revoked epoch: a live member the leader cannot reach, with an upload
that passed its status check before BEGIN, can still store that fragment after the revoke returned;
it cannot be settled and stays an unsettled ticket for the D31 orphan ledger, whose collection of
it stays blocked until that member holds the sealed epoch
([K54 page](../2026-10/k54-member-down-unmount-20261030.md)).

## Verification on xorinox

**Round 3, on `adb479839`.** `git archive` of that commit extracted under `~/efs-taskFO/src`, `make -j8 all
tests/faults/efsd`, TMPDIR under `~/efs-taskFO/tmp`, build id `unknown`; a second tree `base` from
`git archive cff8eb424` for the devel measurements (plus the fault hook for the silent-members stop), and
`src-nowake`, this tree with only the `server_raft_host_fence_wake()` call in `server_handlers_begin_stop`
removed. The commits after `adb479839` change this page, `tests/budgets.tsv` (the budget text and number) and the graph only. Logs in
`~maik/efs-artifacts/taskFO-round3-20261110/` (46 files, `SHA256SUMS` beside it).

| Check | Result |
| --- | --- |
| `python3 tests/test_storage_fence_members.py` x3 plain | 3 of 3 PASS, 0 to 1 s; 4 mutants of the round 3 code run on the Mac (cap 1, no cap, wake without `shutdown`, arm not listing) fail the test |
| `make test` | rc 0, 180 s |
| `python3 tests/test_k109_stalled_member_unmount.py` solo x3 | PASS x3: `4/4`, `2/2`, `2/2`, `1/1` each; 124, 124, 126 s |
| `--healthy --runs 20` x2 per tree | devel: revoke `ms` median 62 and 61, max 80 and 99; this branch: median 44 and 44, max 53 and 55 (above) |
| `--silent2` | this tree: exit 0 in 0.2 s; without the wake: 30.5 s (FAIL); devel: 30.4 s |
| `--park --runs 1`, `--sigstop --runs 1` | PASS 2/2 each, `stop woke 2 worker(s)` exact; node 1 exit 0 in 5.2 s and 35.0 s |
| `python3 scripts/tests_for.py common/session_revoke --run` | rc 0, 148 s |
| `python3 scripts/tests_for.py server/raft_host --run` | rc 0, 666 s |
| `bash tests/sanitizer_check.sh asan` | VERDICT PASS, 62 run, 0 failed, 518 s |
| `bash tests/sanitizer_check.sh tsan` | VERDICT PASS, 34 run, 0 failed, 682 s (`k37_segment_rebuild`, which failed once in round 2, passed) |
| three 8-job `make_test_summary.sh` gates | TOTAL 603, FAIL 0, NOTRUN 11 (host limits), INCOMPLETE, 887 / 899 / 901 s; the K109 row 156.3 / 165.5 / 152.2 s |
| `make docs-check` (Mac worktree, after `scripts/gen_diagnostics.py` for the new log keys) | rc 0, 0 dangling links, concern map and diagnostics checks pass |
| `python3 tests/test_make_test_recipe.py` | PASS (106 budget rows, budgeted gate inside its 1750 s ceiling) |

**Round 2, on the final tree.** `git archive 12c515461` extracted under `~/efs-taskFO/new`
(the page, ledger and graph commits after it change no source), `make all tests/faults/efsd`,
TMPDIR under `~/efs-taskFO/tmp`, build id `unknown`. Logs in
`~maik/efs-artifacts/taskFO-round2-20261110/`.

| Check | Result |
| --- | --- |
| `make test` | rc 0 (`make-test-2-final.log`). The first run stopped at `test_env_inventory` (the new fault variable had no `settings.md` row) and, after that was fixed, at `gen_settings --check` (the fault-hook count in `settings.h`); both fixed in `06bb0e396` and `12c515461`, the whole target then ran again from the start |
| `tests/test_storage_fence_members.py` x5 plain | 5 of 5 PASS (the unit test is part of `make test`) |
| `python3 tests/test_k109_stalled_member_unmount.py` | PASS: `PASS 4/4`, then `PASS 2/2` (the `--park` cluster: unmount clean in 0.3 s, `ms=57`, node 1 stopped with 2 workers waiting, exit 0 in 5.2 s including the 5 s the parked exit record holds the process, 0.2 s of teardown) |
| `python3 scripts/tests_for.py common/session_revoke --run` | rc 0 (7 tests; includes both K54 tests and the K109 wrapper) |
| `python3 scripts/tests_for.py server/raft_host --run` | rc 0 (145 tests). A first attempt failed at `test_query_page_barriers` because the new file-scope registry sat inside the region that test extracts; moved in `1ab6d0180` |
| `bash tests/sanitizer_check.sh asan` | VERDICT PASS, 61 run, 0 failed, 438 s; `storage_fence_members` and `k109_stalled_member` PASS |
| `bash tests/sanitizer_check.sh tsan` | VERDICT FAIL, 33 run, 1 failed, 724 s: `k37_segment_rebuild` (`refused files once node 3 is back: packer moved 0 of 4`, the packer intermittent already recorded in `sequence-binaries-sanitizers-20261109.md`; no fence code is on its path); rerun alone on the same TSan tree: PASS twice (53 s, 56 s). `storage_fence_members` and `k109_stalled_member` PASS; `k96_status_slots_live` PASS in this run (K107 is not touched here) |
| `python3 scripts/check_doc_links.py`, `python3 docs/gen-architecture-full.py`, `make docs-check`, `python3 tests/test_env_inventory.py`, `python3 tests/test_make_test_recipe.py` | 0 dangling links; architecture pages regenerate with no tracked change; docs-check and the inventory and budget-recipe tests PASS (on the Mac worktree; the xorinox tree has no `.git`, so `docs-check` cannot run there) |
| `python3 tests/test_code_graph.py` after `sh scripts/rebuild-code-graph.sh` | PASS (18,693 nodes; parser warnings as before this change) |
| Old code, same test, ASan lane | FAIL: heap-use-after-free, see "Tests" |

Solo timings of the wrapper (72, 73, 73 s), the first ASan pass of the new tree (72 s) and
the old-code ASan failure were taken on the round 2 tree before the three small commits that
followed (registry placement, comments, documents, `settings.h`): `02618815e`, which has the same
product behaviour; the final-tree runs above repeat the wrapper, the sanitizer lanes and the
component tests.

**Round 1** (tree `935c39e36`/`a8001b6f8`): `make test` rc 0 (2 m 55 s); `tests_for
common/session_revoke` rc 0 (7 tests, 1 m 35 s); `tests_for server/raft_host` rc 0 (145 tests,
10 m 28 s); TSan VERDICT FAIL 33 run 2 failed in 849 s: `k96_status_slots_live` (51 TSan reports,
K107, not this task's) and a race in the harness of the new unit test (a late worker's log against
the next row's reset; fixed in `a8001b6f8`, the two new entries then PASS under TSan in 63 s);
ASan VERDICT PASS, 59 run, 385 s (the new entries joined the ASan list after it and passed in
a separate run, the live test 101 s); unit test 40 plain, 10 TSan and 5 ASan builds, 0 failures;
docs checks and graph test as above.

The 20-run "after" table row ran on a build made before the success log line was
restored and the comments reworded (no logic differs); the other "after" rows
and the live test ran on the final build of round 1.

## Not verified

* The multi-host check, Task EZ's S2 method with clean unmounts during the
  stall (production caps, real hosts, RDMA), is still owed. Everything here is
  loopback on one host with the K22 rehearsal caps.
* An old-epoch PUT against the stalled member after a successful revoke (the
  K54 control with a stall in place of a kill) was not run; the argument above
  rests on the code path.
* The `gc_reclamation` fixture `--storage-session-fence-fault` (a certificate
  fsync that fails on armed members) was not run. With a fault on every member
  nothing seals and the call fails as before; with the fault on fewer than a
  majority the revoke now succeeds with those members unsealed (K54's rule).
* The stop drain is checked for TCP with a member that never answers a fence (`--park`), with
  one frozen by `SIGSTOP` (a worker waiting in the declare handshake) and with two silent members
  and an unmount handler waiting (`--silent2`). Not run: the RDMA stop (by reading of
  `message_pause`, the exchange is woken like a TCP one; the upgrade's own waits were not read; see above), and a stop
  while a worker is in `connect()` to an address that does not answer SYN (the connect's own
  2 s bound). The `--sigstop` stop takes 35 s in total, of which 5 s is the parked exit record: about 30 s are
  spent in the raft-host phase on the frozen member elsewhere than the fence workers (45.5 s on devel);
  that wait is older than this task and was not attributed. A worker's last steps after the drain returns (its answer recorded into its own
  refcounted round, one telemetry line, safe once the sink is unbound) can still run; they use no
  host state.
* Cost of the private connections. They are now capped (32 per member) but do not have the pool's
  down-cooldown, so a member that refuses connections costs one failed connect per round, and the
  per-round handshake was measured on loopback only (above), not on a real network or on RDMA, and
  not at production unmount rates.
* The 200 s row rests on one host (xorinox), not the three the budget header names.

## Reproduction and artifacts

```
git archive HEAD | ssh xorinox 'tar -x -C ~/efs-taskFO/src'      # then, there:
make -j8 all tests/faults/efsd
export TMPDIR=$HOME/efs-taskFO/tmp                               # work only under $HOME
python3 tests/live/k109_stalled_member_unmount.py --runs 20 --report-only --port-base 18900
python3 tests/live/k109_stalled_member_unmount.py --runs 5 --sigstop --port-base 18920
python3 tests/live/k109_stalled_member_unmount.py --runs 5 --split --client-node 2 --port-base 18940
python3 tests/live/k109_stalled_member_unmount.py --park --runs 1 --port-base 18960
python3 tests/live/k109_stalled_member_unmount.py --sigstop --runs 1 --port-base 18980
python3 tests/live/k109_stalled_member_unmount.py --silent2 --port-base 18900     # stop with a handler waiting on two silent members
python3 tests/live/k109_stalled_member_unmount.py --healthy --runs 20 --port-base 18920   # revoke ms and fence_ms, healthy cluster
python3 tests/test_k109_stalled_member_unmount.py
python3 tests/test_storage_fence_members.py
EFS_SAN_ONLY=k109_stalled_member bash tests/sanitizer_check.sh asan      # the stop with workers waiting, under ASan
```

Expected: every line `clean=1` with `ms=` under 5000 for the one-stalled
variants; `FAIL`/`clean=0` at about 15 s on a tree before `935c39e36`.
Cleanup: the driver removes its work directory on PASS; remove `~/efs-taskFO`
with `~/bin/edelete --force --delete`.

Round 1 raw output (logs of every run in the reproduction table, the nodes' and clients' text
logs of the before and after runs, the devel unit adapter and its output, `make test` and the
sanitizer logs) is in `~maik/efs-artifacts/taskFO-20261110/` on xorinox, `SHA256SUMS` beside it;
sha256 of the `SHA256SUMS` file: `d842ed44e66d99994f630d63a292e0e9485a278316d5c23b2cb38a2b78ba6592`.
Round 2 raw output (`make test`, unit repeats, wrapper, `tests_for`, TSan and ASan lanes, the
TSan rerun, the old-code ASan failure `asan-k109-round1-code-FAIL.log` with the full report, the
solo timings) is in `~maik/efs-artifacts/taskFO-round2-20261110/`, `SHA256SUMS` beside it (27
files); sha256 of that `SHA256SUMS`:
`baa0638d096d9d9a20cbdad56c29bf9afa00a7f4242eb462a02632afb295da8c`.
Round 3 raw output (the healthy-path batches on both trees, the three `--silent2` runs, `--park` and
`--sigstop`, the solo wrapper runs, `make test`, `tests_for`, the ASan and TSan lanes, the three 8-job gates with their
summaries and timings, the build logs and the orchestration scripts) is in
`~maik/efs-artifacts/taskFO-round3-20261110/`, `SHA256SUMS` beside it (46 files); sha256 of that `SHA256SUMS`:
`f7df039ca488f01e3607633a4845df74d52c7217be2d29ea1ca505baf39d3230`.
