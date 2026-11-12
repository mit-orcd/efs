# K114 fix 2: the read-only receive sites join the cut (Task GC)

**Status (Nov 11):** two of the five read-only receive sites the design study named now call the reply-wait hook: the GETCHUNKS receive (`efs_client_rpc_getchunks_fileid`) and the receive of the unpinned query snapshot (`query_snapshot_rpc`). Both repeat a failed receive on another voter inside their own loop, so a cut ends in a correct answer: on intel2 a read through a fresh mount and a `.efs/find` query, with the member stopped (SIGSTOP) while the request was held inside it, returned the same bytes and the same names as before the stop in 6.1 s, not 30 s. The other sites are LEFT OUT on purpose, because their failed receive is returned to the caller and no repeat on another member follows (readdir, writer view, lane writer view, publication status), or because the receive bound is shorter than the probe (receipt query). Source: branch `claude/task-gc-k114-fix2-2e2792`, base `60c9fcdad` (devel). Row K114 stays OPEN (this task does not edit `docs/status/k/`). Raw output: intel2 `~maik/efs-artifacts/taskGC-20261111/` with `SHA256SUMS`.

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
| `efs_client_rpc_getchunks_fileid` (`:2688`) | GETCHUNKS | `handler.c:1427` calls `server_raft_host_getchunks` (`raft_host.c:25696`): `host_read_index` and reads, no `host_propose` | drop, `skip = nid`, loop of 16 attempts on another voter | **covered** |
| `query_snapshot_rpc` (`:3848`) | QUERY_SNAPSHOT | `handler.c:1530` calls `server_raft_host_query_snapshot` / `_query_page` (`raft_host.c:13406`, `:13452`): `server_raft_host_query_read` (ReadIndex) and a host-memory page session, no propose | unpinned: drop, `skip = nid`, loop of 16 attempts; pinned (token names the node): returns `EFS_ERR_NET` | **covered unpinned**; pinned left out (no repeat) |
| `efs_client_rpc_readdir_cur` (`:1634`) | READDIR | `handler.c:1367` calls `server_raft_host_readdir` (`raft_host.c:25855`): ReadIndex and reads | drop, returns `EFS_ERR_NET`; the only `goto retry` is for a BUSY reply; the caller (`efs_fuse/getattr.c:493`) turns it into `-EIO` | **left out**: a cut would turn a 30 s hang into an EIO after 6 s, not into a listing |
| `efs_client_rpc_writer_view` (`:1732`) | INODE_WRITER_VIEW | `raft_host.c:25547` (`_reuse`): ReadIndex and reads | drop, returns `EFS_ERR_NET`; its caller (`write.c:1638`) treats any failure as "needs ordinary repull" | **left out**: no repeat on another member |
| `efs_client_rpc_lane_writer_view` (`:2516`) | LANE_WRITER_VIEW | `raft_host.c:25434`: ReadIndex and reads | drop, returns `EFS_ERR_NET` | **left out**: no repeat |
| `efs_client_rpc_publication` (`:2453`) | PUBLICATION, PUBLICATION_STATUS, PUBLICATION_RETIRE | status only (`query_only == 1`) reads (`raft_host.c:25273`); the other two propose | drop, returns `EFS_ERR_NET`; one site carries a read and two mutations (told apart by `query_only`) | **left out**: no repeat on another member, and the site is shared with mutations |
| `rpc_report_receipt_query` (`:3462`) | REPORT_CHUNKS with `EFS_REPORT_F_RECEIPT` | reads the host's receipt table (`request_receipt.c`), runs nothing | sets `efs_conn_set_recv_timeout(conn, 2000)`, drop, `EFS_ERR_NET` | **left out**: the 2 s bound is shorter than the 5 s probe exchange, so the hook would delay the failure to about 6 s (it runs the exchange before it checks the bound); and it belongs to the REPORT resolution (K122) |
| `rpc_send_recv_dual_once` (`:3300`) | REPORT and other dual-voter requests | REPORT proposes | drop, resend | **out of scope** (K122); pinned by `test_k114_read_sites.py report` |
| session command (`:1821`), put ticket (`:2298`), degraded mark (`:2396`), lane bootstrap (`:2577`), session revoke (`:3932`) | RAFT_MKFS, LANE_BOOTSTRAP | these change state on the server | drop, another voter | **out of scope** (mutations without an identity decision) |

`rpc_send_recv_dual_once` and the state-changing sites are not touched. Left-out sites need a repeat inside the caller first (a loop like the getchunks one) before a cut gives an answer instead of an early error: that is a separate change, named in the last section.

## Tests

- Unit (`tests/test_k114_read_sites.py`, fast suite, both sanitizer lanes): the production text of `efs_client_rpc_getchunks_fileid` and `query_snapshot_rpc` is compiled with stubbed transport whose member is silent (every receive fails). `getchunks` and `query` assert the call order `send, hook, receive, drop` twice and that the second attempt skips the failed node; `pinned` asserts the order `send, receive, drop` and no hook call; `report` asserts, on the production text, that `rpc_send_recv_dual_once`, readdir, writer view, lane writer view, publication and the receipt query contain no hook call.
- Mutation proof (run on the Mac, each reverted): removing the hook call in getchunks fails `getchunks` (`order srdsrd want shrdshrd`); removing it in the query site fails `query`; dropping the `!pinned` guard fails `pinned`; adding a hook call to `rpc_send_recv_dual_once` fails `report`.
- Live (`tests/test_k114_read_sites_live.py`, `test-long`): three loopback daemons, a mount on node 1, the hold hook `EFS_FAULT_HOLD_REQUEST_FILE` holding the first request of the type inside the member, SIGSTOP, wait for the client's `node-waiter-cut` line, SIGCONT, assert the answer equals the answer before the stop and the time is under 20 s. `getchunks`: the file is read through a fresh mount (no cached chunk map); the member held is the leader, node 3. `query`: `.efs/find` of the mount; the first dual voter, node 1, is held; the daemons run with `EFS_QUERY_REFRESH_MS=2000` and the test waits for the `query-publish ... files=N` log line of every member so that all catalogs hold the file before the baseline answer is taken (without that wait the answer differs between members for reasons of the catalog refresh, not of the cut: seen in one of six early runs).
- Live times on intel2 (final tree): getchunks 6.15 s (three solo runs 11.7 to 12.1 s whole), query 6.11 to 6.13 s in eight consecutive runs (whole run 11 to 14 s).
- Not live: readdir, writer view, lane writer view, publication status, receipt query (left out, nothing to test); the pinned query (no user operation reaches a pinned first request deterministically; unit test only).

## What K114 still leaves open

- Mutations without an identity: chmod, chown, utimens, XATTR, HOLD, FLOCK (a delayed repeat can undo a later change).
- REPORT: stays out until K122 is settled; the late-copy exposure for the truncate that gave up is K121.
- Left-out reads: readdir, writer view, lane writer view, publication status and the pinned query page wait 30 s on a stopped member. Each needs a repeat on another member in its own loop first. The receipt query needs a probe bound that fits its 2 s receive.
- The sites whose idempotence on the server was not read (put ticket, degraded mark, lane bootstrap, session command, revoke), the PUT, GET and data-commit receives (own deadlines).
- RDMA connections (the hook returns at once) and a dead or partitioned host (the probe's connect fails, the verdict is not silent).
