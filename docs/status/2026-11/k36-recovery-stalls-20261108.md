# `test_k36_recovery` step stalls: fixed 10 s rebuild skips (Task DY)

**Verdict: PASS. The stalls were product waits on fixed timers, not work; no correctness defect, so no K row (blocks alpha: no).** Steps 4, 6 and 11 waited out a 10 s node skip in the W154 chunk rebuild (`src/server/raft_host.c`) or the K37 segment rebuild (`src/server/segment_rebuild.c`) after a failure that had cost milliseconds: a target still fenced by its own recovery, a target between kill and restart, or a pooled connection to a peer's dead previous process. The skips are now driven by the event (the fence clearing, the next pass after a cheap failure, the peer's FIN). Solo on node9900, 20 runs each on the same base: total median 41.9 → 25.7 s, p95 57.6 → 27.9 s, max 57.6 → 28.1 s. 8-job gate on node9900: `TOTAL 593 FAIL 0 NOTRUN 4`, no over-budget row, `test_k36_recovery` 24.7 s (55.4 s in the wave 62 gate). Budget row lowered 55 → 40 s.

Purpose: Task DY, the slowest intermittent in every gate (`tests/budgets.tsv` row 55 s): 33 to 57 s normally, 77.4 s once in a Task DU gate, a bimodal ~10 s at steps 4 and 6 in Task DU's timings ([K94 page](k94-rdma-upgrade-budget-20261107.md)).

Tested source: branch `claude/wonderful-poincare-e52e7a`, commit `7a776a132` on devel `658a5c67c` (after wave 62), built from `git archive` (build id `unknown`). Before: devel `658a5c67c` with only this branch's `tests/test_k36_recovery.py` (per-step times) and `scripts/evidence/dy/`. An earlier pair on devel `826d0ef22` (before wave 62) gave the same picture and is in the raw data. The budget-row change came after the gate; it changes no code.

Environment: node9900 (192 cpus, libfuse `/data1/efs/deps/libfuse`, no valgrind), private loopback clusters of the test (three, later four `tests/faults/efsd` members, transport auto: RDMA on mlx5_0 with TCP setup, buffered I/O, 1 GiB quota each, one efs-fuse mount at a time), TMPDIR under `/tmp/efs-taskDY-tmp`, free ports. Solo: every counted run started with no other `efsd` on the host and load under 2 (`runNN.load`). Five pilot runs that overlapped the wave 62 gate and eight before runs that overlapped Task DX's gate were not counted (they are in the raw data, separately).

## Where the time went

`scripts/evidence/dy/k36_trace.py` charges every top-level statement of the test that takes 1 s or more to its line; the test now prints each step's own and cumulative time. The daemon logs of every run were kept.

| Step | Statement that waited | Log signature (before) | Cause |
|---|---|---|---|
| 4 (fence) | `wait_status(... rebuilt ... rebuild_waiting == 0)`, 12.2 s in half the runs | `raft-host: rebuild ... node=3: PUT failed (rc=-1); mark kept` while node 3 sat in `recovery: waiting: held by EFS_FAULT_RECOVERY_HOLD_FILE`; `rebuilt ... on node 3` 10.7 s later | Node 3's store accepts the rebuilt fragment but its COMMIT returns EIO until verified recovery (`recovery_uncertain`, by design). W154 treated the refusal as a failed node and skipped node 3 for `DG_NODE_BACKOFF_US` (10 s), long after the fence cleared. The recovery watch then saw the clear at its next `RV_WATCH_MS` (2 s) poll. |
| 4, 6 (restart) | same, and `whole(c)` after node 2's restart | `PUT failed (rc=-6)` 20 to 150 ms around a member's kill or restart | No answer: the target was between `SIGKILL` and listening (node 3 crashed 15:24:57.087, PUT failed .107/.131, listening .148), or the peer pool handed out a connection to the dead previous process (its own comment: stale pooled connections "are still found and dropped on use"). Again a 10 s skip for a call that cost milliseconds. |
| 11 | `wait_status(... segments ... rebuilt=[1-9] waiting=0)`, 2, 8, 12 or 18 s | none (the K37 skips were silent) | K37's audit probe of the killed holder failed the same way and set `SR_NODE_BACKOFF_US` (10 s) without a log line; one or two such skips per run. With the new skip log the after runs show these probes (`segment-rebuild: node N skipped for 0 ms: audit probe failed (rc=-6)`). |
| 1 → 2, ~30 s | not seen in 30 solo before runs or 40 after runs | — | Task DU's one instance was on DU's own tree with a send-path BUSY change it later reverted (`e8a61a1d9`). Not reproduced on devel. |

## The change

- **W154 fence** (`raft_host.c`): `dg_put_fragment` returns `EFS_ERR_FENCED` (internal) when the target stored the fragment and refused the COMMIT (locally: the store is not settled). The node is not skipped; it is marked fenced for this pass, and each later pass sends it one empty `DATA_COMMIT` (`dg_commit_probe`) before fetching any source. When that succeeds, the marks go at once. A long production fence costs one small RPC per pass, not two fragment fetches per mark.
- **Cost-proportional skip** (both loops): a failed fetch, PUT, probe or audit skips the node for twice what the failed call cost, at most the old 10 s (`dg_skip`, `sr_skip`). A hung peer's 5 s I/O bound still buys the full 10 s, so the hung-target guarantee the comments promise is unchanged; a refused connect or a stale conn costs milliseconds and the next pass (about 1 s) tries again. Every skip is logged with its length.
- **Peer pool** (`peer_pool.c`): a checkout of a connection idle for over 1 s checks it first and reconnects if the peer closed it (CLOSE-WAIT after a restart, fd recycled, socket error). The check is the client pool's `conn_fd_is_dead`, moved to `src/common/network.c` as `efs_conn_peer_closed` and used by both pools. One run's RDMA connection count matched devel (171/190/207 vs 189/187/200 `RDMA transport up` per member), so there is no reconnect churn.
- **Recovery watch** (`recovery.c`): `server_recovery_kick()` wakes the watch when this replica applies a W154 or K37 mark clear; the 2 s poll stays only as a fallback for a group this node does not host.
- **Test**: `tests/test_k36_recovery.py` prints `[step s, at total s]` on every PASS line. No claim changed.

No timeout, budget or bound was raised and no sleep was added. The 10 s ceilings are unchanged.

## Results

Solo, node9900, devel `658a5c67c` (before) vs `7a776a132` (after), seconds:

| Step | Before, 10 runs: median / p95 / max (runs > 5 s) | After, 20 runs: median / p95 / max (runs > 5 s) |
|---|---|---|
| 1 form | 4.2 / 5.0 / 5.0 (0) | 4.2 / 5.0 / 5.0 (0) |
| 2 crash in PUT | 2.4 / 3.1 / 3.1 (0) | 2.4 / 3.6 / 3.7 (0) |
| 3 crash in recovery | 0.1 / 0.1 / 0.1 (0) | 0.1 / 0.1 / 0.1 (0) |
| 4 fence | **7.5 / 12.9 / 12.9 (5)** | **1.9 / 2.5 / 2.8 (0)** |
| 5 node 1 down | 1.6 / 2.0 / 2.0 (0) | 1.8 / 2.8 / 3.5 (0) |
| 6 publication | **4.7 / 14.6 / 14.6 (2)** | **4.7 / 5.1 / 5.9 (2)** |
| 7 reclaim | 2.0 / 5.4 / 5.4 (1) | 2.0 / 3.9 / 4.1 (0) |
| 8 compaction | 1.4 / 1.9 / 1.9 (0) | 1.4 / 1.5 / 2.3 (0) |
| 9 certificate | 2.3 / 2.5 / 2.5 (0) | 2.3 / 2.3 / 2.3 (0) |
| 10 group 2 | 1.8 / 2.3 / 2.3 (0) | 1.5 / 2.6 / 2.6 (0) |
| 11 segments | **9.5 / 19.3 / 19.3 (7)** | **2.1 / 3.0 / 3.0 (0)** |
| total | **41.9 / 57.6 / 57.6** | **25.7 / 27.9 / 28.1** |

Log counts over the same runs: before, 4 W154 `PUT failed (rc=-1)` and 3 `(rc=-6)`, each a 10 s skip. After, no 10 s skip at all; 11 `COMMIT refused (store fenced)` (no skip), 154 W154 and 209 K37 skips, every one 0 to 2 ms. The earlier pair on devel `826d0ef22` (20 solo before, 20 after) gave total median 41.7 → 26.6 s, p95 52.9 → 28.1 s.

- Gate (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, `make test-all`), node9900, `7a776a132`: `TOTAL 593 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (613s)`. NOTRUNs: cluster scripts absent, valgrind absent (RDMA Memcheck, leak gate), RDMA hardware runs of `test_qualify_rdma_live`. No over-budget row. `test_k36_recovery` 24.7 s.
- `make test`: rc 0. `tests/test_peer_pool_deadline.py`, `tests/test_conn_checkout_budget.py` and `tests/test_resolver_deadline.py` (harnesses of the touched pools) PASS.
- TSan (`EFS_SAN_ONLY="k36_recovery k36_segment_recovery k37_segment_rebuild heartbeat_shutdown d4_progress k96_status_slots_live data_commit" make tsan-check`): `VERDICT: PASS (7 run, 0 failed, 299s)`.
- Budget row `tests/budgets.tsv`: 55 → 40 s (the slowest after run, 28.1 s, × 1.25 rounded up to 5 s). One gate on one host; fstor007 was not re-measured.

## Limits

- One host. fstor007 (historically about 10 % slower on this test) and distinct-host clusters were not run; the row should be refit from three gates per host at the next wave.
- A member that stays down is retried about once a pass until the heartbeat marks it down (three missed heartbeats, about 6 s); for W154 each retry fetches the two source fragments first. That is the price of not waiting 10 s for a restart; a hung member still gets the full skip.
- The pool's dead-connection check covers a peer that closed its socket. A host that vanished without a FIN still costs one bounded timeout, as before.
- Step 6's median (4.7 s) is the step's own work (restart, six writes, a crash, recovery) and did not change.

## Raw data

On node9900 under `~/efs-artifacts/taskDY-20261108/` (shared `erbmi1` home): one tarball per loop (`out-pilot`, `out-before`, `out-before2`, `out-before-load`, `out-before-solo`, `out-after` on `826d0ef22`; `out-before3`, `out-after2` on `658a5c67c`), each run's test output, tracer gaps, load record and daemon and mount logs; `summary.txt` (the tables above), `make-test.log`, `tsan.log`, `gate.log`, `gate-summary.txt`, `gate-timings.tsv` and the chain scripts as run. `SHA256SUMS` sha256: `1680b48168c2f4accd95da47c94ff3ac09b87cc768c30b5d8bde2e812e4ee620`. `/tmp/efs-taskDY*` were removed.

## Reproduction

From a tree built with `make test-bins tests/faults/efsd` (on node9900 with `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse` and `LD_LIBRARY_PATH` at its `lib64`), TMPDIR set to a private directory, on an otherwise idle host:

```bash
sh scripts/evidence/dy/k36_loop.sh "$PWD" /tmp/k36-after 20
```

```bash
python3 scripts/evidence/dy/k36_summary.py /tmp/k36-after
```

For the before side, run the loop in a devel tree with this branch's `tests/test_k36_recovery.py` and `scripts/evidence/dy/` copied in. Expected: before, steps 4 and 11 over 5 s in about half the runs, with `PUT failed` lines followed 10 s later by `rebuilt` lines; after, every step under 6 s and no `skipped for 10000 ms`. One run takes about 26 s after, 42 s before. Remove the output directories and TMPDIR afterwards. Remaining gates: fstor007 gate and a three-gate budget refit at the next wave.
