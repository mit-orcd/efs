# K94: pooled RDMA peer setup inherited a spent read-proof budget (Task DU)

Task DU, Nov 7 2026, node9900. Purpose: fix K94 (both tables of the [checklist](../alpha-1.0-2plus1-checklist.md)), found by Task DH ([over-budget paths](overbudget-paths-20261105.md), finding 3). On RDMA, forwarded read proofs whose 400 ms budget was already spent failed the RDMA upgrade of new peer connections and broke fresh ones, and `tests/test_k38_shared_chunk_pressure.py` spent 18 to 22 s in its write phase instead of 6 to 7 s. K38 (OPEN, blocks alpha: yes) runs on the same path. Rules kept: no timeout, bound or sleep raised or added. The 400 ms proof budget (`HOST_READ_TRIES` x `HOST_TICK_US`) stays, and the K38 budget row moves down (25 to 20 s).

**Verdict: PASS. K94 FIXED, blocks alpha: no.** Tested code: branch `claude/great-greider-08f8d5` on devel `826d0ef22`, product code `dab77615c`, gates, `make test` and TSan at `0dba77033` (test harness only on top). Binaries are `make` builds of that tree on node9900 (gcc, `-O3 -march=native`, rpath to libfuse 3.16). On DH's tree (`bb8fcdcc4`) the K38 test logged 226 to 364 `segmented RDMA setup failed or incompatible peer` and 1 to 14 `RDMA send type=95 FAILED` in 4 of 6 solo runs. With the final patch applied to that tree: 0 and 0 in 6 of 6. On devel with the fix, 10 solo runs spend 6.1 to 7.1 s in the write phase with 0 setup failures. Three 8-job gates on node9900 at `0dba77033` (same code as `dab77615c` plus a test harness fix): `TOTAL 593 FAIL 0 NOTRUN 4`, no over-budget row, K38 at 12.4/10.8/12.3 s.

## Cause

`peer_conn_get` (`src/server/peer_pool.c`) checked the caller's thread deadline (`efs_net_deadline_ms`) only before `peer_connect`. Connect, protocol declaration and `efs_rdma_client_upgrade` (QP creation, MR registration, one TCP SETUP exchange) then ran on whatever was left of the caller's budget. A forwarded read proof (`host_remote_cmd`, `src/server/raft_host.c`) that reached the pool with a few milliseconds left started a setup it could not finish. Two failure points followed:

1. The SETUP exchange failed on the spent deadline (`segmented RDMA setup failed or incompatible peer`). In `auto` mode the connection stayed TCP (`transport auto: RDMA upgrade ... failed`), although the server may already have answered and published its side, and the spent caller then dropped it.
2. A setup that did complete was broken by its first send. `efs_rdma_send_frame` computed `ms=0`, jumped to `fail:` and set `rc->broken` before posting anything (`RDMA send type=95 FAILED (segmented send failed) ... posted=0`). `host_rpc_submit` then dropped a QP that had never carried a byte.

Each failed setup cost a QP on both sides and left no usable connection, so the next proof connected again. DH's slow runs logged 1,167 to 1,769 `RDMA transport up` lines, against about 500 in fast runs.

On DH's tree, the proofs reached the pool already spent because of K96 (Task DQ, fixed in wave 60, `d8331e756`). Forwarded ticket STATUS held the two read-proof slots, so proofs waited for a slot, and proofs whose leader could not answer within 400 ms were dropped with their reply still in flight. Those reply-timeout drops (`RDMA counters ... data_rx=0/0`) remain on DH's tree with this patch, 44 and 60 in its two slow runs (216 to 479 before). They account for that tree's remaining slow runs (14.8 and 15.9 s). Devel logs 0 of them in every run measured here, and it does not reproduce K94 on its own (0 of 24 runs). So the before/after pair is measured on DH's tree and the acceptance on devel. The mechanism is the same on both: devel has the same `peer_connect` and `efs_rdma_send_frame` code, and any caller that reaches the pool nearly spent still hits it.

## The fix

Policy: a pooled connection outlives its first caller. Its setup is therefore bounded by a budget of its own, not by that caller's leftover. A caller whose budget runs out fails on its own and leaves the connection usable for the next caller. The other option, that a proof must not open a connection it cannot finish, cannot be decided up front: setup cost depends on the peer, and a nearly spent caller cannot tell whether 3 ms is enough.

- `src/server/peer_pool.c`, `peer_conn_get`: when the caller's deadline is sooner than now + `EFS_CONNECT_TIMEOUT_SEC` (the existing 2 s connect bound), the pooled `peer_connect` runs with that bound instead. The caller's deadline is restored afterwards. A caller with no deadline, or with more than 2 s left (the 5 s membership notice), keeps its own. Private connections (`server_peer_conn_new`: Raft senders, heartbeat and packer probes) are unchanged, because the probe owns that connection and its deadline.
- `src/server/raft_host.c`, `host_rpc_submit`: after pool admission and any setup, a spent budget (`!efs_net_remaining_ms(1)`) releases the connection to the pool unsent and returns `EFS_ERR_BUSY`. `host_remote_cmd` already treats BUSY as retryable. A receive timeout still drops the connection, because a request is in flight and its late reply would poison the next caller.
- `src/common/rdma.c`, `efs_rdma_send_frame`: `ms == 0` at entry returns `EFS_ERR_NET` as before, but before `tx_id` or any post, and without setting `rc->broken`. This covers the sub-millisecond race after the check above.
- `include/efs/network.h`: `EFS_CONNECT_TIMEOUT_SEC` moves here from `network.c` (same value).
- `tests/test_peer_pool_deadline.py`: the harness stubs the thread deadline. A new case checks three things: a short caller deadline is replaced by the connect bound for the setup and restored after it; no deadline stays none; a longer deadline is kept.
- `tests/test_host_read_deadline.py`: `host_rpc_submit` with a spent budget returns BUSY, releases the connection and sends nothing.
- `tests/budgets.tsv`: K38 shared-chunk row 25 to 20 s (see [Results](#results)).

Commits: `a141ee309` (first version), `a14d8acd4` (harness), `e8a61a1d9` (send error codes kept, see the finding below), `dab77615c` (extend only, budget row), `0dba77033` (`tests/test_host_read_deadline.py` stubs the budget and checks the spent-budget release).

Edge cases. A proof that connects to a slow or hung peer can now wait up to the 2 s connect bound for that setup instead of failing at 400 ms. It still fails BUSY on its spent budget afterwards. That 2 s is the bound every pooled setup already had for callers without a deadline, for the TCP connect itself. Mutations (`host_remote_cmd_impl` outside a read scope) have no deadline and are unchanged.

### Finding: the first version changed the client's error contract

The first version (`a141ee309`) also returned `EFS_ERR_BUSY` from `efs_conn_send_msg_parts` whenever the budget was spent before any byte. That path is shared with the client, which gives BUSY its own meaning: retry the fragment every 50 ms until the 30 s `put_pool_run` deadline, where NET fails it over to another member. `tests/test_k36_recovery.py` step 2 (node 3 exits after its fifth PUT acknowledgement) took 2.3 to 2.5 s in 9 of 9 runs on devel. On that version it took 2.4, 3.2, 30.4, 2.3, 2.3 and 3.2 s, and gate 2 of the first gate set had the test at 77.4 s against its 55 s budget. `e8a61a1d9` reverts the send-path change and moves the check into `host_rpc_submit` (server only). The k36 rerun on the final code is in [Results](#results).

## Results

Host: node9900 (192 CPUs, idle, `mlx5_0`), loopback 3-node cluster, `transport auto` (picks RDMA), buffered I/O, the test's default fixture (24 writers, 200 records of 47,008 B, dcache 1 MiB). The write phase is the test's `write+fsync` line. Counts are over the three daemon logs of each run.

| Tree | Runs | write+fsync s per run | setup failed | send type=95 FAILED | reply-timeout drops |
|---|---|---|---|---|---|
| devel `826d0ef22` (before) | 8 solo | 5.8, 6.5, 6.5, 7.0, 6.4, 7.7, 6.3, 6.9 | 0 | 0 | 0 |
| devel `826d0ef22`, 4 loops in parallel | 16 | 6.6 to 8.1 | 0 | 0 | 0 |
| DH tree `bb8fcdcc4` | 6 solo | **21.3, 29.8**, 6.7, **32.0**, 6.6, **25.9** | 226, 244, 0, 364, 0, 240 | 3, 1, 0, 14, 0, 5 | 216, 257, 0, 479, 0, 252 |
| `bb8fcdcc4` + final patch | 6 solo | 7.8, 6.6, 6.8, 15.9, 6.7, 14.8 | 0 (all) | 0 (all) | 0, 0, 0, 44, 0, 60 |
| devel + fix `dab77615c` (after) | 10 solo | 6.2, 6.2, 7.0, 6.3, 6.6, 7.1, 6.2, 6.2, 6.1, 6.2 | 0 (all) | 0 (all) | 0 (all) |

The first version on DH's tree (10 solo) had 0 setup and send failures as well, with 6 of 10 runs slow (10.6 to 20.6 s, 20 to 114 reply-timeout drops each).

`tests/test_k36_recovery.py`, 9 solo runs per tree, alternating devel and the fix (`k36-v3/`). Totals: devel 34.3, 36.2, 39.1, 43.6, 42.1, 32.8, 38.2, 43.2, 48.9 s (median 39.1); fix 37.0, 32.3, **62.5**, **63.8**, 45.0, 38.6, 36.5, 46.6, 26.5 s (median 38.6). Step 2, the step the first version slowed, takes 1.2 to 3.0 s on devel and 1.1 to 3.7 s on the fix, with no 30 s wait. The test has three steps with a 10 s slow variant on both trees: step 4 (fence), step 6 (publication crash) and step 11 (segment rebuild). Their rates are similar, and the two long runs on the fix hit all three at once. None of 15 devel runs with a per-step log did (devel maximum 49.2 s over 18 runs), so this looks like chance at this sample size, but more runs did not rule it out. The gates' K36 times are below.

Gates (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, node9900, final code at `0dba77033`, `gates-v4/`): three times `TOTAL 593 FAIL 0 NOTRUN 4`, 638/659/659 s, no OVERBUDGET. K38 12.4/10.8/12.3 s (row 20 s); `test_k36_recovery` 37.2/42.0/54.8 s (row 55 s). The 4 NOTRUN are the host's usual ones: `test_cluster_stop` (no cluster scripts), `test_rdma_memcheck` and `test_leak_gate` (no valgrind), and the RDMA hardware part of `test_qualify_rdma_live`. Wave 61 on node9900 had the same 593/0/4. Earlier gate sets, kept for the record: on the first version, three gates with FAIL 0 (after one aborted because `test_peer_pool_deadline` did not link), K38 12.4/11.5/10.5 s and `test_k36_recovery` 31.9/**77.4**/54.2 s, which led to the finding above. On `dab77615c`, gate 1 failed only `test_host_read_deadline`, whose harness did not stub the new budget check (fixed in `0dba77033`, which adds the spent-budget release case).

Budget: the method in `tests/budgets.tsv` (slowest host median of three 8-job gates x 1.25, rounded up to 5 s). The slowest median is fstor007's 12.8 s from DH's gates on `bb8fcdcc4` (12.8/12.1/16.0; not re-run on the fix). node9900 on the fix: 12.4/10.8/12.3 s, median 12.3 s. 12.8 x 1.25 = 16.0, so 20 s.

Other checks on the fixed tree: `make test` rc 0; TSan (`EFS_SAN_ONLY="k96_status_slots_live k38_swept_hint_live k88_chunk_hints_live d4_progress heartbeat_shutdown k36_recovery" bash tests/sanitizer_check.sh tsan`) VERDICT PASS, 6 of 6; `python3 tests/csrc.py`, `python3 scripts/check_include_boundary.py` and `python3 tests/test_make_test_recipe.py` (budgeted gate 1302 s of 1750 s) PASS.

## Reproduction

Prerequisites: an RDMA host on which a 3-node loopback cluster picks RDMA (`transport auto`), and libfuse 3.16 (`EFS_FUSE_PREFIX`).

```bash
make -j32 && make -j32 test-bins tests/faults/efsd
# one run; the kept directory holds the daemon logs k/n1.log .. k/n3.log
EFS_K38_KEEP=1 python3 tests/test_k38_shared_chunk_pressure.py
grep -c 'segmented RDMA setup failed or incompatible peer' <kept>/k/n*.log
grep -c 'RDMA send type=95 FAILED' <kept>/k/n*.log
grep -c 'RDMA counters.*data_rx=0/0' <kept>/k/n*.log   # reply-timeout drops
python3 tests/test_k36_recovery.py                      # step 2 should take about 2.4 s
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
EFS_SAN_ONLY="k96_status_slots_live k38_swept_hint_live k88_chunk_hints_live d4_progress heartbeat_shutdown k36_recovery" bash tests/sanitizer_check.sh tsan
```

Before: check out `bb8fcdcc4` (devel itself no longer triggers K94, see [Cause](#cause)), run the K38 line 6 times and count. After: apply `git diff 826d0ef22 dab77615c -- src include` to that tree, or use the branch. Delete the kept directories afterwards. The driver used here (run, keep, count, delete) is not committed; its counts are exactly the greps above.

## Limits

- Devel no longer triggers K94 (K96 removed the trigger), so the before numbers come from DH's tree. On devel the fix is shown by the unit case, by 0 setup failures and by unchanged timing, not by a before/after difference.
- The remaining slow runs on DH's tree come from reply timeouts (K96 there), not traced further.
- Not changed: in `auto` mode, a SETUP exchange that fails after the request was sent keeps the connection as TCP, though the server may have published RDMA. With setup on its own budget this now takes a real transport failure. A fail-closed fallback (destroy the connection) is a follow-up, not part of K94.
- Not checked: whether any test expects a proof against a peer that accepts TCP but never declares to fail within 400 ms (none failed in the gates).
- Hosts: node9900 only (loopback, one HCA). fstor007 and distinct-host RDMA (K38 acceptance) were not run.

## Raw captures

`~/efs-artifacts/taskDU-20261107/` on the ORCD hosts (NFS home):

- K38 solo runs (per-run output, daemon and mount logs, `summary.txt`): `before/`, `before-par1..4/`, `dh-bb8fcdcc4/`, `dhfix-bb8fcdcc4-v1/`, `dhfix-v3/`, `after/` (first version), `after-v3/`.
- k36 runs: `k36/` (first version against devel), `k36-v3/`.
- Gates (summaries, `timings.tsv`, failure logs): `gates-aborted-harness/` (first version; `test_peer_pool_deadline` harness did not link), `gates/` (first version), `gates-v3-harness/` (`dab77615c`, gate 1 only; `test_host_read_deadline` harness), `gates-v4/` (final).
- `tsan-v4.txt`, `make-test-v4.txt`, `v3.txt`, `v4.txt` (final); `tsan/`, `make-test.txt`, `checks.txt` (first version).

`SHA256SUMS` sha256 `030c115208e0bafce3157c7af756fbe32103cbb8288557ba5d7f44604b87d7d6`. Reproduction does not depend on them.
