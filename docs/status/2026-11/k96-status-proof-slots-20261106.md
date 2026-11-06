# K96: forwarded ticket STATUS held the read-proof slots, a cluster-wide BUSY stall (Task DQ)

Task DQ, Nov 6 2026, node9900. Purpose: find and fix the periodic cluster-wide BUSY stall that Task DK's `efs-bench` measurement showed ([Task DK](k88-hint-per-chunk-20261105.md), "Follow-up, not K88"): about one round in 12, every concurrent writer failed at once with `bench worker=N ... rc=-13` after 31 to 41 s. Linked row: K96 (both tables of the [checklist](../alpha-1.0-2plus1-checklist.md)). Rule kept: fix code; no timeout, budget or sleep changed and no per-class connection cap raised (the pool gains a 2-connection class, see [The fix](#the-fix)).

**Verdict: PASS. K96 FIXED, blocks alpha: no** (see [Blocks alpha](#blocks-alpha)). Same reproducer, same cluster shape, the two metadata groups' leaders on different nodes: at 8 concurrent writers 56 of 960 runs failed in 8 of 120 rounds before and 0 of 960 after; at 24 writers 238 of 240 before and 0 of 240 after. The daemons answered 67,636 and 152,893 PUTs with an error before, none after. New live test `tests/test_k96_status_slots_live.py` fails 72 of 72 copies before and passes after.

## What stalls, where, and why for 30 s

Every fragment PUT (`src/server/put_upload.c`, `put_upload`) asks the ticket's lane leader for `PUT_TICKET_STATUS` before it stores. A storage member that is not that leader forwards the request (`host_remote_cmd`, read-only, one 400 ms budget, `HOST_READ_TRIES`). W166 (`878c00e96`) routed this forward onto the peer pool's two *read-proof* slots (`EFS_PEER_READ_CONNS`, `src/server/peer_pool.c`), to keep it off the 32 mutation slots. The read-proof slots were introduced in `eb2fad1b7` for "at most one coalesced remote proof per hosted metadata group", that is, for leaf ReadIndex requests.

STATUS is not a leaf. The lane leader serving it runs `host_session_establish_admit`, which takes a ReadIndex of the client's *session* group. When the two metadata groups (g0, g2) are led by different nodes, that ReadIndex is remote: the coalesced `host_remote_read_index` sends it over the same two read-proof slots, in the reverse direction. The cycle, with the roles of the reproduced cluster (n3 leads g0, n2 leads g2, n1 leads neither):

1. n2's PUT handlers forward STATUS for g0 tickets to n3 and hold n2→n3's two proof slots, waiting for n3's replies.
2. n3, serving them, needs a g2 session proof from n2. Its one coalesced proof waits in `peer_conn_get` for an n3→n2 proof slot.
3. Those slots are held by n3's own STATUS forwards for g2 tickets, waiting for n2, whose handlers wait for an n2→n3 proof slot (step 1).
4. Each hop gives up at its 400 ms read budget: the forward returns BUSY, `put_upload` answers BUSY, and queued STATUS forwards (14 per node in the capture) take the freed slots before the proof does, so the cycle closes again at once.

On the client, `put_fragments_parallel_run` (`src/client/write.c`) retries a BUSY fragment every 50 ms within the whole-operation deadline that `put_pool_run` sets to 30 s (`report_clock_ms() + 30000`), then returns `EFS_ERR_BUSY`. The writers' retries keep the cycle loaded, so the stall lasts until all of them give up: 30 s plus the in-flight tail, the 31 to 41 s `wall_s` of every failed copy. Then the load is gone and the cluster recovers by itself. A degraded acknowledgement (`why=budget`) is the same budget running out after two of three fragments were stored.

Evidence from the capture (node9900, 250-round diagnostic run of the unfixed tree with daemon logs kept and gdb stack samples taken when a copy ran 6 s or more):

- Daemon `iostats` during a failed round (06:09:52 to 06:10:21Z): on all three nodes, PUT `p50_us` 400,000 and every PUT an error, `wdisk` ops flat; no election, Raft kept committing (the session group index advanced by 450 in the window). Outside the long windows, `p50_us` near 400,000 recurs for 5 s intervals: the same cycle, broken before the 30 s client budget.
- Stack samples 8 s apart (01:12:33 and 01:12:41 local, round 34, which failed): on n1 14 handler threads in `peer_conn_get` ← `server_peer_read_conn_get` ← `host_rpc_submit` ← `put_upload`, 2 to 4 holding a proof slot in `efs_conn_recv_msg`; on n3 (g0 leader) 20 to 27 handlers serving forwarded STATUS in `host_remote_read_index` ← `host_session_establish_admit`, and the one coalesced proof in `peer_conn_get`; on n2 (g2 leader) the mirror image, 11 to 12 handlers. Same populations in both samples.

Not the cause: Raft (no election; commits continued), disk, quota, K88 (`report-stuck` 0 in every run).

## The fix

`src/server/peer_pool.c` gets a third slot class, `EFS_PEER_STATUS_CONNS` (2), for forwarded STATUS; the two proof slots carry only leaf ReadIndex (an empty command the leader answers locally, never forwarding on). `host_rpc_submit` (`src/server/raft_host.c`) chooses:

```c
pc = !clen ? server_peer_read_conn_get(host, port) :
     host_submit_read_only(cmd,clen) ? server_peer_status_conn_get(host, port) :
                                       server_peer_conn_get(host, port);
```

A proof holder never waits for another slot, so no wait cycle can pass through the proof slots. STATUS still stays off the mutation slots (W166's reason) and keeps its 400 ms budget. Each class wakes only its own waiters. Timeouts, budgets, retry pauses, the proof-slot count (2) and the mutation-slot count (32) are as before. The pool grows from 34 to 36 connections per peer: STATUS could hold at most 2 before (the proof slots) and holds at most 2 now (its own). The cycle is removed by the separation, not by the capacity; a STATUS holder only ever waits for a leaf proof. The zero-connection alternative, STATUS back on the 32 mutation slots, is also free of this cycle, but re-exposes what W166 moved it away from (STATUS queued behind mutations that themselves need metadata authority); this fix keeps W166's isolation at 2 more connections per peer. Commit `d8331e756`; no wire, store or log format change.

A leader that does not host the session's owner group sends a session GET instead (`host_remote_cmd` with a command, mutation slots, 30 s). Its service is local on the owner's leader, so it is a chain, not a cycle. In three-node clusters every member hosts both groups and the GET path is not taken.

K94 (forwarded read proofs reaching `peer_connect` with their 400 ms spent) may share a cause: proofs that queued behind STATUS forwards for the proof slots spent their budget in `peer_conn_get`. Not measured here; K94 stays OPEN.

## Tests

- `tests/test_host_read_deadline.py` (fast suite): the real `host_rpc_submit` sends an empty proof on a proof slot, STATUS on a STATUS slot, a mutation on a mutation slot. On the unfixed source the STATUS case aborts (`status_gets==1 && !read_gets`).
- `tests/test_peer_pool_deadline.py` (fast suite, the real `peer_pool.c`): with every mutation and STATUS slot held, a proof still checks out; a STATUS checkout never takes a proof slot; releases wake only their class. The unfixed source does not compile against it (no slot classes).
- `tests/test_k96_status_slots_live.py` (`test-long`): three private production `efsd`, TCP, 3 rounds of 24 concurrent `efs-bench --size 8M --io-size 64K --workers 4`; pass when every copy exits 0 and no worker reports `rc=-13`. Before (unfixed tree): 24 of 24 copies failed in each of the 3 rounds, slowest copy 42.5 to 89.7 s, 199 s in all. After: 0 of 72, slowest copy 0.8 to 1.0 s, 5.9 s in the test (7 s wall). The test places the leaders itself (`place_leaders`: g0 on node 1, g2 on node 2).

## Before and after (`efs-bench`, same script)

`scripts/evidence/k88/bench_chunk_hints.py OUT PAR ROUNDS 8M 64K 4`, one private three-node production cluster per arm on node9900 (TCP loopback, 8 GiB quota per store, `--writers 2`), each copy `efs-bench --size 8M --io-size 64K --workers 4`. 120 rounds at 8 copies stay below the quota; `split` places the leaders first (see below). PUT counts are the three daemons' cumulative `iostats` at the end of the arm.

| Arm (`PAR ROUNDS`, leaders split) | Tree | Runs failed | Rounds with a failure | Failed copy `wall_s` | Worker `rc=-13` lines | Mean run | Arm wall | PUTs (daemons) | PUT errors |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `8 120` | unfixed | 56 of 960 | 8 (rounds 25, 29, 34, 66, 68, 70, 103, 114) | 31.3 to 46.4 s | 130 | 6.73 s | 834 s | 425,038 | 67,636 |
| `8 120` | fixed | 0 of 960 | 0 | none | 0 | 0.38 s | 62 s | 343,298 | 0 |
| `24 10` | unfixed | 238 of 240 | 10 of 10 | 51.9 to 76.0 s | 820 | 64.3 s | 653 s | 177,595 | 152,893 |
| `24 10` | fixed | 0 of 240 | 0 | none | 0 | 0.87 s | 17 s | 35,684 | 0 |

Every failed run ended `BENCH_FAIL ... rc=-1` with its workers at `rc=-13`; 26 of the 56 failed runs at 8 copies also logged a `degraded acknowledgement ... why=budget`. No run logged `report-stuck`. The leaders stayed where they were placed for every arm (the script prints the placement at start and end).

Leader placement decides it. Left to the election, both groups' leaders formed on node 1 in two of the runs here, and the unfixed tree then had no stall: 0 failed rounds in 40 (`8 40`) and in 132 (`8 250`, stopped at round 132 to rerun with split leaders). With split leaders the unfixed tree failed 16 of its first 134 rounds in a longer run, the same rate as above; that run was stopped once the stores passed the 8 GiB quota (8.8 to 8.9 GiB each at about round 135, after which rounds slowed to about 20 s on quota backpressure), which is why the comparison uses 120 rounds. Task DK's daemon logs were not kept, so its leader placement is unknown; its 1 round in 12 matches the split-leader rate here. With both leaders on one node the unfixed tree still answered 21 % of PUTs with an error (112,696 of 523,789 in the 132 rounds), likely STATUS forwards from the other two members taking the proof slots their own session proofs needed (not traced). That costs retries, not a 30 s stall; this placement was not measured on the fixed tree (the one attempt, `8 40 ... any`, got split leaders).

## Checks on the merged tree

Merged branch (devel `f94d64e60` + this task), built on node9900 with `make -j48 all test-bins tests/faults/efsd` (`efsd` `ef13cfceeb0f6e19`, `efs-bench` `a6073e4250e182f3`):

- `tests/test_host_read_deadline.py`, `tests/test_peer_pool_deadline.py`: PASS.
- `tests/test_w166_authority.py` (W166's own case: a lagging member delegates the whole STATUS read to the lane leader): PASS, 4.59 s.
- `tests/test_k96_status_slots_live.py`, three solo runs at load below 2: 7.0, 7.2, 7.1 s wall (5.4 to 5.6 s in the test), PASS. Budget row 10 s, 2 slots (1.25 × 7.1 s rounded up to 5 s); `tests/test_make_test_recipe.py` PASS, budgeted gate 1304 s of 1750 s.
- `make test` (fast suite): PASS, 111 s.
- TSan (`EFS_SAN_ONLY=... bash tests/sanitizer_check.sh tsan`, merged tree): `k88_chunk_hints_live` PASS (6 s; production daemons, library writer, so its fragment PUTs take the STATUS forward route through the new slot class). `w166_authority` is not in the TSan list. The K96 live test run once under TSan (`K96_ROUNDS=1`, a trial entry not kept in the list) reported 2 races, both in the client, not in this change: `rpc_resolve_ms` (`src/client/inode_rpc.c:142`) initialises a function-static `long` without synchronisation, and four `efs-bench` workers creating their files at once read and write it (the pattern K83 fixed for `rpc_prof_enabled`). Filed below as a follow-up.
- Fixed tree, leaders left to the election, `8 40 8M 64K 4 any`: the election split them this time (g0 node 1, g2 node 2); 0 of 320 runs failed, 0 PUT errors.
- 8-job gate (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`): `TOTAL 592 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (617s)`, no over-budget row; the four NOTRUN are this host's missing tooling (no `~/git/cluster` scripts, no valgrind for the RDMA Memcheck and leak gates, no RDMA hardware run in `test_qualify_rdma_live`). In the gate: `test_k96_status_slots_live` 7.3 s (budget 10 s), `test_w166_authority` 5.5 s, both unit cases 0.1 s.

## Source, build, environment

- Unfixed tree: `3fadcdb7c` (Task DK tip, `claude/interesting-wescoff-8ab604`) plus the reproducer changes `cfb4917b9` (keeps daemon logs, adds start times) and `a7d903e5e` (optional `split` leader placement); no product code. Fixed tree: the same plus `d8331e756`. The measurement trees were built from these sources on node9900 (the reproducer change was copied in; product source identical to the commits). Branch `claude/infallible-goodall-edd2b1` was then merged with devel (`f94d64e60`, wave 59, which contains Task DK), which changed other parts of `raft_host.c` and `server_internal.h`; the fix's hunks merged unchanged. Checks on the merged tree are above.
- Build on node9900: `make -j48 efsd efs-mgmt efs-bench`, gcc 11.5.0 `-O3 -march=native`, `build=unknown`. sha256 prefixes: unfixed `efsd` `bba210ba0f3c92cd`, `efs-bench` `e4dfde48fbffe62f`; fixed `efsd` `7a6552112fa8d125`, `efs-bench` `d5230fea87a186c0`.
- node9900: AMD EPYC 9474F (192 CPUs), Linux 5.14.0-687.39.1.el9_8. Clusters on free loopback ports chosen by `tests/k33_daemons.py`, stores under `/tmp/efs-taskDQ/tmp`; GC on, 128 KiB chunks. No live cluster port or `/data1/efs` path was touched. Only this task ran on node9900 during the measurement.

## Reproduction

```sh
export TMPDIR=/tmp/efs-taskDQ/tmp; mkdir -p "$TMPDIR"
make -j32 efsd efs-mgmt efs-bench
python3 tests/test_host_read_deadline.py && python3 tests/test_peer_pool_deadline.py
python3 tests/test_k96_status_slots_live.py                         # 3 rounds x 24 writers
python3 scripts/evidence/k88/bench_chunk_hints.py OUT 8 120 8M 64K 4 split    # OUT/summary.txt, runs.tsv, n1-n3.log
python3 scripts/evidence/k88/bench_chunk_hints.py OUT24 24 10 8M 64K 4 split
# before: build d8331e756^ (or src/server/{peer_pool.c,raft_host.c,server_internal.h} from 3fadcdb7c)
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh               # gate, about 12 min
rm -rf /tmp/efs-taskDQ
```

To see the cycle on the unfixed tree: while a round has run 6 s or more, `gdb -p <efsd pid> -batch -ex 'thread apply all bt'` on each daemon; the `peer_conn_get` ← `server_peer_read_conn_get` ← `put_upload` and `host_remote_read_index` ← `host_session_establish_admit` populations above appear on all three. The two groups' leaders must be on different nodes (`raft: group=N became leader` in the daemon logs); with both on one node there is no remote session proof and no cycle.

## Blocks alpha

No, as FIXED. The defect itself would block alpha: with no node down and no election, every writer of the cluster failed for 30 s under 8 concurrent library clients (32 writer threads), and nearly always at 24. It is a data-path availability failure, fail-closed (the writes were refused, nothing acknowledged was lost). Acceptance: the before/after measurement here and the live test in `test-long`.

## Limits and remaining gates

- Loopback clusters on one host, TCP only; no RDMA, no orcd-mini or orcd-test run. The FUSE client takes the same PUT path; it was not measured under this load.
- Larger clusters, where a leader may not host the session owner's group, take the session GET chain above; not measured.
- K94 not re-measured.
- Follow-up, not K96: the TSan race in the client's `rpc_resolve_ms` (see Checks). Once it is fixed, `k96_status_slots_live|K96_ROUNDS=1 tests/test_k96_status_slots_live.py` can join the TSan daemon list. Done as [K97](k97-resolve-ms-race-20261106.md): fixed, and the entry is in the TSan list.
- Both leaders on one node was not measured on the fixed tree (the election split them in the one attempt).
- `docs/status/README.md`: no change; the K register row stays OPEN overall.

Raw output stays outside Git: `~/efs-artifacts/taskDQ-20261106/` (shared `erbmi1` home on the ORCD hosts). `SHA256SUMS` has sha256 `55aaf596aba958d92d24b136cc16479acf12f43d1a80e354eb9aba506c659091`. It holds:

- `node9900-diagnosis.tar.gz` (`3d9bafc2...`): the first runs (`8 40` and the gdb-sampled `8 250` stopped at round 108, both with daemon logs), the 60 stack samples, the stopped colocated-leader and quota-limited arms, and the watcher and stack-summary scripts.
- `node9900-measurement.tar.gz` (`5096293b...`): the four measured arms (`runs.tsv`, `summary.txt`, failed-run logs, the three daemon logs each), the two live-test logs, and the driver script.
- `node9900-merged-checks.tar.gz` (`f0897603...`): unit, W166, live-timing, fast-suite and gate logs, the gate's `timings.tsv` and `summary.txt`.
- `node9900-extra.tar.gz` (`19e97868...`): the fixed tree's `8 40 ... any` run and the TSan runs (`k88_chunk_hints_live`, the trial K96 entry with its two reports).
- `bench_chunk_hints.py` and the live test as run, then named `test_dq_status_slots_live.py` (env `DQ_ROUNDS`, `DQ_KEEP`). It was renamed to `test_k96_status_slots_live.py` after the runs; only the file, environment and cluster names and the printed prefix changed.

The gdb samples paused the daemons briefly, so the stack-sampled run is diagnosis only and is not counted in the before/after table.
