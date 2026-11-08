# Client RDMA reply waits miss a dead peer: the k36 put-window and REPORT wait (Task DZ)

**Verdict: PASS. A product wait on a bound, not a correctness defect; no K row (blocks alpha: no).** After a member crash, the client's PUT reply waits on RDMA connections polled only the RDMA event fd. An idle queue pair never errors when its peer exits; only the TCP control socket closes, and nothing watched it. So every PUT holding a connection to the crashed member waited out the 2 s post-quorum drain, and a PUT still short of quorum waited out its 30 s operation budget. That is the "~30 s between steps 1 and 2" of `test_k36_recovery` that [Task DY](k36-recovery-stalls-20261108.md) attributed and left open. The waits now poll the connection's TCP socket too, and an empty ready check retires a connection whose control socket has EOF or stray bytes. That rule was already `message_pause`'s, and the `tcp_fd` field comment in `rdma.c` promised it. No timeout, bound or budget changed and no sleep was added.

Purpose: the Task DY follow-up "Client put-window and REPORT wait after a member crash" (the `tests/budgets.tsv` k36 row).

Tested source: branch `claude/dz-rdma-peer-exit` on devel `3f9110d52` (after wave 63, which merged DY and DX/K100), commits `8e8beade9` (the change) and `88d983ffe` (the unit-test harness), built from the working tree with `make test-bins tests/faults/efsd` (build id `unknown`). Before: devel `3f9110d52` from `git archive`. The attribution runs (`out-diag`) used DY's tip `9fe5772de` with a diagnostic-only hook, not committed (`diag-gdb-hook.patch` in the raw data): `gdb -batch -ex 'thread apply all bt'` on the mount when the put-window wait timed out or the unmount REPORT failed 10 times. It fires only on the slow path.

Environment: node9900 (192 CPUs, libfuse `/data1/efs/deps/libfuse`, no valgrind). The test's private loopback clusters of `tests/faults/efsd` members used transport auto (RDMA on mlx5_0, TCP for setup), buffered I/O, a 1 GiB quota each and one efs-fuse mount at a time. TMPDIR was under `/tmp/efs-taskDZ-tmp`, with free ports. Every counted run started with no other `efsd` on the host. The user's four `tiny-files` screens ran on the host throughout, at load 0.3 to 2.3, and were left alone.

## Where the time went

The slow DY run (run04 of DY's `out-after3`, 57.2 s; its logs are in DY's raw data):

| Time (UTC) | Mount A log | Meaning |
|---|---|---|
| 16:35:50.511 | (node 3: `fault: injected crash at put-acked`) | node 3 exits (SIGKILL); its sockets close, its queue pairs go silent |
| 50.525 → 52.519 | 27 × `put_fragments ... fragment N for node 3 not acknowledged; published degraded` | PUTs that had two acknowledgements waited out the **2 s drain** on node 3's dead RDMA connection, then acknowledged degraded |
| 52.5 → 00.500 | `dcache-flush-fail ino=20481 stage=put-window rc=-13` | one PUT of W1 was still in flight; `efs_dcache_flush_ino` waited its 8 s window bound; W1's write returned EIO |
| 02.023 → 20.468 | 86 × `report-sync-fail ino=0 stage=report-retry ... rc=-13`, every ~217 ms | `a.close()` (step 5) unmounts; the unmount drain retries every 200 ms while that PUT's unit stays held |
| 20.708 | `put tickets outstanding at unmount: 0` | the stuck PUT ended at its 30 s operation budget (W1 started ~50.4) and the drain completed |

Nodes 1 and 2 logged no further PUT errors from 55.0 on (`iostats ... put ... errors=3` and `errors=5`, unchanged), so the PUT was not resending. It was blocked. In `put_fragments_parallel_once` (`src/client/write.c`) both the quorum wait and the drain pass `poll()` the fd that `efs_conn_put_reply_watch` returns. For an RDMA connection that fd is `efs_rdma_reply_fd()` = the eventfd the completion poller signals. `efs_rdma_message_ready` reads the receive ring and never looks at the TCP socket. A peer that dies after its sends completed leaves no completion, no overdue send and no wake. Only `message_pause` (the blocking waits) and the server's `efs_conn_wait_request` polled the TCP socket, which is why DY's server-side work never saw this. The GET pair wait in `src/client/read.c` (`get_two_parallel_run`, 5 s bound) had the same efd-only poll.

The 2 s drain cost every run. Measured from node 3's injected crash to mount A's last degraded acknowledgement, devel took 2.005 to 2.028 s in all 20 runs. The 30 s case needs a PUT that is short of quorum at the crash (another member answering BUSY once, for example). It was seen in 1 of DY's final 10 runs and in none of the 80 runs here (40 diagnostic, 20 before, 20 after). The hook therefore never fired, and the 30 s path is attributed from the log above and the code, not from a stack.

## The change

- **`src/common/rdma.c`**: `tcp_retired()` holds `message_pause`'s rule: after the upgrade, TCP application bytes or EOF retire the connection. `message_pause` uses it, and `efs_rdma_message_ready` now calls it when nothing is ready (one non-blocking `recv(MSG_PEEK)`, only on an empty check). The server's `efs_conn_wait_request` already tore such a connection down; it now does so one check earlier, with the same outcome.
- **`src/client/write.c`**: `put_reply_pollfds()` adds the connection's TCP socket next to the event fd for RDMA connections, in the quorum wait and in the drain. On a wake the existing re-check sees `-1` and takes the existing drop path: a hard slot failure, the K35 resend once, then a degraded acknowledgement.
- **`src/client/read.c`**: the same TCP socket entry in the GET pair's poll set.
- **Tests**: `tests/test_rdma_progress_ready.py` adds a case for an empty ready check on a retired control socket. `tests/test_put_reply_deadline.py` builds the new helper with the PUT loop it compiles from source and checks the poll set.

The 2 s drain cap, the 8 s put-window bound, `EFS_IO_TIMEOUT_MS`, the 30 s down mark and the unmount drain are unchanged. They simply stop being reached.

## Results

Solo, node9900, devel `3f9110d52` (before) vs `8e8beade9` (after), 20 runs each, in seconds:

| Step | Before: median / p95 / max | After: median / p95 / max |
|---|---|---|
| 1 form | 4.3 / 5.0 / 5.0 | 4.2 / 5.0 / 5.0 |
| 2 crash in PUT | **2.4 / 3.3 / 3.7** | **0.6 / 2.0 / 2.0** |
| 3 crash in recovery | 0.1 / 0.1 / 0.1 | 0.1 / 0.1 / 0.1 |
| 4 fence | 0.9 / 2.8 / 2.8 | 2.1 / 2.8 / 3.7 |
| 5 node 1 down | 1.5 / 2.5 / 2.6 | 1.8 / 2.5 / 2.6 |
| 6 publication | **4.7 / 5.6 / 6.7** | **3.5 / 4.9 / 4.9** |
| 7 reclaim | 1.8 / 3.3 / 5.4 | 1.6 / 2.2 / 2.4 |
| 8 compaction | 1.5 / 1.9 / 2.6 | 1.2 / 1.6 / 2.2 |
| 9 certificate | 2.3 / 2.3 / 2.3 | 2.3 / 2.5 / 2.5 |
| 10 group 2 | 1.5 / 2.3 / 2.3 | 1.4 / 2.6 / 2.7 |
| 11 segments | 2.1 / 3.0 / 3.3 | 2.4 / 3.0 / 3.0 |
| total | **25.4 / 28.1 / 28.9** | **23.6 / 25.6 / 26.3** |

- From crash to the last degraded acknowledgement: before, 2.005 to 2.028 s in all 20 runs. After, 17 to 39 ms in 15 runs, and 0.22, 0.78, 1.57 and 1.57 s in the rest (the PUT retry backoff, not a drain). Every run still acknowledges the same 18 to 28 stripes degraded, all rebuilt later as before.
- Step 4 is about 1.2 s longer at the median. The rebuild of step 2's marks now waits in step 4 instead of overlapping step 2's 2 s drain; steps 2 to 4 together went from 3.4 to 2.8 s.
- No `stage=put-window` and no `report-sync-fail` in any of the 80 runs.
- Gate (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, `make test-all`), node9900, at `88d983ffe`: `TOTAL 593 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (615s)`. That is the same four host NOTRUNs as DY's gates: no cluster scripts, no valgrind (two) and no RDMA hardware run. No over-budget row; `test_k36_recovery` 24.4 s, `test_k37_segment_rebuild` 39.6 s.
- A first gate at `8e8beade9` had 2 FAILs. One was `tests/test_put_reply_deadline.py`, whose harness compiles the PUT loop from source and lacked the new helper; that is fixed in `88d983ffe`. The other was `test_k37_segment_rebuild` step 3: `Protection: full` was expected but status read `Degraded chunks: 8 ... retired=8`. W154 had retired those 8 chunk marks as "object superseded" at 20:47:32.909 to .910, 3 ms before the segment rebuild the test waits for (.912). `rebuild_retired` counts a clear on its proposal and `Degraded chunks` on its apply, so the check raced the apply. It did not recur in 5 solo runs on each of devel and this change, or in the second gate. It is recorded below as open.
- `make test`: rc 0 at `88d983ffe`.
- TSan (`EFS_SAN_ONLY="k36_recovery k36_segment_recovery k37_segment_rebuild k38_swept_hint_live rdma_handler_alignment heartbeat_shutdown data_commit" make tsan-check`, at `8e8beade9`): `VERDICT: PASS (6 run, 0 failed, 172s)`.
- Budget row `tests/budgets.tsv`: 40 → 35 s (the slowest after run, 26.3 s, × 1.25 rounded up to 5 s). One host; fstor007 was not re-measured.

## Limits

- One host, loopback members. fstor007 and distinct-host clusters were not run. On a real network, a host that vanishes without a FIN (power loss, a cable) still costs the bounded waits as before. This change covers a peer process that exits.
- The 30 s case was not reproduced in 80 runs, so its removal rests on the mechanism, not on an observed before/after pair. If it recurs, the 35 s row will show OVERBUDGET.
- The `test_k37_segment_rebuild` step 3 race (`Protection: full` read before the superseded clears apply) was seen once in 2 gates and 10 solo runs. It is a test check on a counter pair, not lost protection, and it is not fixed here.
- `efs_bench.c`'s two reply-watch loops keep their efd-only poll; they are benchmark code. Their ready checks now see a retired control socket, so a dead peer ends them at the next poll slice.

## Raw data

On node9900 under `~/efs-artifacts/taskDZ-20261108/` (shared `erbmi1` home):
- one tarball per loop: `out-diag` (40 runs, DY tip with the hook), `out-before`, `out-after` and `k37`, each with per-run output, load and the daemon and mount logs only;
- the chain scripts and statuses, `make-test*.log`, `tsan.log`, both gates' `summary.txt` and `timings.tsv`, and the first gate's `fail-503.log`;
- `diag-gdb-hook.patch` and `summary.txt` (the table above, from `k36_summary.py`).

`SHA256SUMS` lists every file; its own sha256 is `cb0282b7e68830259cfd6daaac6fb1888cc8968522dd7ebc4b2ba719ee6cc769`. Stores were deleted after each run.

## Reproduction

From a tree built with `make test-bins tests/faults/efsd` (on node9900 with `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse` and `LD_LIBRARY_PATH` at its `lib64`), TMPDIR set to a private directory, on an otherwise idle host:

```bash
sh scripts/evidence/dy/k36_loop.sh "$PWD" /tmp/k36-after 20
```

```bash
python3 scripts/evidence/dy/k36_summary.py /tmp/k36-after
```

Crash-to-last-degraded-acknowledgement, per run: the time of the last `not acknowledged` line in `runNN-logs/k36/mnt-a.log` minus that of `fault: injected crash at put-acked` in `runNN-logs/k36/n3.log`. Run the same loop on devel `3f9110d52` for the before set. Clean up: delete the TMPDIR and the output directory.
