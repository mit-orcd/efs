# K114 fix 2: the read-only receive sites join the cut (Task GC)

**Status (Nov 12, fix 3 added):** directory creates (MKDIR) go through `rpc_send_recv_dual` and waited the full 30 s receive bound on a halted dual voter, because that function called no hook; on intel2 the mkdir took 35.0 s. `rpc_send_recv_dual_once` now calls the hook before its receive when `rpc_reply_wait_cut` accepts the request and the call sets no bound of its own (`recv_ms == 0`). The same mkdir, member stopped with the request held inside it, now returns in 6.07 s (three runs), the directory exists once with the inode of the first answer, and the held first copy is replayed from the op-id receipt. REPORT stays uncut (K122): `rpc_reply_wait_cut` is 0 for it in every form, and a unit test runs it through the function and counts hook calls. Details in the last section.

**Fix 2 status (Nov 11):** two of the five read-only receive sites the design study named now call the reply-wait hook: the GETCHUNKS receive (`efs_client_rpc_getchunks_fileid`) and the receive of the unpinned query snapshot (`query_snapshot_rpc`). Both repeat a failed receive on another voter inside their own loop, so a cut ends in a correct answer: on intel2 a read through a fresh mount and a `.efs/find` query, with the member stopped (SIGSTOP) while the request was held inside it, returned the same bytes and the same names as before the stop in 6.1 s. Before: the read waited the 30 s receive bound; the `.efs/find` query has an 8 s command deadline (`src/client/efs_fuse/find_res.c`) and ended at that deadline with EBUSY (8.04, 8.06, 8.07 s on intel2, base code with the query hook call removed). The other sites are LEFT OUT on purpose, because their failed receive is returned to the caller and no repeat on another member follows (readdir, writer view, lane writer view, publication status), or because the receive bound is shorter than the probe (receipt query). Source: branch `claude/task-gc-k114-fix2-2e2792`, base `60c9fcdad` (devel). Row K114 stays OPEN (this task does not edit `docs/status/k/`). Raw output: intel2 `~maik/efs-artifacts/taskGC-20261111/` with `SHA256SUMS`.

Design input: [K114 design study](k114-uncovered-waits-design-20261111.md) section 3, [fix 1](k114-fix1-safe-repeats-20261111.md), [K106 (a)](k106a-inflight-wait-20261110.md).

## What changed

| file | change |
|---|---|
| `src/client/inode_rpc.c` `efs_client_rpc_getchunks_fileid` | `efs_client_conn_await_reply(nid, conn)` just before `efs_conn_recv_msg`, same position as in `rpc_send_recv_shard_once`; no guard: the function sends GETCHUNKS only, an immutable read |
| `src/client/inode_rpc.c` `query_snapshot_rpc` | the send and the receive were one `||` expression; split so the hook runs between them, guarded by `!pinned` (a query pinned to a token's node returns its failure instead of repeating it) |
| tests | new `tests/test_k114_read_sites.py` (unit, 4 scenarios), new `tests/test_k114_read_sites_live.py` (2 scenarios); `tests/test_getchunks_budget.py` and `tests/test_query_snapshot_failover.py` splice the two functions and got a stub line for the hook |
| registration | `Makefile` (4 unit rows in `make test`, 2 live rows in `test-long`), `tests/budgets.tsv` (6 rows), `tests/sanitizer_check.sh` (4 rows in both lanes) |

Nothing in `rpc_reply_wait_cut`, `rpc_send_recv_dual_once` or the node cache changed.

## Inventory

Every blocking receive of a metadata reply in `src/client/inode_rpc.c` (`grep -n efs_conn_recv_msg`). All use pooled TCP connections from the node pool (`efs_client_conn_get`, `rpc_owner_conn_shard`, `raft_voter_conn_skip`, `raft_dual_voter_conn`), which the hook can find; on RDMA the hook returns at once (design study, RDMA paragraph). Without an RPC deadline the wait is bounded by the connection's receive timeout (30 s); with a deadline, by the deadline.

| site (line) | request | read-only proof | failed receive today | verdict |
|---|---|---|---|---|
| `rpc_send_recv_shard_once` (`:1006`) | every type | already covered by `rpc_reply_wait_cut` (fix 1) | drop, `rpc_retry_peer`, loop | unchanged |
| `efs_client_rpc_getchunks_fileid` (`:2689`) | GETCHUNKS | `handler.c` calls `server_raft_host_getchunks`: `host_read_index` and reads, no `host_propose` | drop, `skip = nid`, loop of 16 attempts on another voter | **covered** |
| `query_snapshot_rpc` (`:3848`) | QUERY_SNAPSHOT | `handler.c` calls `server_raft_host_query_snapshot` / `_query_page`: `server_raft_host_query_read` (ReadIndex) and a host-memory page session, no propose | unpinned: drop, `skip = nid`, loop of 16 attempts; pinned (token names the node): returns `EFS_ERR_NET`, which `.efs/find` shows as EBUSY at its 8 s deadline | **covered unpinned**; pinned left out (no repeat) |
| `efs_client_rpc_readdir_cur` (`:1634`) | READDIR | `handler.c` calls `server_raft_host_readdir`: ReadIndex and reads | drop, returns `EFS_ERR_NET`; the only `goto retry` is for a BUSY reply; the caller (`efs_fuse/getattr.c:493`) turns it into `-EIO` | **left out**: a cut would turn a 30 s hang into an EIO after 6 s, not into a listing |
| `efs_client_rpc_writer_view` (`:1732`) | INODE_WRITER_VIEW | `server_raft_host_writer_view` (`_reuse`): ReadIndex and reads | drop, returns `EFS_ERR_NET`; its caller (`write.c:1638`) treats any failure as "needs ordinary repull" | **left out**: no repeat on another member |
| `efs_client_rpc_lane_writer_view` (`:2516`; no caller in `src/`, only `inode_rpc.c`, `client_internal.h` and tests) | LANE_WRITER_VIEW | the lane writer view handler: ReadIndex and reads | drop, returns `EFS_ERR_NET` | **left out**: no repeat |
| `efs_client_rpc_publication` (`:2453`) | PUBLICATION, PUBLICATION_STATUS, PUBLICATION_RETIRE | status only (`query_only == 1`) reads (the publication status handler); the other two propose | drop, returns `EFS_ERR_NET`; one site carries a read and two mutations (told apart by `query_only`) | **left out**: no repeat on another member, and the site is shared with mutations |
| `rpc_report_receipt_query` (`:3462`) | REPORT_CHUNKS with `EFS_REPORT_F_RECEIPT` | reads the host's receipt table (`request_receipt.c`), runs nothing | sets `efs_conn_set_recv_timeout(conn, 2000)`, drop, `EFS_ERR_NET` | **left out**: the 2 s bound is shorter than the 5 s probe exchange, so the hook would delay the failure to about 6 s (it runs the exchange before it checks the bound); and it belongs to the REPORT resolution (K122) |
| `rpc_send_recv_dual_once` (`:3300`) | REPORT and other dual-voter requests | REPORT proposes | drop, resend | REPORT **out of scope** (K122), pinned by `test_k114_read_sites.py report`; identified directory creates are covered by fix 3 (last section) |
| session command (`:1821`), put ticket (`:2298`), degraded mark (`:2396`), lane bootstrap (`:2577`; no caller in `src/`), session revoke (`:3932`) | RAFT_MKFS, LANE_BOOTSTRAP | these change state on the server | drop, another voter | **out of scope** (mutations without an identity decision) |

The REPORT path and the state-changing sites are not touched. Left-out sites need a repeat inside the caller first (a loop like the getchunks one) before a cut gives an answer instead of an early error: that is a separate change, named in the last section.

## Tests

- Unit (`tests/test_k114_read_sites.py`, fast suite; the `read_sites_*` rows are in the TSan lane list of `tests/sanitizer_check.sh` only, like the `waiter_cut_*` rows): the production text of `efs_client_rpc_getchunks_fileid` and `query_snapshot_rpc` is compiled with stubbed transport whose member is silent (every receive fails). `getchunks` and `query` assert the call order `send, hook, receive, drop` twice and that the second attempt skips the failed node; `pinned` asserts the order `send, receive, drop` and no hook call; `report` counts the calls of the hook in the whole of `inode_rpc.c` (comments stripped) and names the enclosing function of each: exactly `rpc_send_recv_shard_once`, `efs_client_rpc_getchunks_fileid`, `query_snapshot_rpc` and (fix 3) `rpc_send_recv_dual_once`; a call added to any other receive, including the ones that were not read-checked (lane bootstrap, put ticket, degraded mark, session revoke), fails it.
- Mutation proof (run on the Mac, each reverted): removing the hook call in getchunks fails `getchunks` (`order srdsrd want shrdshrd`); removing it in the query site fails `query`; dropping the `!pinned` guard fails `pinned`; adding a hook call to the lane writer view receive fails `report`.
- Live (`tests/test_k114_read_sites_live.py`, `test-long`): three loopback daemons, a mount on node 1, the hold hook `EFS_FAULT_HOLD_REQUEST_FILE` holding the first request of the type inside the member, SIGSTOP, wait for the client's `node-waiter-cut` line, SIGCONT, assert the answer equals the answer before the stop and the time is under 20 s. `getchunks`: the file is read through a fresh mount (no cached chunk map); the member held is the leader, node 3. `query`: `.efs/find` of the mount; the first dual voter, node 1, is held; the daemons run with `EFS_QUERY_REFRESH_MS=2000` and the test waits for the `query-publish ... files=N` log line of every member so that all catalogs hold the file before the baseline answer is taken (without that wait the answer differs between members for reasons of the catalog refresh, not of the cut: seen in one of six early runs).
- Margin of the query cut: the hook probes after 1 s and the exchange takes up to 5 s (`node_cache.c`, `EFS_NODE_PROBE_MS`, exchange bound), so the cut comes at about 6.0 s of the 8 s command deadline. A repeat on the other member that needs more than about 1.9 s ends in EBUSY at the deadline, and a probe that starts later than about 3 s into the call gives no cut at all (the hook gives up at the deadline). The live `query` scenario waits for the `node-waiter-cut` line and asserts the answer, so on a loaded host (slow catalog read on the other member, late probe) it can fail. It is not made independent of that margin: accepting "answer or EBUSY by the deadline" would let it pass without the cut, which is the thing it tests; the unit test `query` is the margin-free pin of the call order. Neither the deadline nor a test timeout was raised.
- The live `getchunks` scenario resumes the member as soon as the cut line appears. It does not test a leader that stays stopped: with a follower still hinting the stopped leader, the 16-attempt loop could end in EBUSY after about 5 s of pauses. Not tested.
- Live times on intel2 (final tree): getchunks 6.15 s (three solo runs 11.7 to 12.1 s whole), query 6.11 to 6.13 s in eight consecutive runs (whole run 11 to 14 s).
- Not live: readdir, writer view, lane writer view, publication status, receipt query (left out, nothing to test); the pinned query (no user operation reaches a pinned first request deterministically; unit test only).

## What K114 still leaves open

- Mutations without an identity: chmod, chown, utimens, XATTR, HOLD, FLOCK (a delayed repeat can undo a later change).
- REPORT: stays out until K122 is settled; the late-copy exposure for the truncate that gave up is K121.
- Left-out reads: readdir, writer view, publication status wait 30 s on a stopped member, and the pinned query page waits for its 8 s command deadline. Each needs a repeat on another member in its own loop first. The lane writer view and lane bootstrap have no caller in `src/`, so nothing waits on them today.
- Writer view (`inode_rpc.c:1732`; callers `write.c:1638-1643`, `:1871-1877`) treats any failure as "not an owned retry" and falls back to the ordinary rebase, so a cut would give the fallback, not a user error. Not done. A proof would need: the fallback's pull runs through the covered getchunks loop, shown live with the writer view held inside a stopped member. The receipt query needs a probe bound that fits its 2 s receive.
- The sites whose idempotence on the server was not read (put ticket, degraded mark, lane bootstrap, session command, revoke), the PUT, GET and data-commit receives (own deadlines).
- RDMA connections (the hook returns at once) and a dead or partitioned host (the probe's connect fails, the verdict is not silent).

## Fix 3: directory creates on the dual path (Nov 12)

Cause: `efs_client_rpc_create` sends a directory with `rpc_send_recv_dual` ("MKDIR scatters the child inode ... send dirs to a dual-host"), and `rpc_send_recv_dual_once` had no hook call (removed with REPORT in wave 81). A halted dual voter held every MKDIR sent to it for the whole receive bound.

Change (`src/client/inode_rpc.c`, `rpc_send_recv_dual_once`, after the receive timeout is set): `if (recv_ms == 0 && rpc_reply_wait_cut(type, req, req_len)) efs_client_conn_await_reply(nid, conn);`. The recv-failure branch is unchanged: drop the connection, `target = 0; skip = nid`, retry with the same request bytes and deadline. After the cut the node is marked down, so `raft_dual_voter_conn` picks the other dual voter.

Rule for the bound: the hook only runs when the call sets no bound of its own. The hook's probe starts after 1 s and its exchange can take about 5 s, so a caller bound shorter than that (the receipt query's 2 s) would be delayed past its bound. Both real callers pass either 0 (MKDIR) or a REPORT bound of 30 s and more.

### Request types that reach `rpc_send_recv_dual_once`

Callers of `rpc_send_recv_dual` (grep): `efs_client_rpc_create` (directories only) and the REPORT sender. The resolve loop (`rpc_resolve_identified`, `dual = 1`) re-runs `rpc_send_recv_dual_once` for the same two.

| request | `recv_ms` | `rpc_reply_wait_cut` | repeat on another dual voter with the same identity | verdict |
|---|---|---|---|---|
| INODE_CREATE with op-id suffix (every MKDIR; `opid_begin` fails with BUSY before any send otherwise) | 0 | 1 (`rpc_identified`) | yes: the request buffer is copied once and re-sent with its op-id (only the ack field is refreshed); the server answers a repeat from the op-id receipt (`opid-receipt: replay`), the same path the shard route (fix 1) already relies on, and the 30 s timeout already repeated it on another voter | cut |
| INODE_CREATE without op-id suffix | 0 | 0 (mutation, not identified) | no identity | never cut; unit test `mkdir` |
| REPORT_CHUNKS (type 67), any length, with or without publication id or receipt flag | 30000 + count/2 | 0 (mutation, not identified, no case in the switch) | not safe: late copy can meet a superseded ticket (K122) | never cut; unit test `report` runs it |
| UNLINK, LINK, RENAME_AT with op-id | not sent by the dual path (callers use the shard route) | 1 | same receipt rule | would be cut if a caller ever used the dual path; no caller does |

No type is accepted by `rpc_reply_wait_cut` with an unsafe repeat, so there was no stop condition.

### Tests (fix 3)

- Unit, `tests/test_k114_read_sites.py` (TSan lane list only, like the fix 2 rows; production text of `rpc_send_recv_dual_once`, `rpc_reply_wait_cut`, `rpc_type_mutates` compiled with a silent-member stub; the stub picker skips a member the hook marked down):
  - `report`: REPORT (type 67) run through the function with four length and bound combinations, zero hook calls; the text check now covers readdir, writer view, lane writer view, publication and the receipt query only.
  - `mkdir`: identified INODE_CREATE, order `shrdshrd`, second attempt on the other voter, both sent buffers byte-identical (same op-id); an INODE_CREATE without suffix is not cut.
  - `bounded`: identified INODE_CREATE with `recv_ms = 2000`, no hook call.
  - Mutation proofs (Mac, each reverted): hook removed fails `mkdir`; the `rpc_reply_wait_cut` guard removed fails `report` and `mkdir`; the `recv_ms == 0` guard removed fails `bounded`; `rpc_reply_wait_cut` widened to accept REPORT fails `report`.
- Live, `tests/test_k114_read_sites_live.py mkdir` (`test-long`): the fix 2 harness (three daemons, both Raft groups led by node 3, mount on node 1, hold hook on node 1 for type 45, SIGSTOP, wait for `node-waiter-cut`, SIGCONT). It asserts the `mkdir` call returned without error (an EEXIST would show a second execution), took under 20 s, the directory is there once, and after the held first copy is released a server logs `opid-receipt: replay` while the directory keeps its inode.
- Reproduction (intel2, tree without the fix, the same test with the cut wait replaced by a join): the mkdir returned after 35.04 s; the client printed `inode-rpc: retry type=45 attempt=0 why=recv rc=-6 req_len=332 recv_ms=30000 wait_ms=30030 cut=deadline nid=1`.
- With the fix, intel2, three runs: 6.07, 6.06, 6.07 s (whole runs 10.8 to 11.2 s).
