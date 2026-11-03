# K54 member-down fence: harness port race (Task CS)

Task CS, Nov 3 2026, node9900 (192 CPU, XFS, TCP, buffered I/O). Purpose: stop `tests/test_k54_member_down_fence.py` failing with `efsd n4 exited rc=1: bind failed for 127.0.0.1:NNNNN: Address already in use` (seen in wave 53's gate on fstor007 and node9900). Verdict: **PASS (harness defect fixed)**. This is a test-harness defect, not a product defect, so there is **no K row**: no `src/` change, nothing blocks alpha.

## Root cause

The K54 test does not use `tests/k33_daemons.py` (whose `hold_port` already holds its ports for the life of a `Cluster`). It uses `ticket_put.Cluster` (`tests/live/ticket_put.py`), which chose its member ports with `free_base()`: bind-probe four consecutive ephemeral ports, then **release them all**. Nothing held them afterwards, so a member that was SIGKILLed or stopped and restarted (or the joiner starting after the seed) could find its port taken by another process's outgoing connection (the kernel hands ephemeral ports to `connect()`), and efsd failed to bind.

## Fix

`tests/live/ticket_put.py`: `free_base()` now returns `(base, held)` and `Cluster` keeps one SO_REUSEADDR socket bound (not listening) per member port from construction until `close()`, across kills and restarts. Held reuse-addr ports are skipped by the kernel for `bind(0)` and outgoing `connect()`; efsd binds its listener with SO_REUSEADDR too (`src/common/network.c`) and listens beside the held socket. An explicit `base=` is held the same way. `free_base` had no other caller. No timeout widened, no retry added. All other users of `ticket_put.Cluster` (`test_live_ticketed_put.py`, the `tests/live/*` smokes) get the hold.

## Source and environment

Source: devel `ab264d99e` plus commit `71a783375` (the `ticket_put.py` change), shipped with `git archive HEAD` to `/tmp/efs-taskCS`. Build: `make` with `PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig` (private libfuse 3.16.2; the system fuse3 lacks `max_threads`); the gate ran with `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse` after `make clean`. node9900, 8 job gate. The user's live orcd-mini client, ports 17432-17435 and /data1 were not touched; work used `/tmp/efs-taskCS` and ephemeral loopback ports only.

## Reproduction

```
tests/live/port_churn_loop.sh 30 OUTDIR     # 8 RST-closing loopback churners + 30 runs of the K54 test
```

Run it once with devel's `ticket_put.py` and once with the fixed one. (The recorded loops used an equivalent inline script; this file is its committed form.) Gate: `TMPDIR=/tmp/efs-taskCS-tmp EFS_TEST_JOBS=8 EFS_FUSE_PREFIX=/data1/efs/deps/libfuse bash scripts/make_test_summary.sh`.

## Results

| Harness | K54 test with 8 port churners, 30 runs |
|---|---|
| devel `ticket_put.py` | **24 of 30 pass**; 5 failures are `efsd n4 exited rc=1 ... Address already in use` (ports 33300, 50618, 45974, 55356, 34350) |
| fixed `ticket_put.py` | **30 of 30 pass** |

The sixth devel failure (run 3) was a different assertion, `the returned member stored a revoked-epoch fragment`; it did not recur in the fixed 30 and is not explained here (limitation: unexamined, possibly a separate K54 timing issue; open if it appears again without churn). A first, heavier churn (plain closes, TIME_WAIT everywhere) exhausted the port range and failed both harnesses at `no free consecutive ports`; it was discarded.

Full gate with the fix, clean build: **`TOTAL 584 FAIL 0 NOTRUN 4`** (NOTRUN: cluster stop scripts, RDMA Memcheck, RDMA live qualification, leak gate; no valgrind and no cluster scripts on node9900). Earlier gate samples on the same tree: one had `FAIL 2` (`test_d4_plateau`: `efs-fuse` loaded the system libfuse, `FUSE_3.12 not found`, because the gate lacked the private libfuse in the library path; `test_store_durability` `test_barrier_backoff`), and one after `make clean` had `FAIL 1` (`test_barrier_backoff`, a 1.2 s timing window; passes solo 3 of 3). Neither test touches `ticket_put.py`; both are load-sensitive and tracked outside this page.

## Limitations and remaining gates

Only `ticket_put.Cluster` was changed; `k33_daemons.py` already held its ports. Other ad hoc `free_port()` users (`test_shutdown_*`, `test_k33_autoid`, `test_settings_listing`) pick a port for a single immediate use and were not changed. Hosts other than node9900 were not run.

## Artifacts

Raw loop logs, gate `summary.txt`/`progress.txt`/`gate.log` and `fail-*.log` stay outside Git on node9900 in `~/efs-artifacts/taskCS-20261103/` (26 files); `SHA256SUMS` there has sha256 `15ff50eff57daa7093691b2a93946aae45fb7fa864525a80a333a4eba055f2a0`. Cleanup done: `/tmp/efs-taskCS` and `/tmp/efs-taskCS-tmp` removed.
