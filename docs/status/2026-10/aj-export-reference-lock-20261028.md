# AJ — export acquire/release under the server lock on every GET and PUT (2026-10-28)

Coordinator report: branch `agent/devel-aj`, head `9c4bfd171ccc5450ae852a2597351150e543730a`
(docs-only branch; measurement-only task, no product change). Gate on intel1:
`TOTAL 546 FAIL 1 NOTRUN 12 STATUS FAIL (785s) OVERBUDGET 4` — the one FAIL is
`test_b6_unavailable.py` slow mode, an **untouched-base** failure (the deployed
tree is unmodified devel; the solo base rerun reproduces it deterministically),
registered as **K55**. Evidence: this page; raw data intel1
`~/efs-artifacts/taskAJ-20261028/`. NOTRUN: RDMA hosts (no device on intel1;
the mechanism is transport-independent — the lock section is identical on both
channels). Product defects found: one pre-existing on devel (K55, B6
slow-member fsync EIO at the 60 s deadline, 3/3 deterministic); none from this
task's mechanism. No product change.

**Verdict: PASS — measured, no change.** The export acquire/release critical
sections never exceeded **0.021 % wait share** of `s->lock` at the highest load
(64 connections, ~59k GET/s, two acquisitions per GET); the task threshold for
touching the lock is 5 % at 32 streams or a measurable end-to-end gain.

## Purpose and checklist item

Task AJ of the Stage 1 hot-path serialization block
(`docs/status/alpha-1.0-2plus1-checklist.md`, "Stage 1 additional letter tasks —
hot-path serialization (AG–AJ)", AJ checkbox): GET in
`src/server/handler.c::server_handle_conn` and PUT in
`src/server/put_upload.c::server_put_upload` acquire an export with
`server_export_acquire_or_create_locked` under `s->lock`;
`src/server/store.c::server_export_put` takes `s->lock` again to drop the
in-flight reference. Confirmed serialization point, never measured. The task is
measure-first: a change is justified only above the threshold.

## Tested commit and build identity

- Source: devel `9c4bfd171ccc5450ae852a2597351150e543730a`, branch
  `agent/devel-aj` (no product commit; the branch carries only this page and
  the checklist/README updates).
- Measured build (instrumented, see Method):
  `efsd 9c4bfd171ccc5450ae852a2597351150e543730a (branch agent/devel-aj, built 2026-10-29T05:54:14Z on intel1)`.
- Gate build (clean tree): `efsd 9c4bfd171... (branch agent/devel-aj, built
  2026-10-29T06:36:53Z on intel1)`.

## Host and configuration

intel1 (8 CPUs), nothing else running (Task T's confirmation gate had finished;
load 0.00 before start). Each run: a fresh 3-member loopback TCP cluster
(`tests/k33_daemons.py`, the `EFS_FAULTS=1` daemon build of the instrumented
tree, `EFS_RAFT_OBS=1`, quota 4G, stores under `$HOME/efs-taskAJ/tmp`, free
loopback ports), deleted with `edelete` after each run.

## Method (measurement)

Temporary, **uncommitted** instrumentation (transformer and drivers retained in
the artifacts): `g_server->lock` acquisitions at the four export sites (GET
acquire, PUT acquire, rebuild acquire, `server_export_put`) wrapped with
wait/hold accounting in relaxed atomics plus a 2 s TSV dump thread
(`EFS_EXP_PROF_FILE`, a scratch-build-only reader; not a product setting — the
patched tree was never committed and is deleted after the run). `perf lock` is
unusable on intel1 (perf/kernel mismatch), so the in-process counters are the
primary source, as the task allows.

Loads, 60 s each, 3 repetitions, against node 1 (the formation seed, also doing
Raft leader work):

- `get` mode: N raw TCP connections (8/32/64), each thread loop-issuing
  `EFS_MSG_GET_CHUNK` (W133 `CLIENT_HELLO` declaration first), round-robin over
  1 or 8 export ids. Every GET does the full acquire → store lookup → release
  path; the two `s->lock` acquisitions per GET are the measured mechanism.
- `fuse` mode: one FUSE mount, 8/32/64 threads of 4 KiB random pwrite/pread
  (1:1, fsync every 64 writes), the real-stack GET/PUT path at export 1.

Wait share denominator: wall × daemon thread count (from `/proc/pid/status`),
so the reported shares are *lower bounds on per-thread share only if all daemon
threads were hot*; with ~33 daemon threads the aggregate share is the honest
measure of "how much wall time hot threads lose on the lock".

## Results

Raw `GET_CHUNK` stress (medians of 3; spread was < 4 % on every column):

| config | GET/s | site | acq/s | mean wait | mean hold | wait max | wait share |
|---|---|---|---|---|---|---|---|
| 1 export, 8 conns | 60,607 | acquire | 58,825 | 106 ns | 108 ns | 187 µs | 0.020 % |
| 1 export, 32 conns | 59,160 | acquire | 57,430 | 102 ns | 110 ns | 325 µs | 0.016 % |
| 1 export, 64 conns | 59,186 | acquire | 57,469 | 102 ns | 110 ns | 194 µs | 0.020 % |
| 8 exports, 8 conns | 60,526 | acquire | 58,674 | 106 ns | 113 ns | 367 µs | 0.018 % |
| 8 exports, 32 conns | 59,310 | acquire | 57,584 | 104 ns | 120 ns | 241 µs | 0.020 % |
| 8 exports, 64 conns | 59,100 | acquire | 57,278 | 106 ns | 119 ns | 177 µs | 0.021 % |
| (all rows) | | release | = acq | 93–103 ns | ~49 ns | ≤ 436 µs (one 1.8 ms outlier) | 0.014–0.020 % |

FUSE 4 KiB mixed read/write (medians of 3):

| config | mixed MiB/s | daemon CPU | acquire wait share | PUT wait share | release wait share |
|---|---|---|---|---|---|
| 8 threads | 40.1 | 89 s | 0.0003 % | 0.0002 % | 0.0015 % |
| 32 threads | 81.3 | 97 s | 0.0007 % | 0.0002 % | 0.0039 % |
| 64 threads | 15.4 (8-CPU oversubscription) | 76 s | 0.0005 % | 0.0002 % | 0.0013 % |

Observations:

- Throughput plateaus at ~59k GET/s already at 8 connections and is flat to 64;
  the limit is the per-connection dispatch/store-lookup path, not the lock:
  mean wait is ~100 ns at every concurrency and the aggregate wait share never
  exceeds 0.021 %.
- 1 vs 8 exports is indistinguishable (the bounded 16-slot table scan and the
  per-slot inflight counter cost nothing measurable).
- The worst single wait seen in 27 runs was 1.8 ms (one outlier at 8
  connections, release site); p-max across runs is otherwise ≤ 0.44 ms. No wait
  grows with concurrency — the signature of a lock that is not contended.
- The FUSE 64-thread row's throughput collapse is 8-CPU oversubscription
  (daemon CPU falls, per-op latencies rise everywhere); its lock shares fall
  too, confirming the lock is not the mechanism.

## Gate

`make test-all` on intel1 via `scripts/make_test_summary.sh`, 8 jobs, clean
(uninstrumented) tree of the same commit:

`TOTAL 546 FAIL 1 NOTRUN 12 STATUS FAIL (785s) OVERBUDGET 4 BUILD 30s`

The one FAIL is `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py`:
`slow: fsync refused: {'F0': ('EIO', 60.06), 'F1': ('ok', 8.01), ...}` — one
writer's fsync reaches the 60 s deadline with EIO while the other seven
complete in 8 s. **Base comparison:** this branch carries no product change
(the deployed tree is `git archive` of unmodified devel `9c4bfd171`), the same
FAIL occurred in the xorinox gate of the same commit (Task AG), and a solo
rerun on intel1 (`b6-rerun.log` in the artifacts) reproduced the identical
signature — the untouched base fails the same way, deterministically (3/3).
Registered as **K55** (both checklist tables, blocks alpha: yes); the kept
case directory is retained as `k55-b6-slow-case/` in the artifacts (stores and
logs, 18 MB) for whoever picks K55 up. NOTRUN 12: the nine no-RDMA-device commands, `test_cluster_stop.py`
(no `~/git/cluster`), `test_rdma_memcheck.py` (no valgrind headers),
`test_sanitizer_gate.py` (probe NOTRUN): all host limits.

## Limitations and NOTRUN

- RDMA transport NOTRUN on intel1 (no device). The measured section is the same
  code on both channels (acquire before, release after the store I/O); RDMA
  changes the reply buffer path, not the lock scope. Task AI owns the RDMA-side
  registry lock.
- Raw-GET requests are store misses (synthetic identity): the disk read of a
  hit happens outside `s->lock`, so hit/miss does not change the measured
  sections; the FUSE rows cover the hit path through the real client stack.
- No product change, so no new regression test, no budgets row, no settings
  inventory row; the invariants listed in the task (destroy admission,
  in-flight drain, geometry init once, reference balance) are untouched.

## Artifacts

intel1 `~/efs-artifacts/taskAJ-20261028/`: `measure/*.json` (one per run),
`measure/*.log`, `matrix.log`, `exp_prof_patch.py`, `exp_prof.h`,
`aj_measure.py`, `aj_run.sh` (the temporary tooling, for reproduction),
`gate.log`, `summary.txt`, `timings.tsv`, `SHA256SUMS` (manifest sha256:
`5b45638a5ccf35bc08607ae9cc71feabdfb738fe6a062a9189bb1ef39f128df1`). Scratch trees deleted after the run; the measurement is
reproduced from devel `9c4bfd171` plus the retained patch script.
