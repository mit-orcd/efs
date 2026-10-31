# K79: daemon stop with a blocked stderr, task BP — October 31, 2026

**K79: FIXED (blocks alpha: no). Stop-blocked test fails on base, passes with the fix; gate FAIL 1 (K37 load timeout, passes alone); TSan and ASan PASS.**
Linked items: K79, K72/K80 ([AB/AE page](ab-ae-raft-logging-20261030.md)), letter task AE.
Tested source: branch `claude/bp-k79-stderr-dcc693`, base devel `909a07d71`.

## Defect

With a daemon's stderr a pipe nobody reads, a SIGTERM'd `efsd` did not exit
cleanly: about 230 synchronous `fprintf(stderr)` sites in the Raft host, heartbeat,
GC, recovery, packer and handler paths wrote on the stdio lock, so threads that
the stop joins sat in `write(2)` and the stop reached its forced exit (rc 3
after 55 s). The user decided the design: the remaining synchronous writers go
through the existing reporter (K72). No second queue, no lock added or widened.

## Design

- `efs_telemetry_log(0, ...)` is the drop-in for `fprintf(stderr, ...)`. It
  formats on the calling thread (no stdio lock), enqueues into the one reporter
  mailbox with a trylock, and counts every drop in the `telemetry: overflow=`
  line. With no reporter bound (before the Raft host starts, after it stops, tools,
  tests) it falls back to synchronous stderr, so one call is correct on both sides.
- Mailbox: 4 → 32 slots (routine lines share it with the periodic records,
  which still coalesce); line buffer 512 → 2048 bytes (longer lines count as
  overflow). Still one queue, one reporter thread.
- Producers: a routine line takes the binding read lock and the mailbox copy lock (blocking, bounded: no thread holds either across I/O); the pump's records keep the trylock.
- Stop: `efs_telemetry_stop` now drains pending records for at most 200 ms on a
  sink that accepts them (before, pending records were discarded); a blocked sink
  never holds the stop longer (`emit`/`send_bytes` give up at the deadline).
- Stop order is unchanged and already right: `server_raft_host_stop` joins the
  query, membership, GC, finisher and pump threads and the senders, then stops
  the reporter, and it contains no stderr write. The stop-time lines of `efsd.c`
  (`shutdown_say`, exit report) already run on detached helper threads (K01) and
  stay synchronous; the exit report waits at most 5 s for a blocked pipe, which
  is the 5.4 s exit below.
- Raft core (`raft.c`), KV, meta and protocol code now log through the same call.
  Under a Raft lock this replaces a blocking formatted write by a bounded
  format plus two trylocks and a memcpy; no I/O and no new lock.

## Inventory (daemon-linked code: `src/server`, `src/raft`, `src/kv`, `src/meta`, `src/common`)

Class (a) routine, converted: **385** sites. Class (b) must stay synchronous,
each with a one-line `sync:` comment: **101**. Class (c) test or fault hook, same
comment: **39**. Remaining class (a): **0** (a scan of all 142 `fprintf(stderr` hits finds every one except the two `vfprintf`
fallbacks in `telemetry.c` under a `sync:` comment; three `perror` sites in the accept loop were
converted by hand).

| Class | Count | Why |
| --- | --- | --- |
| (b) startup | 92 | argument parsing, join/discovery, storage and member-store refusals, quota, `raft-host` startup refusals before the reporter exists (`efsd.c` before the host start, `storage_roots.c`, listener and transport refusals in `network.c`/`rdma.c`/`common.c`, KV manifest refusal, query-disk config) |
| (b) stop helper | 3 | `efsd.c` helper threads for stop lines and exit-record failure (K01: the stopper never touches stdio) |
| (b) abort / child | 6 | `kv-flush` and `kv-lsm` lines before `abort()`; `perf`/`strace` recorder child `perror` before `_exit` |
| (c) fault and debug hooks | 39 | `EFS_FAULT_*` and `fault:` lines, `raft_disk` window-drop, `txn-stale-version` trace, `rdma-trace` backtrace pairing |

| File | (a) | (b) | (c) |
| --- | --- | --- | --- |
| `src/common/common.c` | 0 | 2 | 0 |
| `src/common/network.c` | 0 | 3 | 1 |
| `src/common/protocol.c` | 6 | 0 | 0 |
| `src/common/query_stage_trace.h` | 2 | 0 | 0 |
| `src/common/rdma.c` | 12 | 5 | 0 |
| `src/kv/kv_compact.c` | 9 | 1 | 2 |
| `src/kv/kv_lsm.c` | 4 | 1 | 2 |
| `src/kv/kv_lsm_internal.h` | 0 | 1 | 0 |
| `src/kv/kv_snap.c` | 1 | 0 | 0 |
| `src/meta/meta_apply.c` | 1 | 0 | 2 |
| `src/meta/metadata.c` | 3 | 0 | 0 |
| `src/meta/query_disk.c` | 8 | 3 | 0 |
| `src/meta/query_disk_incremental.h` | 3 | 0 | 0 |
| `src/meta/txn.c` | 0 | 0 | 2 |
| `src/raft/raft.c` | 25 | 0 | 0 |
| `src/raft/raft_disk.c` | 0 | 0 | 1 |
| `src/raft/raft_log.c` | 3 | 0 | 0 |
| `src/server/cluster.c` | 24 | 0 | 1 |
| `src/server/dest_fault.h` | 0 | 0 | 1 |
| `src/server/efsd.c` | 6 | 64 | 1 |
| `src/server/handler.c` | 9 | 0 | 0 |
| `src/server/packer.c` | 11 | 0 | 2 |
| `src/server/peer_pool.c` | 1 | 0 | 0 |
| `src/server/put_orphan.c` | 1 | 0 | 0 |
| `src/server/put_upload.c` | 4 | 0 | 2 |
| `src/server/raft_host.c` | 201 | 8 | 20 |
| `src/server/reclaim.c` | 5 | 0 | 0 |
| `src/server/recovery.c` | 20 | 0 | 1 |
| `src/server/segment_rebuild.c` | 13 | 0 | 0 |
| `src/server/segment_store.c` | 2 | 0 | 1 |
| `src/server/storage_roots.c` | 0 | 6 | 0 |
| `src/server/store.c` | 11 | 7 | 0 |
Not counted: stdout lines (`printf` in `store.c`, `cluster.c`, one listening line in
`efsd.c`: startup and join messages, rare); the 11 sites that already used
`efs_telemetry_log` before this change; `log_ts.c` (the timestamp stream
itself). `src/mgmt`, `src/client`, `src/bench` are not daemon code. Reproduce the
count: `git grep -n 'fprintf(stderr' -- src/server src/raft src/kv src/meta src/common`
and check each hit has a `sync:` comment line above it.

Known limit: a routine line emitted after the reporter stops (peer-pool and
storage-root close phases) is synchronous again; those phases have no
log sites in the loop paths.

## Test

`tests/test_stop_blocked_stderr.py` (`make test-long`, budget 60 s, TSan and
ASan lists in `tests/sanitizer_check.sh`): three-daemon TCP cluster, FUSE mount,
two rewriting writers for 15 s under the W159 10 ms fsync interposer, every
stderr a 4 KiB pipe nobody reads (unread output in the pipe at SIGTERM is asserted),
checked unmount, SIGTERM to all three, each must exit rc 0 within 55 s, no core.
`pump_logging_profile.py` now signals all daemons together and records `stop_s`.

## Results

Host intel2 (8 CPU, Rocky 10), build gcc `-O3 -march=native`; raw outputs in
`~/efs-artifacts/taskBP-20261031/` on intel2 (not committed).

| Tree | Result |
| --- | --- |
| devel `909a07d71` (+ the new test and profile script) | **FAIL**: `daemon stop rc 3` (forced exit after 55 s) |
| this branch, 3 runs | **PASS**, rc 0 for all three daemons; exit 5.4 s / 0.4–0.5 s / 0.5–5.3 s after SIGTERM; wall 45, 39, 45 s |

Gate (`TMPDIR=$HOME/tmp EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, intel2, idle apart from other sessions' leftover processes):

| Run | Tree | Gate line |
| --- | --- | --- |
| 1 | first conversion | TOTAL 582 FAIL 50 NOTRUN 10 STATUS FAIL (1672 s): 41 harness tests that compile pieces of `raft_host.c` standalone lacked `efs_telemetry_log` (stubs and link lists added), plus `k36_*`/`k33_*` losing a startup line (below) |
| 2 | stubs + producer fix | TOTAL 582 FAIL 1 NOTRUN 10 STATUS FAIL (966 s): `test_w155_report_resolve` (same missing stub; passes alone after the fix) |
| 3 | final tree | TOTAL 582 FAIL 1 NOTRUN 10 STATUS FAIL (835 s): `test_k37_segment_rebuild` timed out waiting for segments under 8-job load (the known K37/K82 load dependence, Task BM); PASS alone twice (158 s each) and PASS in run 2. **0 FAIL is not claimed.** NOTRUN 10 are host limits (no RDMA device, no valgrind headers). No test is over budget. |

A real defect found by the gate and fixed here: the first version dropped
routine lines when two threads logged at once (producers used a trylock on the
binding and mailbox locks), which lost the one `raft-host: up ...` line the
k33/k36 harnesses wait for. Routine lines now wait for the mailbox copy lock
(never held across I/O or a wait); the pump's periodic records and printf lines
keep the trylock. Sanitizers (intel2): TSan `telemetry` PASS 3 s, `pump_logging`
PASS 72 s, `stop_blocked_stderr` PASS 39 s, 0 reports; ASan+UBSan
`pump_logging` PASS 69 s, `stop_blocked_stderr` PASS 43 s.

Raw outputs: intel2 `~/efs-artifacts/taskBP-20261031/` (`SHA256SUMS` sha256
`22f9c14a8c2b3660ea8e581ac74e71992123a18ba1ddcd3978425455576e6ea7`).

Merge note: Task BL (branch `claude/bl-report-stall-b59547`, K81) converts seven
REPORT/receipt sites in `raft_host.c` to the same call and stubs
`efs_telemetry_log` in `test_w155_report_resolve.py`; expect trivial conflicts on
those lines and that test (this branch converts the same sites with the same format).

## Limits

- Critical-event retention is as before: routine lines are counted, not kept,
  when the mailbox overflows (32 slots). Class (b) lines are never dropped.
- Timestamps on routed lines are taken when the reporter writes, not when the
  event happened (a few ms under a healthy sink, later under a slow one).
- No RDMA or multi-host run.

## Reproduce

```sh
make -j8 efsd efs-fuse efs-mgmt tests/faults/efsd
TMPDIR=$HOME/tmp python3 tests/test_stop_blocked_stderr.py
# negative control: the same script and scripts/pump_logging_profile.py in a tree at 909a07d71
EFS_SAN_ONLY="stop_blocked_stderr pump_logging" bash tests/sanitizer_check.sh tsan
EFS_SAN_ONLY="stop_blocked_stderr pump_logging" bash tests/sanitizer_check.sh asan
```

## Wave 47 merge note

The wave 47 gate (intel2, `fa5abd5b2`) failed `tests/test_k33_growth.py`: the
`membership: committed ... as member 4` line was queued on the reporter and the
`EFS_FAULT_MEMBER_EXIT` hook ended the process before the reporter wrote it, so a
fault-hook exit lost the routine lines queued before it. `efs_fault_exit()` now calls
`efs_telemetry_flush()` first: it waits until the bound reporter has written what was
queued, at most the 200 ms drain, so a blocked sink never holds the exit longer and a
"crashed" node answers at most that much longer (`tests/test_telemetry` checks the flush;
`test_k33_growth` is the regression test). The `test_w155_report_resolve.py` stub this
branch added duplicated the one Task BL merged in wave 46 and was dropped.
