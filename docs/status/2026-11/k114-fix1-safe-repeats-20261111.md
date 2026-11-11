# K114 fix 1: the safe repeats join the K106 (a) cut (Task FZ)

**Status (Nov 11):** all four kinds are in the cut. The wait of a truncate SETATTR, an APPEND, a CREATE_RETAINED with probe=0 and a REPORT with a publication id on a stopped (SIGSTOP) member now ends at the exchange verdict: the operation returned in about 6.1 s in every live run, against 30.00 s for the same REPORT on the devel client (control run). For each kind a test shows that the same identity sent twice has one effect in the three orders the false cut can produce, and a live test shows that the first copy, run late after the member is resumed, undoes nothing. chmod, chown, utimens, XATTR, HOLD and FLOCK stay uncut (the `gating` scenario pins all 256 request types and the body cases, and fails when chmod or XATTR is made cut). One test hook was added (`EFS_FAULT_HOLD_REQUEST_FILE`, EFS_FAULTS builds only). Row K114 stays OPEN (this task does not edit `docs/status/k/`): what it still leaves open is in the last section. Not yet run: the full `make test-long` gate; the new live rows have a budget but no gate has run them.

Source commit: base `29644e61d` (devel), branch `claude/task-fz-k114-fix1-6de5c6`; the final commit is the head of that branch. Design input: [K114 design page](k114-uncovered-waits-design-20261111.md), [K106 (a) fix page](k106a-inflight-wait-20261110.md) (Round 2). Raw output: intel1 `~maik/efs-artifacts/taskFZ-20261111/` with `SHA256SUMS`.

## What changed

| file | change |
|---|---|
| `src/client/inode_rpc.c` `rpc_reply_wait_cut` (`:901`) | also yes for: SETATTR whose length is the struct plus the op-id suffix and whose mask is exactly SIZE or SIZE\|MTIME (the server takes SIZE only as a truncate and answers INVAL to any other mix, `raft_host.c` `server_raft_host_setattr`; SIZE\|MODE, SIZE\|ATIME and the rest are pinned as not cut); APPEND with the ack suffix (`sizeof(struct efs_msg_inode_append) + EFS_APPEND_ACK_LEN`, the only form the server treats as a receipt-bearing reserve or abandon); CREATE_RETAINED of exactly the struct length (probe=0 is not a read; probe=1 was already cut as a read); REPORT_CHUNKS with `EFS_REPORT_F_PUBID` in `sync` |
| `src/client/inode_rpc.c` `rpc_send_recv_dual_once` (`:3299`) | calls `efs_client_conn_await_reply` just before the blocking receive (after `efs_conn_set_recv_timeout`, which the hook reads), for a REPORT_CHUNKS the predicate accepts only; the other dual requests keep waiting as before |
| `src/server/handler.c` | test hook `EFS_FAULT_HOLD_REQUEST_FILE` (EFS_FAULTS builds): holds the first request of a message type (optionally with a given body byte) before dispatch until the test opens a fifo (blocking open and read, no timer); logs `holds`, `released` and, after the dispatch, `dispatched`. Row in `docs/how-it-works/settings.md` (and the generated `include/efs/settings.h`) |
| tests | `tests/test_meta_apply.c` `test_k114_truncate_identity`; `tests/test_append_transaction.c` `k114_append_identity`; `tests/test_node_waiter_cut.py` `gating`; new `tests/test_k114_false_cut.py` (4 scenarios) |
| splice stubs | `tests/test_w155_report_resolve.py` splices `rpc_send_recv_dual_once`; it needs the same two stub lines (`efs_client_conn_await_reply`, `rpc_reply_wait_cut`) the other three splicing tests got in K106 (a) round 2 |
| registration | fast suite (`make test`): `./tests/test_meta_apply test_k114_truncate_identity` and `./tests/test_append_transaction k114` (the binaries run in `test-long` as a whole, so these narrow rows put the new cases in the fast suite); `Makefile` test-long: 4 live rows after K42; `tests/budgets.tsv`: 6 rows; `tests/sanitizer_check.sh`: `append_transaction` in the ASan and TSan lists (`test_meta_apply`, which contains the truncate case, was in both already) |

## Per kind

For every kind: (i) second copy after the first was applied; (ii) the two copies in the log with another client's change between them; (iii) the false-cut order: the repeat applied first, the original copy last (also after the client acknowledged the identity and went on).

### SETATTR truncate: cut

- Identity: the client builds an op-id for every SETATTR (`efs_client_rpc_setattr`, `inode_rpc.c:2890`); the truncate receipt is keyed by (uuid, epoch, seq) and probed before the preconditions (`src/meta/meta_apply/append.c:352-368`, `truncate_receipt_load`). A receipt retired by the client's ack answers STALE ("acknowledged and reclaimed").
- Tests, all new, `tests/test_meta_apply.c` `test_k114_truncate_identity`: (i) the repeat answers the first result (`EFS_OK`, epoch 1) and leaves epoch, base size and history revision as they were (the existing `test_truncate_lazy` shows the same for a stale-precondition retry); (ii) client B truncates to epoch 2 between the copies, the repeat of A's first truncate still answers epoch 1 and B's size and epoch stand; (iii) the repeat of seq 2 is applied first (epoch 3), the late original built from the older row answers epoch 3 while unacknowledged, and after seq 3 acknowledged seq 2 it is refused with `EFS_ERR_STALE` and the file is unchanged (epoch 4, size of seq 3).
- Answer the client gets: `EFS_OK` with the current inode; `efs_client_rpc_setattr` returns success on `EFS_INODE_RPC_OK` and maps anything else with `rpc_status_to_efs`.
- Live (`test_k114_false_cut.py truncate`): the first SETATTR (mask SIZE\|MTIME) is held on node 3, node 3 is stopped, the cut line appears, the member is resumed. `truncate` returned in 6.06, 6.07, 6.06 s; then 10000 bytes were written after the new end; after the held first copy ran, size 60000 and content `A*50000 + Z*10000` through the mount on node 1 and through one on node 3.

### APPEND: cut

- Identity: reserve and abandon carry uuid, epoch, seq and ack (`efs_client_rpc_append_reserve`, `:2983`, `efs_client_rpc_append_abandon`, `:3042`). The leader probes `efs_meta_append_result` before it proposes (`raft_host.c:20337`), and `efs_meta_capture_append` probes it again (`append.c:629`, `append_receipt_load` `:248`, result `:280`); the abandon retires the receipt (`efs_meta_abandon_append`, `raft_host.c:20378`).
- Tests: new `tests/test_append_transaction.c` `k114_append_identity`: (iii) two plans of one reservation from the same state, the repeat executes, the stale original plan fails and changes nothing (watermark 208, one OPEN); (i) the receipt answers offset 200; (ii) the first reservation completes, another client appends (offset 208), the repeat still answers offset 200 and the watermark stays 216; the same identity with another length is `EFS_ERR_INVAL`; (iii, after the ack) seq 2 acknowledges seq 1, the late original is `EFS_ERR_STALE`; abandon: first copy retires, the repeat answers OK with `retired=0`, a late reserve copy is `EFS_ERR_STALE`. Existing: `receipt_capture_race(0)` (`:147`) shows a peer executing the same identity between the first receipt miss and the capture.
- Answer the client gets: the repeat returns `EFS_OK` with the first offset in `inode.atime` and the end in `inode.size`, which `efs_client_rpc_append_reserve` copies to `start_out` and `new_size_out`.
- Live (`append`): 6.09, 6.08, 6.07 s; `B*1000` then an O_APPEND write of `C*777` under the stop, a further append of `D*100`, then the held first copy released: content `B*1000 + C*777 + D*100`, size 1877, same through the mount on the resumed member.

### CREATE_RETAINED, probe=0: cut

- Identity: the full op-id with ack in the request (`efs_client_create_begin`, `inode_rpc.c:1457`); `host_retained_create_proof` (`raft_host.c:18869`) answers a recorded verdict before a submission, `host_create` re-probes, apply probes the directory op-id window first (`src/meta/meta_apply/create.c:155-163`; a retired success is STALE, `:160`).
- Tests (existing, `tests/test_meta_apply.c` `test_create_initial_lease_atomic`, run for `pending=1`, the retained form, `:6468`): (i) `:6533` "CREATE replay returns original inode after unlink" (no new batch, the original inode, no lease recreated); (ii) `:6540` another request reused the name, the replay still answers the original inode and keeps the replacement; (iii) `:6550-6553` a later create acknowledges the old one and the late original is `EFS_ERR_STALE` and "cannot grant unknown adoption". No new test was written: the same identity is the same apply call in any order.
- Answer the client gets: `known=1` with the recorded inode and generation, `create_request_rpc` completes the request (`inode_rpc.c:1485-1515`).
- Live (`create`): 6.08, 6.12, 6.12 s (the held request is the probe=0 submit, selected by the `probe` byte); one name `new` with `N*500`; the client unlinked it and created `new` again (`M*5`, another inode); after the held first copy ran the directory lists one name, `M*5`, through both mounts.

### REPORT with a publication id: cut

- Identity: session plus publication id (mint, seq) built at `inode_rpc.c:3700-3740` (`efs_report_pubid_pack` at `:3730`); the host answers a repeat from the receipt without running it (`src/common/request_receipt.c:186-229`) and apply holds the durable verdict (`src/meta/meta_apply/publish.c`, `efs_meta_apply_publish` `:742`: a recorded verdict is returned before `publish_inner` runs). The size record of a REPORT is not a separate effect: `host_pub_pack` puts it into each publication as `new_size` (`raft_host.c`, the loop at `:25263`), the digest covers it (`test_publication_recovery.c:62` `changed.new_size++` is `EFS_ERR_PROTO`), and a replay returns the recorded verdict without applying the lane EOF again, so a late copy cannot move the size or the grow-only lane maximum. The time stamps (`now`, `mtime_gen`) are outside the digest (`:74-77`) and are not re-applied on a replay either.
- Tests (existing, `tests/test_publication_recovery.c` `exercise`, `:37-66`): (i) `:49-50` the same publication again returns OK and the receipt locator stays the first one; (ii) `:51-58` a loser and a newer write (generation 12) land in between, then the first publication again returns OK and the mapping stays generation 12 ("reply lost, object superseded"); (iii) `:66` after a content fence the first publication is not resurrected, and `retirement` (`:124`) shows a retired identity answers STALE. No new test was written.
- Answer the client gets: `EFS_INODE_RPC_OK` (`rpc_send_recv_dual_once` returns `EFS_OK`, the caller maps the status); a verdict recorded as STALE is returned as the same STALE.
- Path of a cut REPORT: the hook shuts the waiting connection down, so `efs_conn_recv_msg` fails and `rpc_send_recv_dual_once` takes its receive-failure branch (`inode_rpc.c:3315-3331`, the log line `retry type=67 ... why=recv` at `:3323`, then `continue` at `:3331`): the connection is dropped, `rpc_tl_lost` counted, the same request bytes go to the next dual voter that is not marked down, exactly as a receive timeout does today. Only when the loop returns an error (deadline or attempt cap) does the caller enter `efs_client_rpc_report_resolve` (`inode_rpc.c:3481`, called from `src/client/write.c:2639` with the `rpc_tl_sent` bitmap, which includes the cut member): the same-identity receipt lookup. The cut changes when the first resend happens, not where it goes.
- Live (`report`): the first REPORT reaches node 1 (first dual voter), node 1 is stopped; about 6.1 s; a 300000-byte file reads back whole; it is then overwritten with a shorter file (120000 bytes, other content); the held first REPORT is released and forwarded to the leader; size, mtime and bytes read through fresh mounts are the same before and after it ran, and equal the shorter file.

## Live proof

`tests/test_k114_false_cut.py truncate|append|create|report` (test-long, no argument runs all four). Three loopback daemons (`tests/k33_daemons.py`, EFS_FAULTS `efsd`), tcp, both groups led by node 3, one efs-fuse mount (node 1; node 2 for the REPORT, whose first hop is node 1). Steps: arm `EFS_FAULT_HOLD_REQUEST_FILE`; start the operation in a thread; wait for the daemon line `holds type=`; SIGSTOP the member; wait for the client's `node-waiter-cut: node=N` line; SIGCONT; the operation returns; check it; do a later operation that a repeated first copy would undo; move both groups' leadership back to the resumed member (otherwise the deposed leader would refuse the late copy by routing and the test would not reach the identity; not for the REPORT, which the node forwards to the leader); take size, mtime and bytes through a fresh mount; release the fifo and wait for the daemon's `dispatched type=` line; compare through a second fresh mount on the resumed member. No sleep: the waits are the harness's `wait_log`, a blocking fifo and the leader transfer. What the late copy met (kept logs, `late-copy-logs.tgz`): create: `opid-receipt: replay ... rc=0 ino=0` and `opid-replay ... (stub)`, the retired receipt, which the client had acknowledged by then (apply answers STALE); truncate, append and report: dispatched 1 to 3 ms after the release with no further effect and no log line of their own (the handlers log nothing for a refused or replayed copy).

| scenario | solo wall (3 runs) | operation | control |
|---|---|---|---|
| truncate | 11.5, 12.4, 12.2 s | 6.06, 6.07, 6.06 s | not run |
| append | 10.8, 12.4, 11.2 s | 6.09, 6.08, 6.07 s | not run |
| create | 11.1, 11.8, 11.7 s | 6.08, 6.12, 6.12 s | not run |
| report | 10.8, 11.9, 12.9 s | 6.10, 6.09, 6.09 s | devel client: the receive waits 30004 ms (`cut=bound`), the test fails at the missing cut line |

Budgets: 1.25 times the slowest of the three runs, rounded up to 5 s: 20, 20, 15, 20 s.

## Mutation proof (gating)

Making chmod cut (`mask & (SIZE|MODE)`) fails `gating` (`setattr mask 1 cut=1`); making XATTR cut fails it (`!rpc_reply_wait_cut(EFS_MSG_XATTR, ...)`). Both reverted; outputs `mutation-chmod.txt`, `mutation-xattr.txt`.

## Verification (intel1, gcc 14, tree of the commit)

Written in the commit that follows (the run on the tree of the first commit).

## What K114 still leaves open

- Kinds that need an identity first: chmod, chown, utimens (the wire carries the op-id but apply never records the seq), XATTR set and remove, HOLD, FLOCK non-blocking and unlock (session only).
- Blocking FLOCK (row K120), RDMA connections (the hook returns at once for them), a powered-off or partitioned host (the exchange verdict is "failed", not "silent"), the other receive sites of the design page section 3 (readdir, writer view, getchunks, query snapshot, receipt query, session commands, put ticket, degraded mark, publication, lane bootstrap, data commit).
- Other dual-path requests (CREATE through `rpc_send_recv_dual`, line 1393) keep waiting.

## Limitations

- The live scenarios hold the first copy before it is dispatched, so the first copy has not executed before the stop. The case "the first copy was already committed and only the reply is lost" is covered by the apply-level tests (order (i)) and not by a live run. The late copy's reply goes to a closed connection, so the test does not see its answer; for create the log shows the retired receipt, for the other three only that it was dispatched and that size, mtime and bytes did not move. The barrier is the dispatch line, which comes after the handler returned.
- Truncate (ii) and (iii) are shown at apply with the preconditions modelled by the test; the host builds them from the row it reads, which `k114_trunc` imitates but does not call.
- The `EFS_FAULT_HOLD_REQUEST_FILE` hook is new test code in the product tree (EFS_FAULTS builds only); it holds one request per file.
- CREATE_RETAINED and REPORT orders are shown by existing tests that were written for other purposes; their cases name the order but their text does not say K114.
- Full gate (`make test-long`) and distinct-host runs are not done; one machine, three members, tcp only.
