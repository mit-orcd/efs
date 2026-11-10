# Batch F harness fixes: GC fixture scan race and io bench start (Task FB)

Task FB, Nov 9 to 10 2026, fstor007, branch `claude/blissful-chaum-051bdb` from devel `2dd280cee`. Code commits `ab88399e9` (both fixes) and `24b6807e7` (io window behind the last worker start), instrumentation `28bbc0c52`, measured tree `28bbc0c52`. Purpose: the two harness defects that Task EX's batch F run found on devel `9bb50fe79` (page `docs/status/2026-11/batch-f-candidate-20261109.md` on branch `claude/hungry-liskov-47e4f2`, commit `9dbec51fd`, not merged at writing): F4 run 1 (the `gc-reclamation` gate crashed in its own helper) and F1 run 2 (`test_bench_cli.py`, 4 of 256 `efs-bench` workers did no I/O). Release gate: alpha 1.0 Stage 1 batch F ([acceptance matrix](../stage1-acceptance-matrix-20261030.md)).

**Verdict: PASS on both fixes, harness and bench-tool change only, no K row, blocks alpha: no.**

- **GC helper race:** reproduced on the old code and gone on the new.
- **io bench:** the old build failed 6 of 6 pinned idle runs and 5 of 40 pinned runs beside the gate. The new build passed 0-fail on all 78 runs of the same case, from 1 CPU to all 96.
- **fstor007 `make test`:** rc 0.
- **8-job gate:** `TOTAL 595 FAIL 1 NOTRUN 3 STATUS FAIL (662s)`. The one FAIL, `test_k41_packer_placement.py`, is a packer-placement daemon test that this change does not touch. It ran while this task's pinned benches drove the load to about 200 on 96 CPUs, and it could not be rerun solo because the host was then reserved for a merge gate (see Remaining).

No timeout, budget, `--time` value or sleep was changed.

## 1. `tests/live/gc_reclamation.py` `files()`

**Defect:** `files(ino)` listed `folder.glob('*/*')` under the inode's export directory. Physical reclaim removes those directories. In the F4 run one vanished between pathlib's listing and its `scandir`, which raised `FileNotFoundError` inside `wait(lambda: not files(tail_ino), 'span-only truncate then physical reclaim')`. The helper raced the very deletion it waits for. On Python 3.9 (fstor007), pathlib's glob catches only `PermissionError` during a scan.

**Fix:** a new `export_entries(folder)` scans one directory with `os.scandir`, skips dot names as glob did, and returns `[]` only on `FileNotFoundError`. `files()` calls it for the inode directory and again for each subdirectory, so a vanished subdirectory drops only itself and its siblings' fragments still count. Any other `OSError` still raises. The 45 `files()` callers are unchanged: they get the same list of fragment paths.

**Regression check:** `tests/test_gc_files_vanish.py` (in `make test`, budget row 30 s) compiles the real `export_entries`/`files` from the fixture source and injects `os.scandir`. It checks four cases: the resident fragments with `.sum` excluded; a subdirectory removed at its scan, where the sibling is kept; the inode directory removed, or absent, which reads as empty; and a `PermissionError`, which still raises. It fails on the old source, which has no `export_entries`.

**Race reproduction** (`scripts/evidence/fb/gc_files_race.py OLD NEW`): each version's `files()` scans for 5 s while a thread keeps creating eight subdirectories and removing the tree. fstor007, Python 3.9.25, two separate runs:

| source | run 1: scans, `FileNotFoundError` | run 3: scans, `FileNotFoundError` |
| --- | --- | --- |
| devel `2dd280cee` (glob) | 278,071, 82 | 295,824, 17 |
| this branch | 125,894, 0 | 62,133, 0 |

## 2. `efs-bench --bench io` worker start

**Which side was at fault:** the bench tool. The test's many-workers case (`--qd 256 --window 1 --time 1`, `idle_workers == '0'`) asks for what the tool promises: every worker takes part in the window. Before this task the tool fixed the deadline at release and then woke the workers one by one from the main thread, each through its own mutex and condition variable (`802a615fe`, Oct 6). That gave two ways for a worker to lose its whole share of a 1 s window: the release loop itself, and a released worker the scheduler had not yet run.

**Old release loop, measured** (`scripts/evidence/fb/io_start_diag.py release-loop` on `2dd280cee`; it records when the main thread signalled each worker; three runs per row):

| condition | release loop | latest start | verdict |
| --- | --- | --- | --- |
| idle host (load 1 to 4), `taskset -c 0` | 1,003 ms | 1,003 to 1,010 ms | FAIL 3/3, 75 to 117 idle workers |
| idle host, `taskset -c 0-3` | 1,003 ms | 1,003 to 1,013 ms | FAIL 3/3, 145 to 147 idle |
| idle host, unpinned (96 CPUs) | 148 to 202 ms | 370 to 410 ms | OK 3/3 |
| beside this task's gate (load 163 to 172), `0-3` | 80 to 219 ms | 80 to 220 ms | OK 3/3 |
| same, unpinned | 12 to 57 ms | 57 to 84 ms | OK 3/3 |

Even unpinned on an idle 96-CPU host, the serial loop takes up to 0.2 s of the 1 s window, and the latest start reaches 0.4 s. On 1 to 4 CPUs the loop alone uses the whole window. F1's `max_start_us=1000757` comes from one sample with no instrumentation, so this page does not say which of the two ways caused it; the fix removes both.

**Fix** (`src/bench/io_bench.c`):

1. **One release.** The main thread write-locks one `pthread_rwlock_t` before creating the workers. Each worker marks itself ready and blocks in `rdlock`. When all are ready, one `unlock` releases them: glibc wakes every waiting reader with a single futex wake, and the readers acquire without the shared-mutex convoy that `802a615fe` removed (`ab88399e9`).
2. **Window behind the last start.** The last worker to start publishes the deadline, its own start plus `--time`, with one atomic store. The others keep working and read the deadline with one atomic load per operation: no spin, no second wake (`24b6807e7`). Every worker therefore runs for at least `--time` after all are running.

`wall_s` spans release to the last exit, so `GiB_s` counts every op inside its own wall time. `max_start_us` still reports the start ramp. `idle_workers` and the error checks are unchanged and still fail closed on an I/O or verification error. The abort path sets `gate.abort` before the same unlock. `docs/how-it-works/performance.md` describes the new start gate.

**Old and new interleaved** (`scripts/evidence/fb/io_start_compare.sh OLD NEW 10 CPUS`, beside this task's 8-job gate, two sets about four minutes apart, load 6 to 200):

| CPUs | old: FAIL / runs, worst start | new: FAIL / runs, worst start |
| --- | --- | --- |
| `0-1` | 4/20, 1,001 ms | 0/20, 2,001 ms |
| `0-3` | 1/20, 1,001 ms | 0/20, 1,549 ms |
| all 96 | 0/20, 212 ms | 0/20, 219 ms |

**New build start spread** (`io_start_diag.py spread`, 9 runs labelled idle, all BENCH_OK). The load was 24 to 104 when these ran, so they are not idle-host figures. Something besides this task drove the host from load 1 to about 100 within the first minutes; it may have been another session, and it was not identified.

| CPUs | p50 | max |
| --- | --- | --- |
| `0` | 512 ms | 3.9 s |
| `0-3` | 125 to 136 ms | 2.1 s |
| unpinned | 29 to 108 ms | 0.45 s |

Beside the gate (6 runs, all BENCH_OK), `0-3` gave a max of 45 to 210 ms and unpinned 13 to 102 ms. On one CPU, 256 runnable threads take seconds to all reach a CPU. The old build failed those runs; the new build waits for them.

## Runs and environment

fstor007 (96 CPUs, RHEL 9.7, kernel 5.14.0-687.36.1, gcc 11.5.0, Python 3.9.25), `EFS_FUSE_PREFIX` shim to the private libfuse 3.16.2, TCP loopback plus RDMA on mlx5_0 for the RDMA tests. Each round started on a quiet host: `ps -u erbmi1` showed no other gate, and the load was 1.1 to 2.

- **Round 1**, Nov 9, `ab88399e9`, `git archive` tar.gz sha256 `ad5f6be40e263026a615350999e50f3935b5ae58866db7d28a20a78595541cf1`. `make test` rc 0; 8-job gate `TOTAL 595 FAIL 0 NOTRUN 3 STATUS COMPLETE (664s)`. That build still had the deadline fixed at release. Artifacts: fstor007 `~/efs-artifacts/taskFB-20261109/`, `SHA256SUMS` sha256 `b07f8f961c99796048acd70fc7ca22cfff9c47e6705e43cc6b7a84d0c5c02cd2`.
- **Round 2** was stopped at the merge coordinator's request because a merge gate ran at the same time. Its partial output is a note in the shared session notes, not evidence.
- **Round 3**, Nov 10 05:17 to 05:31 UTC. New tree `28bbc0c52` (`git archive` tar sha256 `088380c03a5e7195dcb52cfd9e159d3b3fd50f9f4f385c7777c48b884fa9726c`) and old tree `2dd280cee` (tar sha256 `c918d8cf1fce6751483e2fd94a83ed15d192a1e6e59bd0d44ce5d1da2c09c429`). `efs-bench` sha256: old `34a29489…`, new `3ba683d2…`. Steps in order:
  1. The diag runs.
  2. The race reproduction.
  3. `make -j32 test`: rc 0.
  4. `python3 tests/test_gc_files_vanish.py`: PASS.
  5. `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, with the comparison sets alongside it, starting 60 s in: `TOTAL 595 FAIL 1 NOTRUN 3 STATUS FAIL (662s) HOSTLIMIT 3`. The FAIL was `test_k41_packer_placement.py` line 255, `assert refused >= 1`, during part 1b of that test. `test_bench_cli.py` took 3.1 s and `test_gc_files_vanish.py` 0.1 s. The 3 NOTRUN are the usual host limits: cluster scripts absent, `valgrind/memcheck.h`, RDMA live.

  Artifacts: fstor007 `~/efs-artifacts/taskFB-20261109-r3/` (gate log, summary, timings, `fail-506.log`, diag and comparison outputs, race counts), `SHA256SUMS` sha256 `9e8dc3005a6b19ea02065ef289a04b105eb6c7df232807abfdbaf198b2580d67`.

Scratch trees `/tmp/efs-taskFB*` were removed after each round.

## Reproduction

```bash
git archive --prefix=new/ 28bbc0c52 > new.tar; git archive --prefix=old/ 2dd280cee > old.tar   # ship, check sha256
tar xf new.tar; tar xf old.tar; export EFS_FUSE_PREFIX=<shim: lib64 and include -> private libfuse>
(cd old && make efs-bench); cd new && make efs-bench
cp -a ../old ../old-diag && python3 scripts/evidence/fb/io_start_diag.py release-loop ../old-diag && (cd ../old-diag && make efs-bench)
taskset -c 0-3 ../old-diag/efs-bench --bench io --storage /tmp/s --rw write --io-size 4K --qd 256 --window 1 --time 1
python3 scripts/evidence/fb/gc_files_race.py ../old/tests/live/gc_reclamation.py tests/live/gc_reclamation.py
make -j32 test && python3 tests/test_gc_files_vanish.py
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh &
scripts/evidence/fb/io_start_compare.sh ../old/efs-bench ./efs-bench 10 0-1 /tmp; wait
```

The `--storage` root must be absent or empty, and the bench removes its files. Delete the trees when done. Check that the host is quiet first, because these runs load it.

## Remaining

- `test_k41_packer_placement.py` failed once in round 3 under the extra load. It passed in round 1's gate and in Task EX's gates, and K41 load failures are known on loaded hosts. A solo rerun on fstor007 is owed once the merge coordinator frees the host. Until then the round-3 gate is not a clean pass.
- `src/bench/bench_local.c` (start gate near line 545) and `src/bench/storage_principles.c` (shared `go_cv`) keep the shape this task removed from `io_bench.c`: the deadline is fixed first and the workers released after. They are not changed here, and neither failed in batch F.
- F1 still needs a clean gate on the batch F candidate. This task's gates ran on its own branch.
