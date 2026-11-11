# K114: which uncovered reply waits may be cut (Task FY, design study)

**Merge state (Nov 11):** merged into devel in wave 80. Row K114 now carries the study's result; the resend of a blocking FLOCK is the new row K120. No fix task is started yet.

*The paragraph above is the current state. Everything below is the task's report as it was written before the merge: where it says "unmerged", "not merged" or names a next action, the paragraph above replaces it.*

**Status (Nov 11):** source commit `acc7aab2f` (devel head), branch `claude/task-fy-k114-design-445aad`.
Verdict: **design only, no code changed** (nothing under `src/`, `include/` or `tests/` was touched; nothing was built or run).
Every statement about the code was found by reading, with the file and line given. Row [K114](../k/K114.md) is not changed by this page.
Not determined (what would decide it is named where it occurs): whether the client serialises HOLD open and close per inode; whether a
server handler thread of a cut request can still propose after the repeat has finished; the listen backlog of a member (does a full
accept queue look like a dead host); the idempotence of the PUT, put-ticket, degraded-mark, lane-bootstrap and publication
requests on their servers; whether the size and mtime records of a REPORT without publication id are grow-only in apply.

Purpose: decide, for each reply wait that the K106 (a) cut does not cover, whether sending the request again after an early cut can do
harm. Related: [K106 (a) fix page](k106a-inflight-wait-20261110.md), [ledger](ledger.md).

## 1. What the client does today

- One loop serves every request that goes through `rpc_send_recv_shard_once` (`src/client/inode_rpc.c:901`). After a failed send it
  drops the connection and picks another voter (`:962-975`). After a failed receive it does the same: `efs_client_conn_drop`,
  `rpc_retry_peer` (marks the node failed, forgets it as leader, `skip` = that node; `:795-803`), a retry pause, `continue`
  (`:996-1012`). The request is sent again with the same `req` bytes (`:1028`). Only the four directory ops are copied
  (`create_retry`, `:928-936`) so that `opid_retry_ack` refreshes their ack before each attempt (`:939`).
- How often and until when: `for (; deadline_bounded || attempt < 16; attempt++)` (`:936`). With an RPC deadline set the loop runs until
  the deadline; without one, 16 attempts. A retry pause separates attempts (`rpc_writer_retry_pause`, `:972`).
- **Covered type** (`rpc_reply_wait_cut`, `:896-899`: not a mutation, or a directory op with its op-id): before the receive
  `efs_client_conn_await_reply` (`src/client/node_cache.c:1035`) polls the connection. After `EFS_NODE_PROBE_MS` (1 s,
  `node_cache.c:38`) one waiter per node runs `node_exchange` (`:599`). Verdict silent: the node is marked down, waiting connections
  are shut down (`:1089-1110`), their receive fails and the loop above resends. Verdict answered or failed: everyone keeps waiting.
  With no deadline the hook ends the wait itself at the connection's receive bound (`:1051-1059`).
- **Uncovered type**: no hook; the receive blocks until the reply, the connection's `SO_RCVTIMEO` (30 s on pooled connections,
  `node_cache.c:847`) or the RPC deadline (section 5). Then the same drop, `rpc_retry_peer` and resend as above.
- **Confirmed:** the reviewer's statement is true. devel resends every request type after a failed receive, covered or not. A request
  to a halted member is therefore sent again after 30 s today. What an early cut changes is the timing, and with it the odds of the
  **false cut**: the member is alive but stalled (for example SIGSTOP, then SIGCONT after the cut), it executes the first copy later,
  and that copy meets the repeat in the log or arrives after the client moved on.
- Only for the covered four directory ops `rpc_resolve_identified` (`:1167`) re-sends the identical request under a fresh bound
  when the reply was lost. SETATTR, APPEND and CREATE_RETAINED are not in `rpc_identified` (`:77-87`), so they get neither the cut
  nor that pass. Discrepancy: the design note of the K106 (a) page lists SETATTR, FLOCK, XATTR and REPORT as covered; the code does not
  (`:896-899` returns 0 for them; REPORT goes through the dual path, `:3719`).

## 2. Table of the request types

Reading key: a = effect of a second execution, b = identity and where checked, c = concurrent repeat, d = harm of a wrong repeat.
"Delayed duplicate" = the first copy is applied after the client got the repeat's reply and issued its next request.

| Type | a. Second execution | b. Identity | c. Concurrent repeat | d. Harm if wrong | e. Class |
|---|---|---|---|---|---|
| SETATTR, truncate (SIZE\|MTIME) | no-op, receipt replays the recorded result (`src/meta/meta_apply/append.c:352-368`) | op-id uuid, epoch, seq, ack built at `inode_rpc.c:2868-2900`; handler refuses a missing suffix (`src/server/handler.c:1036-1056`); `host_truncate_lazy` packs it (`src/server/raft_host.c:22132`); the leader does not probe, apply does | apply probes first, in log order, before the epoch checks (`append.c:352-368`) | none | **SAFE WITH AN EXISTING IDENTITY** (TRUNCATE receipt) |
| SETATTR, chmod/chown | second effect, idempotent by value only; `efs_meta_apply_setattr` has no receipt (`src/meta/meta_apply/namespace.c:882-912`) | uuid and epoch only in the command (`raft_host.c:7094-7103`, session gate `:22318-22323`); the wire carries the full op-id but apply never records the seq | two proposals, last applied wins | chmod X repeated after another client's chmod Y reverts the file to X | **NOT SAFE** across a competing writer |
| SETATTR, utimens | second effect, last writer wins; `mtime_gen` is recomputed each time (`raft_host.c:22084`, `apply_utimens_cmd` `:1933-1968`) | uuid and epoch only (`pack_utimens_cmd` `:7117-7133`) | no receipt, a fresh gen always passes | an older time replayed over a newer one | **NOT SAFE** across a competing writer |
| APPEND (reserve, abandon) | no-op: same offset replayed, or STALE once the receipt is retired (`append.c:248-277`) | op-id plus ack (`inode_rpc.c:3000-3012`, `handler.c:1100-1126`); the leader probes `efs_meta_append_result` before proposing (`raft_host.c:19970`) | per-inode trylock gives BUSY (`raft_host.c:22534-22548`); capture rechecks the receipt image (`append.c:~714-731`); a different offset gets INVAL | none for reserve; abandon replays OK (`retired=0` on the replay) | **SAFE WITH AN EXISTING IDENTITY** (APPEND receipt, ack watermark) |
| HOLD | no-op: a lease key in a set, open twice is OK, close of an absent key is OK (`src/meta/session.c:628-668`, line 633) | session uuid and epoch (`inode_rpc.c:3056-3075`); the `owner` field is sent but never used (`raft_host.c:17240-17290`); gate `:17278`, apply `efs_session_accept` (`session.c:620`) | two OPENs or two CLOSEs collapse | a delayed OPEN after the client's CLOSE re-creates the lease (inode stays held to session end); a delayed CLOSE after a re-OPEN drops a wanted hold | **SAFE AS IT IS** in log order; **NOT SAFE** against a delayed duplicate; whether the client serialises open and close per inode: not determined (read `fuse` open/release callers) |
| REPORT_CHUNKS with publication id | no-op: receipt answered without running (`src/common/request_receipt.c:186-229`); durable verdict in apply (`src/meta/meta_apply/publish.c:104-113, 716-775`); `candidate_gen == committed` returns OK (`:293-298`) | session plus pubid (mint, seq): built `inode_rpc.c:3706-3712`, parsed `handler.c:1250-1279` | same host: joined by receipt (5 s, then BUSY, `request_receipt.c:233-243`); new leader: receipt table is in memory per host, the unit transaction is deterministic in (session, pubid, ino, first chunk) (`raft_host.c:24362-24410`) and the chunk CAS makes the loser skip | none | **SAFE WITH AN EXISTING IDENTITY** (session, pubid, CAS); the client also has a receipt query and same-identity resend (`inode_rpc.c:3465-3585`) |
| REPORT_CHUNKS without pubid | data safe by CAS and grow-only size (`handler.c:1235`; apply side not verified) | session only (`inode_rpc.c:3611-3617`) | CAS decides | a repeat can return STALE for a request that succeeded | **SAFE AS IT IS** for the data, false STALE possible; grow-only claim: not determined |
| FLOCK, non-blocking lock, unlock | same owner and range: OK (`src/meta/lock.c:228-232`); unlock of nothing: OK | session uuid and epoch plus lock owner, no seq (`inode_rpc.c:3082-3125`; gate `raft_host.c:17606-17607`, not for GETLK; `efs_lock_release` has no session check) | each copy is its own proposal | lock, unlock, delayed lock re-takes a released lock; lock, unlock, lock, delayed unlock drops the live lock; a duplicate after another owner took it returns LOCKED to the holder | **SAFE AS IT IS** in log order; **NOT SAFE** against a delayed duplicate |
| FLOCK, blocking (WAIT) | the server queues the request in an unbounded wait (`raft_host.c:17392-17480`); a resend queues a second waiter | as above | both waiters can grant; a stale one can grant after cancel or unlock | stale grant | **NOT SAFE** to resend; today the 30 s receive timeout already resends it (a pooled connection has no longer bound, `node_cache.c:847`); disconnect cleanup of the waiter: not determined |
| XATTR set | plain SET of the same value OK; SET\|CREATE repeated gives EEXIST; SET\|REPLACE of an absent name NODATA (`src/meta/meta_apply/xattr.c:160-185`) | session only (`inode_rpc.c:3769-3774`, `handler.c:754-790`, `raft_host.c:22394`, apply `:1913`) | separate log entries, no receipt | SET repeated after another client's change overwrites it; false EEXIST after success | **NOT SAFE** (lost update, false error) |
| XATTR remove | second remove gives NODATA (`xattr.c:181-185`) | session only | as above | removes a value another client set meanwhile; false ENODATA after success | **NOT SAFE** |
| CREATE_RETAINED, probe=0 | no-op or the recorded verdict (`known=1`) | full op-id with ack in `struct efs_msg_create_retained.q` (`inode_rpc.c:1448-1456, 1466`); `host_retained_create_proof` (`raft_host.c:18505-18539`) and `host_opid_replay` (`:18110`); `host_create_ex` re-probes (`:18362`) | apply probes the receipt first (`src/meta/meta_apply/create.c:155-163, 511-519`) | none | **SAFE WITH AN EXISTING IDENTITY** (directory op-id window); window-full case (`init.c:~250-262`) not determined |
| CREATE_RETAINED, probe=1 | read | none needed | none | none | already covered: `rpc_type_mutates` is 0 for probe (`inode_rpc.c:874-876`) |

Notes. (1) SETATTR, APPEND and CREATE_RETAINED carry an op-id that the server stores; they were left out of the cut only because
`rpc_identified` is a length test for four directory types. (2) For every row the first copy may still be inside the server (a handler
thread waiting in `host_read_index` or a propose) when the repeat completes. Whether such a thread can propose after the client has
moved on is not determined; read the handler paths for `host_propose` after a failed or closed connection to decide. The hazard exists
today after 30 s; an early cut makes it likelier, not new.

## 3. Receive sites the cut does not reach

Bound rule (section 5): with an RPC deadline the bound is the deadline, otherwise `SO_RCVTIMEO` of the connection. All connections
below come from the node pool unless stated.

| Site | Waits for | Today | Hook with the same meaning? |
|---|---|---|---|
| `rpc_send_recv_dual_once` `inode_rpc.c:3209-3305` (receive `:3279`) | REPORT and other dual-voter requests; sets `efs_conn_set_recv_timeout(recv_ms)` (`:3277`) | drop, resend to another dual voter with the same identity | yes for a replay-safe type (REPORT with pubid); the hook reads `conn->recv_timeout_ms` |
| `:1616` readdir, `:1714` writer view, `:2498` lane writer view, `:3441` receipt query (2 s, `:3436`), `:3824` query snapshot, `:2668` getchunks | reads and queries | drop; most return `EFS_ERR_NET` with no resend inside; getchunks and query snapshot resend up to 16 times | yes, replay-safe; the caller's existing retry then applies, per site |
| `:1803` session command, `:3906` session revoke | RAFT_MKFS session commands, identified, steps idempotent by their comments (`:1789-1793`, `:3905-3911`) | drop, another voter | yes by those comments |
| `:2280` put ticket, `:2378` degraded mark, `:2435` publication, `:2559` lane bootstrap | RAFT_MKFS and publication requests | drop, another voter or `EFS_ERR_NET` | not determined (idempotence on the server not read); publication STATUS only is a query |
| PUT reply `src/client/write.c:3012` (called `:3278`) | PUT_CHUNK_REPLY | the receive follows a readiness poll with the PUT deadline in slices of at most 2 s (`:3517, 3566`), quorum drain 2 s (`:3610-3627`); a failed fragment is placed around or resent with the same ticket (`:3275-3276`) | no: a poll with its own deadline precedes the receive; server idempotence of a repeated PUT: not determined |
| GET reply `src/client/read.c:134` | GET_CHUNK_REPLY | readiness watch first; pair bound 5 s or the parent deadline (`:298-307`); other fragments are tried | no, same reason |
| `read.c:359` segment, `:963` single fragment | GET_SEGMENT, GET_CHUNK | 2 s deadline (`:401-407`, `:1014-1017`) | replay-safe, but a deadline under 5 s means the exchange never yields "silent" (`node_cache.c:603-606, 626`); the 2 s deadline is the shorter bound |
| `src/client/data_commit.c:325` | DATA_COMMIT_REPLY | `j->deadline` (`:310-311`); one retry on a new connection (`:313-316`); the file comment calls commit idempotent | yes if the deadline is 5 s or more: not determined |
| `src/client/put_ticket_owner.c` | no receive calls | | |
| `node_cache.c:221, 224, 1257`, `protocol.c:825` | declaration on a fresh connection | local bound | not applicable (private connections) |

**RDMA.** A pooled connection upgraded by `efs_rdma_client_upgrade` (`node_cache.c:873`) keeps its TCP descriptor only as a control
socket and carries inode RPCs too. The hook returns at once for it (`node_cache.c:1037`) because a poll on `conn->fd` never sees a reply:
replies arrive as completions. `efs_conn_recv_msg` calls `conn_rdma_frame` (`src/common/protocol.c:455-484`) and `efs_rdma_message_wait`
with the remaining time of the receive bound (`:457-459`); that loop polls the completion eventfd and the control socket in 1 ms steps
(`src/common/rdma.c:1715-1751`) and leaves early only on a frame or on bytes or EOF on the control socket (`:1706-1713`). A SIGSTOPped
peer keeps the socket open and silent, so the wait lasts to the bound. Covering RDMA needs a wait on the completion eventfd
(`efs_rdma_reply_fd`, the one `efs_conn_reply_watch_quick` uses) in place of the descriptor poll; that is a new mechanism.

## 4. A powered-off or partitioned host

- `node_exchange` (`node_cache.c:599-650`) sets a local deadline `EFS_NODE_ADMISSION_MS` = 5 s (`:41`) and calls `efs_connect_tcp`.
  `connect_sockaddr` ends the connect at `EFS_CONNECT_TIMEOUT_SEC` = 2 s (`include/efs/network.h:16`, `src/common/network.c:67-69`) or at
  the deadline if sooner. A silent network path therefore fails the connect after 2 s with `ETIMEDOUT` (`network.c:100-105, 118`); a refusal
  or "no route" fails at once.
- Verdicts. **Answered:** the connection opened and `efs_conn_declare` was answered (`node_cache.c:625`). **Silent:** the connection
  opened, the declaration stayed unanswered for the whole local bound, and the bound was not the caller's deadline (`:626`); only
  this verdict marks the peer mute. **Failed:** everything else: no connect, reset, refusal, local failure, the caller's own deadline.
  `node_exchange` does not read `errno`, so a connect timeout and a refusal are the same verdict.
- Consequence: a host that is off or partitioned never gets the connect that precedes "silent", so nothing is cut and the wait stays
  30 s (or the deadline). Same when the request's deadline is less than 5 s away.
- Would "failed after the whole bound" be treated like silent without cutting a member that is only restarting? A restarting process on a
  live host answers the new connect with a reset at once, and the waiting connection gets the same reset, so its receive fails without
  any cut. A rebooting host is silent for the reboot and its old connection is gone. The case that would be cut wrongly is a live
  member that does not complete a handshake within 2 s, for example a full accept queue. Whether that can happen depends on the
  listen backlog (`efs_listen_tcp`, `network.c:308`) and the accept loop of the server: not determined. The rule needs `errno` =
  `ETIMEDOUT` from the connect (not a refusal), which the current code does not keep. No new timeout is proposed.

## 5. K115 checked

True for TCP. `efs_recv_all` (`src/common/network.c:352-385`): when a deadline is set (`net_deadline_ms != 0`) it receives with
`MSG_DONTWAIT` and polls for the time left to the deadline; `SO_RCVTIMEO` is not consulted. Without a deadline it blocks in `recv`,
which `SO_RCVTIMEO` bounds per call, so a peer that trickles bytes restarts it. `efs_conn_set_recv_timeout` (`:516-523`) only stores
`recv_timeout_ms` and sets that socket option. Effects on this page: it changes no classification in section 2. It changes the "today"
bounds of section 3 (a 2 s receipt query or a REPORT bound set with `recv_ms` holds under a deadline only if the deadline is shorter;
on RDMA `conn_rdma_frame` uses `recv_timeout_ms` directly, `protocol.c:457-459`). The hook's no-deadline branch uses
`conn->recv_timeout_ms` itself (`node_cache.c:1051-1059`), so it is consistent with the socket option and does not depend on K115.

## 6. Proposal (this page only; nothing built)

**(i) Add to the cut now, no new mechanism** (all have a stored identity or are reads):
1. `rpc_reply_wait_cut` also says yes for SETATTR with SIZE (truncate), APPEND reserve and abandon, and CREATE_RETAINED with probe=0.
   One task, one predicate, in `inode_rpc.c`. Proof: with the fault daemon, SIGSTOP the leader, issue the request, wait for
   `node-waiter-cut`, SIGCONT the old leader once the repeat has answered, then check the effect happened once: the append offset and
   file size equal one reservation, the truncate epoch moved once, one retained object exists. Plus a unit case that feeds the same
   identity twice to apply (existing receipt tests: `tests/test_append_sequence.py`, `tests/test_create_retry_identity.py`).
2. The hook in `rpc_send_recv_dual_once` for REPORT with a publication id, and at the replay-safe read and query sites of section 3
   (readdir, writer view, getchunks, query snapshot, receipt query). One task per file area; for the no-resend sites first check that
   the caller retries, otherwise the cut only turns 30 s into an earlier error. Proof: the same SIGSTOP, SIGCONT run with a REPORT, and a
   read that must return the right bytes.

**(ii) Needs an identity or a "what happened" query first.**
3. SETATTR chmod, chown and utimens: the wire already carries the op-id (`inode_rpc.c:2868-2900`), the command and apply drop the seq.
   Smallest existing mechanism: the directory op-id replay window (`src/meta/meta_apply/init.c`, probe and fold used by
   `create.c:155-163`) or the truncate receipt shape (`append.c:290-400`). One task: record and probe it for these masks, then add them to (i).
4. XATTR set and remove, HOLD, FLOCK non-blocking and unlock: the wire carries session only, so a sequence number must be added to the
   request and recorded in apply (a format change takes effect on a fresh cluster). The only existing "what happened" query is the
   REPORT receipt query (`inode_rpc.c:3441`, `request_receipt.c`); nothing equivalent exists for these types. HOLD may need none if the
   client serialises open and close per inode and a handler cannot propose after the repeat; that is the open question of row HOLD above.
   One task per family (XATTR; HOLD and FLOCK).

**(iii) Stay uncovered in alpha**, with the limitation as a user reads it:
- Blocking lock requests: "A blocked `flock` or `fcntl` wait on a stopped metadata member can last 30 s before it is retried, and the retry
  may leave a second waiter queued." This is also a defect today (the 30 s receive timeout resends it); it needs its own row, since the
  hook cannot help (the live member answers the probe).
- RDMA connections: "On RDMA a request to a stopped member waits up to the receive bound (30 s)." Needs the completion-eventfd wait.
- PUT, GET and data-commit replies: they already end at their own deadlines (2 s to 5 s) and fragments are placed or read around the
  member; no change proposed.
- A powered-off or partitioned host: "A request in flight to a host that is off or unreachable waits up to 30 s." The fix would key on
  `ETIMEDOUT` of the connect, not on a new timeout; blocked on the accept-queue question in section 4.

**Order of work:** 1, then 2 (independent of each other), then 3, then 4. A separate row for the blocking-lock resend should be filed
by the coordinator; this task does not edit the K register.

## Limitations

By reading only: no run, no fault test, no cluster. Findings of the three sub-readings (request classes, receive sites) were each
checked at their key lines (xattr apply, lock grant, lease open, lock wait loop, truncate receipt, append result, create probe);
the remaining lines are as read by the sub-readers and carry the same file and line. Lines marked `~` are approximate. Reproduction of
this study: `graphify explain "client/inode_rpc"`, `sed -n 880,1130p src/client/inode_rpc.c`, `sed -n 1035,1130p src/client/node_cache.c`,
`sed -n 599,650p src/client/node_cache.c`, `sed -n 352,385p src/common/network.c`, and the files named in the table.
