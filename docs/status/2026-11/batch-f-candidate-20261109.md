# Batch F candidate regression on fstor007, November 9 (Task EX)

**Verdict: FAIL on F1, PASS on F2, F3, F4 (rerun) and F5.** F1 (`make test-all`, 8 jobs) ran twice on one build and failed once in each run, on a different load intermittent each time: run 1 `test_pump_logging.py` (48 writes, floor 50, no errors), run 2 `test_bench_cli.py` (4 of 256 `efs-bench` workers did no I/O within the 1 s window). Each test passed alone 3 of 3 and passed in the other gate run. Neither run reached 0 FAIL, so the rows served by F1 alone stay FIXED. F2 ASan 59/59 and TSan 31/31, F3 Memcheck 41/41. F4 failed its first driver run in a harness listing race after the K11 case had passed; the driver rerun passed both gates and the direct K11 case passed 3 of 3. **K56 and K73 become VERIFIED**; no new K row (no failure is attributed to the product).

**Purpose:** Stage 1 batch F ([acceptance matrix](../stage1-acceptance-matrix-20261030.md), runs F1 to F5) on the frozen candidate, judged against each served row's acceptance ([definitions](../2026-10/acceptance-definitions-20261031.md)). Release gate: alpha 1.0 Stage 1, [K register](../alpha-1.0-2plus1-checklist.md#k-stage-1-known-defects-in-the-alpha-scope).

**Source and build:** devel `9bb50fe7932d2b487d0d5eed1294b003cb735497` (wave 67), `git archive --prefix=efs-taskEX/ 9bb50fe79 | gzip`, sha256 `090c3e4162db83e245d1f0026a631186519ddc53d265b42faa37470f05ee6d43` on both ends, extracted to `/tmp/efs-taskEX` (F1 to F4) and `/tmp/efs-taskEX-f5src` (F5). No `.git`: `efsd --version` prints `efsd (no git metadata, branch unknown, built 2026-11-09T22:14:44Z on fstor007-mgmt)`; `efsd` sha256 `922c29b07e74a7d6…`, `efs-fuse` `052adefa8b835c86…`. No source change.

**Environment:** fstor007 (ORCD), 96 CPUs, 754 GiB, RHEL 9.7, kernel 5.14.0-687.36.1, gcc 11.5.0 (no clang on the host), Python 3.9.25, private libfuse 3 through `EFS_FUSE_PREFIX=/tmp/efs-taskEX-fuse` (shim of `lib64` and `include`; `efs-fuse` links `/tmp/efs-taskEX-fuse/lib64/libfuse3.so.3`), `/tmp` XFS, `ptrace_scope` 0, TCP loopback, direct fragment I/O (the only mode), `TMPDIR=/tmp/efs-taskEX-tmp/<lane>`, valgrind 3.28.0 from `~/.local` (the system one is 3.26.0). Run in detached screens (`SCREENDIR=$HOME/.screen`). F1 ran alone; F5 ran next to F2, F3 and F4 (8 decoder jobs on 96 CPUs).

## Runs

| Run | Command | Wall | Result |
| --- | --- | ---: | --- |
| F1 run 1 | `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` | 11 min 27 s | `TOTAL 594 FAIL 1 NOTRUN 3 STATUS FAIL (673s) HOSTLIMIT 3 BUILD 13s`; FAIL 525 `test_pump_logging.py` |
| F1 solo | `python3 tests/test_pump_logging.py` x3 | 84 to 88 s each | PASS 3/3 |
| F1 run 2 (same build) | same | 10 min 56 s | `TOTAL 594 FAIL 1 NOTRUN 3 STATUS FAIL (655s) HOSTLIMIT 3 BUILD 0s`; FAIL 568 `test_bench_cli.py`; `test_pump_logging.py` PASS (87.6 s) |
| F1 solo | `python3 tests/test_bench_cli.py` x3 | 3 s each | PASS 3/3 (it passed in run 1 too, 2.9 s) |
| F2 ASan | `bash tests/sanitizer_check.sh asan` | 7 min 26 s | `VERDICT: PASS (59 run, 0 failed, 386s)` |
| F2 TSan | `bash tests/sanitizer_check.sh tsan` | 10 min 47 s | `VERDICT: PASS (31 run, 0 failed, 632s)` |
| F3 | `PATH=$HOME/.local/valgrind-3.28.0/bin:$PATH CPATH=$HOME/.local/valgrind-3.28.0/include make leak-check` | 5 min 16 s | `VERDICT: PASS (41 run, 0 failed, 310s)` |
| F4 run 1 | `python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json --topology loopback-5 --gates gc-reclamation,fragment-integrity --allow all --out <dir> --port-base 18780 --private-port-base 18700` | 2 min 6 s | `fragment-integrity` PASS (13 s); `gc-reclamation` FAIL: `--posix` part PASS, `gc-put-barrier` case lines PASS, then the harness crashed (below) |
| F4 direct K11 | `python3 tests/live/gc_reclamation.py --put-barrier-fault --root $D/p1 --root $D/p2 --port 1875<i>` x3 | 52 to 53 s each | PASS 3/3 |
| F4 run 2 | the driver command again | 2 min 30 s | `fragment-integrity` PASS (14 s), `gc-reclamation` PASS (128 s); driver exit 3 NOTRUN only because `identity` was not selected, as on the [K11 page](../2026-10/k11-put-barrier-case-20261031.md) |
| F5 | `python3 scripts/decoder_check.py --jobs 8 --out /tmp/efs-taskEX-tmp/f5` (gcc) | 2 h 0 min | rc 0, 26 of 26 targets PASS (24 record targets at 1,200 s, `declaration` and `apply` at 7,200 s); `apply` covered 10,839,021,930 cases |

The three F1 NOTRUN rows are host limits in both runs: `test_cluster_stop.py` (`EFS_CLUSTER_SCRIPTS` absent), `test_rdma_memcheck.py` (`valgrind/memcheck.h` not installed for the system build), `test_qualify_rdma_live.py` (RDMA hardware runs NOTRUN here). No OVERBUDGET row in either run.

## Failures

1. **F1 run 1, `test_pump_logging.py`:** `assert r['verdict'] == 'PASS' and r['writes'] >= 50` failed with verdict PASS, `errors: []`, `writes: 48`, write p50 1.38 s, p99 2.08 s, under the deliberate 40 ms serialized fsync and the 8-job pool. Daemon n1 logged 32 and n3 4 `report-split` lines (REPORT finish 439 to 737 ms, all `rc=0`). No EIO, no payload error, so it is not the K81 failure (a REPORT reply withheld behind a blocked sink, turned into EIO). The passing fixtures were cleaned by their tests, so there is no side-by-side comparison. Class: unattributed, load. Not raised, not patched.
2. **F1 run 2, `test_bench_cli.py`:** `efs-bench --bench io --rw write --io-size 4K --qd 256 --window 1 --time 1` printed `4/256 workers completed no I/O before the deadline` (`max_start_us=1000757`, 70 M ops by the other workers). A 1 s run under the pool does not give 256 threads time to start. Class: test sensitivity to load; the bench tool's own fail-closed check worked.
3. **F4 run 1, `gc-put-barrier`:** after `failed server PUT barrier: no publication ACK, retained bytes, recovery and exact cold bytes PASS` and nine `span-only truncate then physical reclaim PASS` lines, `tests/live/gc_reclamation.py` `files()` (line 559, `folder.glob('*/*')`) raised `FileNotFoundError` on an export directory that physical reclaim removed between the listing and its `scandir`, inside `wait(lambda: not files(tail_ino), ...)`. The harness races the very deletion it waits for. Class: harness defect (TOCTOU in the helper). The driver deleted that run's work tree, so its daemon logs were not kept; the K42 and `rc=-9` scans below cover run 2 and the direct runs.

None is a product defect on this evidence, so no K row was added. Items 2 and 3 are harness follow-ups (below).

## Served K rows

| K | F acceptance (definition) | Met? | Row change |
| --- | --- | --- | --- |
| K3, K7, K24, K25, K30, K57, K58, K59, K60, K65, K67, K68, K71, K76, K77, K78 | candidate regression: F1 `make test-all` with no failure | **No**: both F1 runs FAIL (593 of 594 PASS each), on tests outside these rows | FIXED kept, dated clause |
| K2 | F1 `test_decoder_inputs.py`, `test_decoder_refusal.py`; F5 25/25 rc 0 | F1 PASS both runs; F5 26/26 rc 0 (gcc, not clang) | VERIFY kept (c5 cells, BT-1) |
| K4 | F1 `./tests/test_declaration_gate`, `test_declaration_gate_handler.py` | PASS both runs | VERIFY kept (c5 cells) |
| K11 | F4 failed-PUT-barrier case, no `fetch published .* rc=-9` | case PASS in driver runs 1 and 2 and direct 3/3; `put-barrier-fault.json` `fsync_errno` null, 5, 5 and the refusal line in three of four `n*.log` in each direct run; scan prints nothing | VERIFY kept (cells, BT-2) |
| K16 | F1 `test_frag_record`, `test_store_durability`, `test_store_gc`, `test_get_integrity.py`; F4 `fragment-integrity` | PASS both F1 runs (XFS TMPDIR, so `test_store_durability` ran); `fragment-integrity` PASS in both driver runs | VERIFY kept (RDMA cells, BT-3) |
| K19 | F1 `test_reclaim`, `test_store_gc` (both modes), `test_meta_apply`; F4 `gc-reclamation` | PASS both F1 runs; `--posix` part PASS in both driver runs, including `offline fourth member retains exact durable reap ledger`, restart drains and empty physical inventory | VERIFY kept (BT-5 not decided) |
| K26 | F3 `make leak-check` | PASS 41/41 on an ORCD host with valgrind 3.28.0, the ORCD leak run the row listed as NOTRUN | FIXED kept (O9 owed) |
| K42 | F4 loopback-5 `gc-reclamation` | PASS in driver run 2; no `revoke rc=-13` in run 2 or the direct runs | VERIFY kept (H4) |
| K45 | F1 `./tests/test_raft` (`test_snapshot_rejects_delayed_compacted_prefix`) | PASS both runs; the unfiltered binary runs every `RUN(...)` case, this one included (`tests/test_raft.c:3706`) | FIXED kept (H15) |
| K46 | F1 `./tests/test_raft` resync-election cases | PASS both runs (`test_resync_fences_elections`) | FIXED kept (H3) |
| K47 | F1 `test_get_integrity.py`, `test_k36_segment_recovery.py` | PASS both runs | FIXED kept (H4, H5) |
| K51 | integrated-candidate TSan/ASan | TSan `heartbeat_shutdown` and the daemon entries PASS, ASan 59/59 | VERIFY kept (O8 RDMA) |
| K56 | F2 ASan on the candidate, the five startup entries | PASS: `env_removed`, `k36_recovery`, `k36_segment_recovery`, `k37_segment_rebuild`, `d4_progress` among 59/59 | **VERIFIED** |
| K72 | F1, F2 TSan, H13 | TSan `pump_logging` and `stop_blocked_stderr` PASS; F1 run 1 `test_pump_logging.py` FAIL on the write floor (48 of 50, no errors), PASS solo 3/3 and in run 2 | FIXED kept (H13, F1) |
| K73 | F2 TSan on the merged tree exercising the counter | PASS, 0 reports: the `reclaim` entry (`tests/test_reclaim`) runs `wait_readmitted()` (`tests/test_reclaim.c:311`), which polls `server_reclaim_status()` (reads `rj.recovered`, `src/server/reclaim.c:1227`) while the recovery thread's `recover_flush()` increments it (`:1023`) | **VERIFIED** |

No sanitizer entry printed NOTRUN or SKIP (grep over every `*.out`).

## Follow-ups (not done here)

- F1 at 0 FAIL: a third gate on this build, or the coordinator's acceptance of the two load intermittents, would let the 16 F1-only rows become VERIFIED. Not run unasked.
- `tests/live/gc_reclamation.py` `files()`: tolerate a directory removed between listing and scan (the state it waits for). Harness fix; no W identity filed by this task.
- `tests/test_bench_cli.py` many-workers case: 256 workers in a 1 s window under the 8-job pool.
- F5 used gcc 11.5 where the K2 definition names `CC=clang`; fstor007 has no clang.

## Reproduction

```bash
git archive --format=tar --prefix=efs-taskEX/ 9bb50fe79 | gzip > efs-taskEX.tar.gz   # ship, check sha256
tar xzf efs-taskEX.tar.gz -C /tmp && cd /tmp/efs-taskEX
mkdir -p /tmp/efs-taskEX-fuse && ln -s ~/orcd/scratch/efs/orcd-test/deps/libfuse/lib /tmp/efs-taskEX-fuse/lib64 \
  && ln -s ~/orcd/scratch/efs/orcd-test/deps/libfuse/include /tmp/efs-taskEX-fuse/include
export EFS_FUSE_PREFIX=/tmp/efs-taskEX-fuse LD_LIBRARY_PATH=/tmp/efs-taskEX-fuse/lib64 TMPDIR=/tmp/efs-taskEX-tmp/<lane>
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh                    # F1; summary.txt, timings.tsv, tmp/fail-*.log
EFS_SAN_LOGDIR=$TMPDIR/logs bash tests/sanitizer_check.sh asan       # F2 (then tsan)
PATH=$HOME/.local/valgrind-3.28.0/bin:$PATH CPATH=$HOME/.local/valgrind-3.28.0/include make leak-check   # F3
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json --topology loopback-5 \
  --gates gc-reclamation,fragment-integrity --allow all --out $TMPDIR/out --port-base 18780 --private-port-base 18700   # F4
python3 scripts/decoder_check.py --jobs 8 --out $TMPDIR/f5           # F5, separate extracted tree
```

Prerequisites: Linux, gcc with the ASan/UBSan/TSan runtimes, fuse3, valgrind 3.28.0 for F3, an XFS TMPDIR for `test_store_durability`. The tests choose their own daemon ports (ephemeral ports were seen in F1, not only 18xxx/19xxx); the F4 driver uses 18700 to 18799. Cleanup: the harnesses stop their daemons and unmount; delete the trees, `/tmp/efs-taskEX*` and the tar.

## Raw evidence

On fstor007 `~erbmi1/efs-artifacts/taskEX-20261109/` (outside Git): `f1-run1/` and `f1-run2/` (`summary.txt`, `progress.txt`, `timings.tsv`, `fail-*.log`, gate log and time), `f1-run1/pump-logging-fixture/` (the failed fixture's daemon logs and result), `f1-solo/`, `f1-test_raft.log`, `f2-asan.log`, `f2-tsan.log`, `f2-asan-logs/`, `f2-tsan-logs/` (per-entry output), `f3-leak.log`, `f4-run1/` (driver log, gate logs, draft summary), `f4-rerun/` (direct runs with `n*.log` and `put-barrier-fault.json`, driver run 2), `f5-decoder.log`, `f5-results/` (per-target `run.log`, `summary.json`; `apply/run.log` kept as its last 200 lines plus the full file's sha256 `3cac3c4633da3606…`), `PRUNED.txt`. `SHA256SUMS` there: 326 files, its own sha256 `41428357cf08f13d201c8d2090901a65f7718511efbc104cda608ebabfdf58e3`.

## Limitations

One host, single-host loopback, TCP only. F1 rerun once, as the brief allows; a passing third gate was not sought. F5 overlapped F2 to F4. F4 run 1's work tree is gone, so its K42 and log scans are missing. The F1 pump-logging failure is not attributed beyond "load"; the passing runs left no fixture for comparison.
