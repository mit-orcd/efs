# K104: a 30 s stall and `EBUSY` with one replica halted (Task FF)

Task FF, Nov 10 2026, node9900 and intel1 (private clusters only). Purpose: find why `tests/test_ao_readdir_live.py` stalled about 30 s and its churn writer's create/unlink returned `EBUSY` (errno 16) right after one replica was halted with SIGSTOP and continued (wave 70 merge gate, node9900), fix the root cause and prove it. Linked row: K104 (both tables of [the checklist](../alpha-1.0-2plus1-checklist.md)).

**Verdict: cause found in two places, both fixed with regression tests; the unchanged gate test reproduced it once (1 of 96 runs) and a variant of its step that forces the gate's election outcome reproduces it at about 4 % (`pstack` captures from that variant); not introduced by wave 69, which leaves the wave 69 rate change unexplained.** A request sent to a member that has stopped answering (SIGSTOP or a hung host: the kernel still accepts TCP, nothing replies) waits the full 30 s transport timeout (`EFS_IO_TIMEOUT_MS`), which is also the client's operation budget. Two routing defects sent requests there although two live members could serve them:

1. **Client** (`src/client/inode_rpc.c`, the gate's path): the client's per-group leader note and its read routes kept naming the member that led a group earlier. A refusal with a leader hint, the normal answer after a transfer, was followed for that one request and left the note in place, so every later request of the group went to the old leader first. In the test node 3 sometimes wins the first group-2 election; the test then moves leadership away and halts node 3, and the next request of that group (a create's open-lease session command, the listing's LOOKUP/GETATTR) waits 30 s on node 3: the create returns `EBUSY`, and the directory listing waits behind its directory lock. Fix `70330301a`: every refusal the shard, session and ticket loops follow, and every transport failure, drops the read routes and forgets the noted leader when it is the refusing node or disagrees with the hint.
2. **Server** (`src/server/raft_host.c`, `host_remote_cmd_impl`): a replica that forwards a Raft command during an election had no leader in its own view and guessed a peer; with the first peer still electing the next guess was the halted member, and a mutation forward waited 30 s there. A deposed coordinator's DECIDE ABORT (finisher) takes this path, outlives `HOST_FIN_MAX_NS` and is dropped, so the transaction's PREPAREs keep the client's unlink BUSY at the live leader for the client's whole budget. Fix `59075a4a2`: a replica that hosts the group forwards only to the leader its own Raft view names and waits for it inside the existing 400 ms routing budget.

No timeout, budget or bound changed. With node 3 forced to win the first group-2 election (the common factor of the wave 70 failure and every reproduction), the gate step failed 4 of 96 runs on the wave 68 tree, 2 of 48 on devel, 2 of 48 with the server fix alone and 0 of 144 with both fixes, with the same stacks before the client fix on both sides of wave 69. The stranded-transaction trigger, which exercises the server defect, failed 3 of 4 runs before the server fix and 0 of 4 after, and 4 of 4 on the wave 68 and 69 trees. So Task FA's A8 audit is cleared for both defects: K104 is older and became visible.

## Source and environment

- Branch `taskFF-ao-halted-replica` on devel `9403d0666`: `59075a4a2` (server fix, `tests/test_remote_cmd_halted_peer.py`), `4f6ee702a` (splice stub in `tests/test_read_retry_budget.py`), `70330301a` (client fix, `tests/test_client_leader_forget.py`, helper in five splice tests), `be8871512` (helper in `tests/test_create_retry_identity.py`). Before arm: devel `9403d0666`. Wave 68 tip `2094fea52` (before Task FA's audit), wave 69 tree `902a0fb5d` (after it).
- Builds: `make all tests/faults/efsd` from `git archive <commit>` (gcc `-O3`, TCP). intel1 (8 CPUs, 15 GB, Rocky 9, fuse3 3.16.2), trees under `~/efs-taskFF/`, listener ports 18700 to 18799 through `scripts/evidence/ff/port_range.py` (per-slot 12-port range). node9900 (192 CPUs, private libfuse `/data1/efs/deps/libfuse`), shared with other sessions under the three-check quiet rule.
- Cluster in every live run: three `efsd` of the `EFS_FAULTS` build (quota 1 GiB, two writers, discovery off), one `efs-fuse`, directories spread at 64 entries (`EFS_FAULT_DIR_SPREAD_MIN=64`; `EFS_DIR_SPREAD_MIN` on the wave 68 tree, whose name the later builds refuse at startup).

## Fixture of the wave 70 failure (read only)

`~erbmi1/efs-artifacts/merge-wave70-20261110-node9900-aofixture/` (`efs-ao-live-6tbustvl.tar.gz` SHA-256 `d38d3aefe9b8…`, `fail-554.log` `d03f27c5b6d6…`, checked against `SHA256SUMS`): three daemon logs, 14 listings, `result.json`; no client log. Times UTC.

| time | event |
|---|---|
| 05:38:21.597 | node 3 wins the first group-2 election (term 1); node 1 leads group 0 |
| 05:38:22.242 | `place_leaders(c,1)`: group 2 moves from node 3 to node 1 |
| 05:38:22.647–.697 | row-cut listing paused on node 1; groups 0 (22.658) and 2 (22.675) move to node 2; pause released |
| 05:38:22.701 | last client mutation any daemon logs (create receipt replay, op 857, node 2) |
| ~05:38:22.70 | SIGSTOP node 3 (its log is silent after 22.642); listing 11 starts |
| 05:38:23.6–52.8 | node 2's GC pass waits 29.1 s for node 3's reclaim acceptance (`reap_fence=29092`) |
| 05:38:34.6 | nodes 1 and 2: `heartbeat: node 3 … unresponsive — marked down for 30s` |
| 22.7–52.7 | node 1's PUT count stays 282 (273 stable + 9 churn files); no BUSY create/unlink line on node 1 or 2 |
| ~05:38:52.7 | listing 11 returns (written at :52, listing 10 at :22); listings 12–14 immediate; SIGCONT |

So the in-flight operation was a churn create or unlink in `ao-stable`, the wait that ended in `EBUSY` was the client's 30 s operation budget, and the client was waiting for a reply rather than being refused by the live members (they log every BUSY refusal). The intel1 reproductions below show the same picture with the client log and stacks.

## Root cause, with evidence

**Client: requests go to an old leader.** `g_group_leader` (one note per Raft group) was set from OK read replies and session OK hints and cleared only by a hintless refusal from the noted node (`rpc_send_recv_shard_once`) or a transport failure (`rpc_retry_peer`); read routes (`metadata_read_route.h`, the node that last served a key) were reset only when a read reply changed the note. `raft_voter_conn_skip` tries the noted leader first. Captured on intel1 (server fix only, node 3 forced to lead group 2 first, run `diag-fix/r2-s2`, `EBUSY`, 54.5 s): `pstack` of `efs-fuse` 5 s into the first halted listing shows the churn's `efs_fuse_create_at` → `efs_open_acquire_created` → `efs_client_lease_resume` → `efs_client_rpc_lease_edge` → `session_command_reply(shard 3102)` blocked in `recv`, and the listing's `efs_fuse_lookup_at` → LOOKUP of `ao-stable` blocked in `recv`; the client log ends the wait with `inode-rpc: retry type=43 … why=recv rc=-6 nid=3` and `slow-ok type=43 attempts=2 … us=36505797`. Two more captures (`r4-s1`: GETATTR on node 3 for 35.8 s; `r6-s3`: the same `session_command_reply` stack, `EBUSY`) agree. The session and ticket loops log nothing on a lost exchange, which is why the wave 70 client was silent.

**Server: a forward guesses the halted member.** With `EFS_FAULT_DECIDE_LOST` deposing a coordinator before its DECIDE while node 3 is halted: `unlink-txn … rc=-15`, the leader then refuses the client's retries (`unlink-simple … rc=-13`, 41 attempts, `retry-stop type=51 … reason=deadline elapsed_ms=29999`), the finisher's DECIDE ABORT is forwarded by `host_remote_cmd_impl` → `host_pick_peer` (node 1 still electing: NOT_PRIMARY, no hint; next pick node 3) → `host_rpc_submit` waits 30 s → dropped after `HOST_FIN_MAX_NS` without a log line; recovery (`host_txn_recover_pass`) runs on the GC thread after the reap step, which was itself waiting for node 3, and logged `txn-recover … age=31.6s -> ABORT` 1.6 s after the client gave up. After the fix: `txn-finish coord=… abort decided rc=0` 0.85 s after the step-down, unlink done in about 1 s.

**Why the two live members did not serve it.** In the client case they were never asked: the request sat on node 3's socket for the whole budget. In the server case they answered, BUSY, because the transaction's PREPAREs held the keys and the ABORT that releases them sat on node 3's socket.

**Not wave 69.** `git diff 2094fea52 902a0fb5d -- src` changes no value on these paths (`efs_dir_spread_min` is used only by the servers' apply and both trees spread the test directory at 64). The client path fails on the wave 68 tree with the forced election (4 of 96, the same `session_command_reply` and LOOKUP-on-node-3 stacks), and the server path fails the stranded-transaction trigger on both trees. What changed the gate rate at wave 69 (0 slow of 14, then 2 of 7) is not explained: the failure needs node 3 to win group 2's first election and a group-2 request to land in the halt window, and how often that happens under gate load was not measured per wave.

## Before and after

Gate step with node 3 forced to win the first group-2 election (`scripts/evidence/ao/qualify_ao_readdir.py` plus one `k.place_leaders(c,3,groups=(2,))` before the stable files; otherwise unchanged), intel1, eight copies per round on 8 CPUs, `pstack` taken when a halted listing is 5 s overdue:

| tree | runs | `EBUSY` | stalls captured | elapsed (s) |
|---|---|---|---|---|
| wave 68 `2094fea52` | 96 | 4 | 4 (same stacks) | 22.6–56.1 |
| devel `9403d0666` | 48 | 2 | 3 (same stacks; one passed after a 36.8 s LOOKUP on node 3) | 23.9–61.4 |
| server fix `59075a4a2` | 48 | 2 | 3 (one passed after a 35.8 s GETATTR) | to 58.0 |
| both fixes (`70330301a` code) | 144 | 0 | 0 | 23.2–29.1 |

Unchanged `tests/test_ao_readdir_live.py` (no forced election), same host and parallelism; most runs never give node 3 group 2 first, so failures are rare:

| host, tree | runs | `EBUSY` | elapsed (s) |
|---|---|---|---|
| node9900, devel (8 copies, load 5–11) | 32 | 0 | 7.0–9.8 |
| intel1, devel | 48 | 0 | 21.4–27.4 |
| intel1, server fix | 96 | 1 (49.0 s; client log `retry type=51 why=no-hint from=1`, then silent 30 s) | 21.6–49.0 |

Stranded-transaction trigger (`scripts/evidence/ff/halted_strand.py`, intel1, sequential: leaders on node 1, churn, SIGSTOP node 3, `EFS_FAULT_DECIDE_LOST`, 40 s of listings):

| tree | node 3 | runs | `EBUSY` | longest unlink (s) |
|---|---|---|---|---|
| devel `9403d0666` | halted | 4 | 3 | 30.0, 30.0, 25.2, 30.0 |
| server fix `59075a4a2` | halted | 4 | 0 | 1.08, 1.27, 1.10, 1.48 |
| devel / server fix | running | 1 / 1 | 0 | 1.69 / 0.71 |
| wave 68 `2094fea52` | halted | 2 | 2 | 30.0, 30.0 |
| wave 69 `902a0fb5d` | halted | 2 | 2 | 30.0, 30.0 |

node9900 single runs agree (devel 30.0 s `EBUSY`; server fix 1.06 s; wave 68 one `EBUSY` and one 14.7 s stall; wave 69 one `EBUSY`).

Regression tests (fast suite, `tests/budgets.tsv` rows with the 30 s fast-suite ceiling): `tests/test_remote_cmd_halted_peer.py` runs the real `host_remote_cmd_impl` with a 300 ms election and a halted member (0.55 s on a Mac; against devel's `raft_host.c` via `EFS_RAFT_HOST_SOURCE` it fails with `halted_calls=1 … elapsed_ms=30005`). `tests/test_client_leader_forget.py` runs the real leader note, read routes, `rpc_forget_leader` and `raft_voter_conn_skip` (old leader halted after a transfer: no request may reach it) and checks the three retry loops call the helper (0.43 s; against devel's `inode_rpc.c` it fails at the shard loop's hinted refusal).

Gates on node9900 (8 jobs) for the server fix, tree `4f6ee702a`: `make test` 492 commands, 0 FAIL, 3 host-limit NOTRUN, `COMPLETE (28 s)`; `make test-all` 595 commands, 0 FAIL, 4 host-limit NOTRUN, `COMPLETE (594 s)`. The first `make test` on `59075a4a2` failed one splice test (`test_read_retry_budget.py` cuts `host_remote_cmd_impl` and lacked the new helper), fixed in `4f6ee702a`. The client fix (`70330301a`, `be8871512`) has **no gate yet**: node9900 and fstor007 were reserved for other measurements, and intel1's fast suite contains live-daemon tests that would leave its private port range. On intel1 (gcc 14.3.1) every splice test that cuts a changed function passes on the final head: `test_remote_cmd_halted_peer`, `test_client_leader_forget`, `test_read_retry_budget`, `test_lease_edge_rpc`, `test_metadata_retry_budget`, `test_publication_session_rpc`, `test_put_ticket_rpc`, `test_w155_identified_resolve`, `test_create_retry_identity` (failed before `be8871512` added the helper), `test_k32_entry_count`, `test_host_read_deadline`, `test_namespace_leader_route`, `test_metadata_read_route`, `test_getchunks_budget`, `test_identity_revocation`, `test_lease_request`, `test_put_ticket_owner`, `test_revoke_redirect`, `test_session_latch_tsan`, `test_w155_report_resolve`, `test_txn_recovery_priority`.

## Reproduction

```bash
git archive <commit> | ssh <host> 'mkdir -p <dir> && tar -x -C <dir>'
cd <dir> && make -j8 all tests/faults/efsd
python3 tests/test_remote_cmd_halted_peer.py
python3 tests/test_client_leader_forget.py
TMPDIR=<scratch> python3 scripts/evidence/ff/halted_strand.py "$PWD" halt 40 | tail -1     # ROW {...}
TMPDIR=<scratch> python3 scripts/evidence/ff/halted_strand.py "$PWD" nohalt 40 | tail -1   # control
TMPDIR=<scratch> python3 tests/test_ao_readdir_live.py                                      # gate test, loop it
```

The forced-election variant is `scripts/evidence/ff/forced_election.py TREE` (the tree's own driver with `k.place_leaders(c,3,groups=(2,),timeout=15)` inserted before the stable files); `scripts/evidence/ff/forced_loop.sh TREE 8 ROUNDS OUT` runs eight copies per round through `scripts/evidence/ff/port_range.py` (ports 18700+12×slot … +11), records `OUT/runs.tsv`, takes `pstack` of the run's `efs-fuse` and `efsd` once `evidence/listing-0010.json` is 5 s old and `listing-0011.json` is missing, and keeps failed or captured fixtures. On intel1 prefix the other live commands with `python3 scripts/evidence/ff/port_range.py "$PWD" <slot 0-7>`. Cleanup: stop the run's processes by pid, `fusermount3 -u` any mount left, delete the scratch tree.

Raw output, outside Git: intel1 `~maik/efs-artifacts/taskFF-20261110/` (loop tables `*-runs.tsv`, probe rows `strand.tsv` and their output `strand.tsv.raw`, the loop and probe scripts as run, and `fixtures.tar.gz` with each kept fixture's daemon and client logs, listings, `result.json`, `out.txt` and `pstack.txt`; copied stores pruned; wave 68 forced-election table `forced-w68-runs.tsv` and its fixtures `fixtures-w68.tar.gz` `29ca706bbde4…`; `SHA256SUMS` SHA-256 `afa8e548b4d0…`, `fixtures.tar.gz` `f5c3ce7f7467…`); ORCD home `~erbmi1/efs-artifacts/taskFF-20261110/` (node9900 loop and probe rows, gate and sanitizer summaries; `SHA256SUMS` SHA-256 `9afa4d844939…`).

## What is not verified

- The wave 70 gate's own request. Its client log was not kept; the intel1 captures show the client path on the same test step, and the server path needs the same race plus a stranded transaction. Both fixes cover what the fixture allows. The gate rate is re-observed only in later merge gates.
- The client fix needs one refusal of the group between the transfer and the halt: if no request of that group reaches the old leader in that window, the next one still goes there and waits. With the gate's continuous churn this did not happen in 144 runs; it is not excluded.
- Cost of the client fix in steady state: read routes are now dropped on every refusal and the note when a hint disagrees. With the two groups led by different nodes, the per-request routing hops and route churn were not measured.
- Other waits a halted member can still cause, seen here and not changed (follow-ups, outside K104): (a) a request already in flight to a member when it stops still waits its transport timeout (30 s; a LOOKUP has no operation deadline and waited 36 s); (b) every PUT to a halted member reconnects and pays two 5 s protocol-declaration waits (each churn write took 10 s), because the client's background reprobe (`node_probe_run`) only connects TCP, which a stopped process's kernel accepts, and so clears the down mark; (c) GC recovery of stranded transactions runs behind the reap step's member acceptance on the GC thread (29–30 s here).
- Sanitizers: the two regression tests compiled with `-fsanitize=address,undefined` and with `-fsanitize=thread` (gcc 14.3.1, intel1) pass with no report. The `tests/sanitizer_check.sh` TSan and ASan lanes did not run: on node9900 their build picked the system libfuse (`fuse_cmdline_opts` has no `max_threads`) because the script does not set the private libfuse's `PKG_CONFIG_PATH`, and the rerun with it was withdrawn when the host was reserved. Owed: `make test-all`, `make tsan-check` and `make asan-check` (with `PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig`) on the final head.

## Proposed K104 rows (for the coordinator; the tables are not edited here)

First table (description): "K104 | Wave 70 merge gate (Nov 10, node9900, `b4bafedae`): `tests/test_ao_readdir_live.py` stalled 30 s and its churn writer's create/unlink returned `EBUSY` right after one replica was halted with SIGSTOP. Cause (Task FF, page `2026-11/ao-halted-replica-stall-20261110.md`): requests were routed to the halted member although two live members could serve them, and waited the 30 s transport timeout there. Client: after a leader transfer the client's leader note and read routes kept naming the old leader (node 3 when it won group 2's first election), so a create's open-lease command and the listing's LOOKUP went to node 3 (`pstack`, client log `why=recv nid=3`). Server: a replica forwarding a command during an election guessed the halted member, so a deposed coordinator's DECIDE ABORT was lost and its PREPAREs kept the unlink BUSY. Predates wave 69."

Second table (state): "K104 | FIXED, blocks alpha: no (Nov 10, Task FF, `59075a4a2` server, `70330301a` client): forced-election gate step (`scripts/evidence/ff/forced_loop.sh`) 4 of 96 `EBUSY` on wave 68, 2 of 48 on devel and with the server fix alone, 0 of 144 with both; stranded-transaction trigger 3 of 4 before, 0 of 4 after; unit tests `test_remote_cmd_halted_peer.py` and `test_client_leader_forget.py` fail on the old sources; the final head's `make test-all` and sanitizer lanes are owed (the server fix alone passed `make test-all`); VERIFIED needs clean merge gates. Related halted-member waits (a request in flight when the member stops, 10 s per PUT reconnect, GC recovery behind reclaim acceptance) are follow-ups, not this row."

Reason for blocks alpha: no. The cause is in routing code on both sides, reproduced with the gate test and with a deterministic trigger and gone after the fixes; what remains is the transport wait of a request that was already on its way when a member stopped, which no routing change removes and which the follow-ups list.
