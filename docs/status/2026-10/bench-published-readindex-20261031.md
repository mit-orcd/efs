# `test_bench_published` "rc=-4" failures at 24 copies: a lost write, not a read-index timeout (K88)

Task CJ, Oct 31 / Nov 1 2026. Purpose: attribute the second failure class Tasks CE and CF counted in [bench-published-enospc-20261031.md](bench-published-enospc-20261031.md) and [k86-session-admit-race-20261031.md](k86-session-admit-race-20261031.md) (`bench worker rc=-4`, daemon `mkdir ... rc=-13 stage=3`, or all three workers `-4`). Gate: F1 (`tests/test_bench_published.py`) and the K register. Verdict: **FAIL (product defect, K88 OPEN)**; the "read-index/timeout class" does not exist as such.

## Result in one paragraph

There are two different things in those logs. (1) `rc=-4` is `EFS_ERR_EXIST` and is the **expected** output of the test's own name-collision control (`bench(... '--id', 170, ok=False)`): it is in every log, passing or failing, so counting `rc=-4` logs in a directory that keeps only failing logs counted every failure. (2) `mkdir ... rc=-13 stage=3` is `EFS_ERR_BUSY` from `host_read_index_pair` after its 400 ms bound while Raft group 2 was still electing right after formation; the client retries it and it succeeds (`inode-rpc: slow-ok type=45 attempts=3 saw_busy=1 status=0`). It is a load correlate (present in 10 of 20 failing logs at 24 copies), not the cause. The real failure, in all 19 failing logs of this task and in the three CE logs, is the **assertion at `tests/test_bench_published.py:58`: a cold FUSE read returns the correct length but not `b'A'*want`**. The writer `efs-bench` printed `BENCH_OK` and exited 0 although the client had logged `report-stuck ... not re-sent, writeback error` for one chunk: the chunk was dropped.

## Mechanism (client, `src/client/write.c`)

- `efs-bench` writes 3 files from 3 threads in one process through the library client (no FUSE) and ends with `efs_client_report_dirty_ino(ino, 1)`.
- A background flush (`meta_flush_main`, kicked after `REPORT_LANDED_CHUNKS` landed PUTs, 10 ms coalescing) sends an **async** REPORT (`sync=0`) of a chunk that the writer has only half filled (`report-irec size=65536`).
- The writer's final **sync** REPORT then carries the full chunk with `base_gen=0`. The slot's base moves to the async object only after that async REPORT has returned (`dcache_note_committed` after the RPC, `src/client/write.c:2594`). If the host has already applied the async object, the sync REPORT answers `STALE` (the client lost to itself).
- The STALE path (`stale_repull_replay`) classifies the chunk as a loser (`committed=<async gen> ours=<final gen> done=0`), replays, and the rebuilt record is again `base=0` and identical. `report_rec_stuck` (`write.c:2036`) fails a record closed after two identical STALE rounds: it prints `report-stuck`, records an inode writeback error and drops the record. The REPORT then returns `EFS_OK` with the chunk missing. FUSE would surface the recorded error at fsync/close (EIO); `efs-bench` has no close/fsync check and `efs_client_shutdown` does not read it, so the benchmark succeeds with lost data.
- Not established: why the second round's record still has `base=0` although `dcache_note_committed` sets the base after the async ack (an ordering or slot-recreation question in the dcache lifecycle, not answered by the client trace, which has no timestamps). That is the work item for the K88 fix.

## Evidence (raw output is outside Git, see Artifacts)

- Every failing log has the same shape. `dbg-par24/r13-2.log` (client trace on):
  - `report ino=5520 ci=0 only=5520 sync=0 rc=0` then `report-irec ino=5520 size=65536 only=5520 sync=0 rc=0` (the async partial-chunk REPORT, issued before the writer finished);
  - `dcache ino=5520 ci=0 stale-class committed=b6cd47418cb5724c ours=1adcd2c1ae8df1b1 done=0 base=0` and `report-stale: chunks=6 runs=1 committed=5 replayed=1` (five chunks fine, ci=0 a loser), again `report-stale: chunks=1 ... committed=0 replayed=1`;
  - `report-stuck ino=5520 ci=0 gen=1adcd2c1ae8df1b1 base=0 committed=b6cd47418cb5724c span=0+0 stale_rounds=2: not re-sent, writeback error`;
  - `BENCH_OK kind=write ... rc=0` for the same bench and `AssertionError: (0, 699074, 699074)` in the cold-reader check.
- In all 9 traced failures the stuck chunk's inode had an async (`sync=0`) REPORT of that chunk in the same log. In 5 solo traced runs (no load) there is no `sync=0` REPORT at all and the test passes.
- CE's three logs (`taskCE-20261031/par24/r4-6`, `r4-14`, `r8-15`, intel2) have the same `report-stuck` line (`ino=5318 ci=2`, `ino=6650 ci=4`, `ino=4842 ci=1`) and the same `AssertionError`. `r4-14` and `r8-15` also have `mkdir ... rc=-13 ... stage=3` (`02:27:51.148Z`, `02:28:30.717Z`), one second after the cluster formed (`raft: group=2 became leader term=1` at `02:27:51.209Z`, i.e. after the refused mkdir); `r4-6` has no `stage=3` line at all.
- `stage=3` is the `host_read_index_pair` call in `server_raft_host_mkdir` (`src/server/raft_host.c`, `stage = 3`); the bound is `HOST_READ_TRIES` x `HOST_TICK_US` = 80 x 5 ms = 400 ms (`raft_host.c:59,95`); `-13` is `EFS_ERR_BUSY`, `-4` is `EFS_ERR_EXIST`, `-28` is `EFS_ERR_UNREGISTERED` (`include/efs/common.h`).

## Rate against load

Fixture: `tests/test_bench_published.py` (3 nodes, 2 MiB + 68 B, 3 writers), N copies per round, each with its own `TMPDIR`. A failure is a non-zero test exit; every one of them was a `report-stuck` + `AssertionError`.

| Copies | Runs | Failures | Rate | Load average | Notes |
|---|---|---|---|---|---|
| 24 | 1920 | 11 | 0.57 % | 25 to 38 | CE 3/240 (load 38), CJ sweep 1 0/240 (peak 25), CJ long 8/1440 (31 to 34) |
| 24, client trace on | 1920 | 9 | 0.47 % | 30 to 35 | `EFS_DCACHE_TRACE`, `EFS_REPORT_DBG` |
| 12 | 1320 | 1 | 0.08 % | 11 to 15 | |
| 8 | 1280 | 1 | 0.08 % | 7 to 11 | CE: 0/80 |
| 1 | 15 | 0 | 0 | below 2 | CE 0/10, CJ traced 0/5 |

`stage=3 rc=-13` was in 5 of 8 untraced and 3 of 9 traced failing logs at 24 copies, 0 of 1 at 12, 0 of 1 at 8. Whether it also appears in passing runs was not measured (passing logs are deleted by the script). The rate is clearly load dependent (about 7x between 24 and 8 copies) but not zero at 8 copies on 8 CPUs, so it is not a failure only reachable at an absurd load. CF's counts (7/720 before, 2/720 after its K86 fix) were made with the `rc=-4` metric and are consistent with this rate; they were not re-attributed.

## Attribution

(a) **Product defect**, but not the one the task text guessed. The read-index bound is not the cause: the BUSY is retried and succeeds, and 10 of 20 failing logs at 24 copies have no `stage=3` line. The defect is client-side: a single writer that is the only client loses a chunk to its own earlier async REPORT, the STALE loop does not converge because the rebuilt record is identical, and the stuck-record guard turns that into a dropped chunk with a successful REPORT. (b) holds only for the `stage=3` BUSY (transient election window at cluster start under CPU starvation, retried by the client): not worth a row. (c) harness: the test is correct (it caught the loss); two harness faults were found and fixed: the `rc=-4` counter in `scripts/bench_published_par.sh` (now counts `report-stuck` and `stage=3 rc=-13`), and the `efs-bench` write path that ignores the recorded writeback error (left, see K88).

Register: **K88** OPEN, blocks alpha: **yes** (proposed; the user may downgrade). A fix needs the dcache base/ack ordering understood first, which is more than one shared-code change with its own check, so it is left OPEN.

## Reproduction

Prerequisites: a built tree (`make efsd efs-fuse efs-bench efs-mgmt`), FUSE usable (the test prints NOTRUN otherwise), 8 CPUs; on intel2 outside `~/efs-merge*`, ports from the test's own free-port choice (not 17432-17435). Source: devel `8e53122cd` plus the two Task CE/CF commits (`f76ce78e9`, `2a0c257a1` on this branch) plus the counter change in `scripts/bench_published_par.sh`. Build identity: `EFS_BUILD_ID` unknown (plain `make`, gcc -O3, intel2 build `2026-11-01T04:28:29Z`).

```sh
export TMPDIR=$HOME/efs-taskCJ/tmp; mkdir -p "$TMPDIR"
sh scripts/bench_published_par.sh OUT/par24 24 60     # about 10 min; expect ~0.5 % failures
sh scripts/bench_published_par.sh OUT/par12 12 100    # about 15 min
sh scripts/bench_published_par.sh OUT/par8 8 150      # about 21 min
# trace variant: add EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1 to the test's env (a local edit after the
# `env['EFS_TRANSPORT']` line), then run the 24-copy line
```

Checks: `summary.txt` must show `fail` equal to `report-stuck logs` and `rc=-28 logs 0`. Each failing log must contain `report-stuck` and `AssertionError: (<worker>, <len>, <len>)`. Cleanup: `rm -rf "$TMPDIR"`; no daemon or mount survives a run. Wall time per run 7 to 9 s (max 11 s).

Timings and `uptime` (intel2, 8 CPU): sweep 1 24 copies 04:28:54Z to 04:30:37Z (load 1.1 to 25.1); 12 copies to 04:32:06Z (14.9); 8 copies to 04:33:33Z (7.1); long 24 copies 04:33:49Z to 04:43:57Z (31.5, 5-min 34.4); 12 copies to 04:58:44Z (11.5); 8 copies to 05:19:50Z (8.2); traced 24 copies 05:20:05Z to 05:33:33Z (34.5, 15-min 29.7). The coordinator's merge-gate trees were not used or touched.

## Limitations and remaining gates

- No fix. The exact reason the second round still has `base=0` is unproven. Verdict stays FAIL until K88 is fixed and `test_bench_published` passes 3000 consecutive 24-copy runs (the observed rate would give about 17 failures).
- Only intel2 (8 CPU, TCP) was used; RDMA and FUSE-writer behaviour were not tested. With FUSE the recorded error reaches fsync/close as EIO; that was not run.
- The traced runs add stderr output and may shift the timing (rate 0.47 % vs 0.57 %, not distinguishable).

## Artifacts

Raw output: intel2 `~/efs-artifacts/taskCJ-20261031/` (read only; 46 files: failing logs per sweep, `results.tsv`, `summary.txt`, `run*.log` with `uptime`, `build.log`, the driver scripts and the trace-variant test). `SHA256SUMS` in that directory has sha256 `380d716cbc0cf623af4dac6953128429e147fffc74b59b918ba3db82ade9461b`. Inputs read: `~/efs-artifacts/taskCE-20261031/par24/r4-6`, `r4-14`, `r8-15`.
