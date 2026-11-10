# Batch F harness fixes: GC fixture scan race and io bench start (Task FB)

Task FB, Nov 9 2026, fstor007, branch `claude/blissful-chaum-051bdb` from devel `2dd280cee`, code commit `ab88399e9`. Purpose: the two harness defects that Task EX's batch F run found on devel `9bb50fe79` (page `docs/status/2026-11/batch-f-candidate-20261109.md` on branch `claude/hungry-liskov-47e4f2`, commit `9dbec51fd`, not merged at writing): F4 run 1 (the `gc-reclamation` gate crashed in its own helper) and F1 run 2 (`test_bench_cli.py`, 4 of 256 `efs-bench` workers did no I/O). Release gate: alpha 1.0 Stage 1 batch F ([acceptance matrix](../stage1-acceptance-matrix-20261030.md)).

**Verdict: PASS, harness and bench-tool change only, no K row, blocks alpha: no.** Both defects are in test-side code, not in the product. The GC helper race is reproduced on the old code and gone on the new. The io bench's serial worker release is measured and removed. fstor007 `make test` passes, and one 8-job `make test-all` gate gives `TOTAL 595 FAIL 0 NOTRUN 3 STATUS COMPLETE (664s)`. No timeout, budget, window or sleep was changed. Limitation: on 96 unpinned CPUs the old and new bench start cannot be told apart (below), so a single clean gate does not prove that the F1 intermittent is gone.

## 1. `tests/live/gc_reclamation.py` `files()`

**Defect:** `files(ino)` listed `folder.glob('*/*')` under the inode's export directory. Physical reclaim removes those directories, and in the F4 run one vanished between pathlib's listing and its `scandir`, which raised `FileNotFoundError` inside `wait(lambda: not files(tail_ino), 'span-only truncate then physical reclaim')`. The helper raced the very deletion it waits for. On Python 3.9 (fstor007), pathlib's glob catches only `PermissionError` during a scan.

**Fix:** a new `export_entries(folder)` scans one directory with `os.scandir`, skips dot names as glob did, and returns `[]` only on `FileNotFoundError`. `files()` calls it for the inode directory and again for each subdirectory, so a vanished subdirectory drops only itself and its siblings' fragments still count. Any other `OSError` still raises. The 45 `files()` callers are unchanged; they get the same list of fragment paths.

**Regression check:** `tests/test_gc_files_vanish.py` (in `make test`, budget row 30 s) compiles the real `export_entries`/`files` from the fixture source and injects `os.scandir`. It checks four cases: the resident fragments with `.sum` excluded; a subdirectory removed at its scan, where the sibling is kept; the inode directory removed, or absent, which reads as empty; and a `PermissionError`, which still raises. It fails on the old source, which has no `export_entries`.

**Race reproduction** (`scripts/evidence/fb/gc_files_race.py OLD NEW`): each version's `files()` scans for 5 s while a thread keeps creating eight subdirectories and removing the tree. fstor007, Python 3.9.25:

| source | scans | `FileNotFoundError` |
| --- | --- | --- |
| devel `2dd280cee` (glob) | 278,071 | 82 |
| `ab88399e9` | 125,894 | 0 |

## 2. `efs-bench --bench io` worker start

**Which side was at fault:** the bench tool. The test's many-workers case (`--qd 256 --window 1 --time 1`, `idle_workers == '0'`) asks for exactly what the tool promises: every worker takes part in the window. Since `802a615fe` (Oct 6), the main thread released the workers one by one, each through its own mutex and condition variable. The deadline was fixed before that loop began. I instrumented a copy that recorded when each worker was signalled. Each worker started within about 10 µs of its signal, but the loop itself took 990 ms on 1 CPU and 804 ms on 4 CPUs. Worker `i` was signalled about `i` CPU slices (4 to 9 ms) after release. Every wakeup let a running worker take the CPU from the main thread, so the last workers came out of the loop right at the 1 s deadline. The F1 failure's `max_start_us=1000757` is that tail.

**Fix** (`src/bench/io_bench.c`): the main thread write-locks one `pthread_rwlock_t` before it creates the workers. Each worker marks itself ready and then blocks in `rdlock`. After the ready count is full, the main thread sets the deadline and calls one `unlock`, and glibc wakes every waiting reader with a single futex wake. Readers then acquire it without the shared-mutex convoy that `802a615fe` removed. The abort path sets `gate.abort` before the same unlock. The per-worker mutexes and conditions are gone. The deadline still counts each worker's scheduling delay after release, as before, so a worker the host truly starves for 1 s is still reported by the fail-closed `idle_workers` check.

**Measurements**, fstor007, interleaved old (`efs-bench` built from `9bb50fe79`, identical `io_bench.c` to `2dd280cee`) and new (`ab88399e9`), same case, made with `scripts/evidence/fb/io_start_compare.sh`:

| condition | old: FAIL / runs, worst max start | new: FAIL / runs, worst max start |
| --- | --- | --- |
| `taskset -c 0-1`, beside Task EX's gate (load 138 to 174) | 6/10, 1,002 ms | 5/10, 1,002 ms |
| `taskset -c 0-3`, same | 5/10, 1,006 ms | 1/10, 1,001 ms |
| `taskset -c 0`, earlier set beside the same gate | 5/5, 1,011 ms | 2/5, 1,011 ms |
| `taskset -c 0-3`, earlier set | 1/5, 1,002 ms | 0/5, 765 ms |
| unpinned, beside Task EX's gate | 0/10, 399 ms | 0/10, 397 ms |
| unpinned, during this task's 8-job gate (load 2 to 99) | 0/30, 456 ms | 0/30, 457 ms |
| unpinned, between other sessions' loads (load 24 to 59) | 0/8, 122 ms | 0/8, 148 ms |

On an idle host the new build starts all 256 workers within 14 to 57 ms (instrumented copy, four runs). On 1 or 2 CPUs the new build still fails some runs. That is plain CPU capacity: 256 runnable threads at the scheduler's 3 ms minimum slice need about 0.77 s on one CPU before the last one runs, before any foreign load. The check is right to report this.

## Gate run

fstor007 (96 CPUs, RHEL 9.7, gcc 11.5.0, Python 3.9.25), `git archive` of `ab88399e9` (tar.gz sha256 `ad5f6be40e263026a615350999e50f3935b5ae58866db7d28a20a78595541cf1`), `EFS_FUSE_PREFIX` shim to the private libfuse 3.16.2, started once Task EX's third gate had ended (host idle, 0 `efsd`). In the same screen: `make -j32 test` rc 0, `python3 tests/test_gc_files_vanish.py` PASS, then `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, which gave `TOTAL 595 FAIL 0 NOTRUN 3 STATUS COMPLETE (664s) HOSTLIMIT 3`. The 3 NOTRUN are the usual host limits: cluster scripts absent, `valgrind/memcheck.h`, RDMA live. `test_bench_cli.py` took 2.9 s and `test_gc_files_vanish.py` 0.1 s. The 30 old/new bench pairs above ran alongside the gate. The live `gc-reclamation --put-barrier-fault` gate was not re-run here; the regression test and the race reproduction cover the helper.

Artifacts (text only): fstor007 `~/efs-artifacts/taskFB-20261109/` (gate log, summary, timings, `bench-pinned.txt`, `bench-under-gate.txt`), `SHA256SUMS` sha256 `b07f8f961c99796048acd70fc7ca22cfff9c47e6705e43cc6b7a84d0c5c02cd2`. Scratch trees `/tmp/efs-taskFB*` were removed.

## Reproduction

```bash
git archive --prefix=efs-taskFB-gate/ ab88399e9 | gzip > gate.tgz    # ship, check sha256
tar xzf gate.tgz -C /tmp && cd /tmp/efs-taskFB-gate
export EFS_FUSE_PREFIX=<shim with lib64 and include -> private libfuse>
make -j32 test && python3 tests/test_gc_files_vanish.py
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh                     # expect FAIL 0
git show 2dd280cee:tests/live/gc_reclamation.py > old.py
python3 scripts/evidence/fb/gc_files_race.py old.py tests/live/gc_reclamation.py
scripts/evidence/fb/io_start_compare.sh <old efs-bench> ./efs-bench 10 0-3 /tmp
```

Clean up with `rm -rf /tmp/efs-taskFB-gate` once done. The old `efs-bench` is a `make efs-bench` of `2dd280cee`.

## Remaining

- `src/bench/bench_local.c` (start gate near line 545) and `src/bench/storage_principles.c` (shared `go_cv`) use the same shape, deadline first and release after. Not changed here; neither failed in batch F.
- F1 still needs a clean gate on the batch F candidate; this task's gate ran on `ab88399e9`, not on `9bb50fe79`.
