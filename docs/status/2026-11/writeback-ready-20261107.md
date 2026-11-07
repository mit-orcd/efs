# Write-back pool `ready` flag and start-failure join (Task DT)

**Verdict: PASS (Nov 7). Task DM's open finding in
`src/client/efs_fuse/writeback.c` is fixed: every read and write of `ready`
and `shutdown` is under the pool mutex, and a start whose worker fails to
spawn is undone by the same join helper as `efs_wb_stop`, with the pool kept
marked stopping until the join is over.** Two new cases in
`tests/test_wb_interface.c`: TSan on node9900 FAIL 3 of 3 before (2, 2 and 3
reports, one run also a SEGV in `pthread_join`), PASS 10 of 10 after; the
plain build before hung once (60 s timeout) and segfaulted once. K90 drain
unchanged and PASS. No K row, blocks alpha: no. Not merged, not pushed.

- Purpose: close the open finding of
  [write-back pool ownership](writeback-ownership-20261105.md) (Task DM,
  W171 phase 3). DM recorded it without a K row; this page keeps it so (see
  "K row" below).
- Source: base devel `7d5e56026` (wave 60). Branch
  `claude/vigorous-benz-5972ed`: code and tests `6266aea9b` (tested), this
  page, the ledger entry and the budget note in the next commit, then the
  graph rebuild. The host trees were copied from the uncommitted working
  tree with `git archive`: before = devel plus the new tests and `Makefile`
  rule (no `writeback.[ch]` change); after = byte-identical to `6266aea9b`
  except `tests/test_write_queue.py`, whose stub fix (the one other file in
  `6266aea9b`) was copied onto the host tree before the second `tests_for`
  and `make test` runs. The 10 TSan runs and the first verify ran before
  that copy; neither builds `test_write_queue.py`.
- Environment: node9900 (192 cpus, `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`),
  gcc 11.5.0, trees in `/tmp/efs-taskDT/{before,after}`,
  `TMPDIR=/tmp/efs-taskDT-tmp`, TCP loopback, buffered I/O; load average
  0.6 at the start, about 10 during the after-fix `make test`.

## The finding (devel `7d5e56026`)

- `g_wb.ready` read without `g_wb.mu` in `efs_wb_ensure` (fast path),
  `efs_wb_sync`, `efs_wb_sync_ino`, `efs_wb_pending`, `efs_wb_stop`;
  written under it by `efs_wb_ensure` and without it by `efs_wb_stop`
  after the join (`ready = 0; shutdown = 0;`).
- `efs_wb_ensure`'s start-failure path set `shutdown`, dropped the mutex,
  joined `workers[0..i)` and cleared `shutdown` without the mutex, all
  with `ready == 0`. A second writer took the mutex during that join, saw
  the pool unstarted, rewrote `nworkers` and `workers[]` and started
  workers that might see `shutdown` set (exit at once, its job never run)
  or have it cleared under them.
- Two stops could both see `ready` and both join the same threads.

## Fix (`writeback.c` +53 −39, `writeback.h` +10 −3)

Which rule: the mutex, not an atomic. Every reader of `ready` takes the pool
mutex on its next line anyway (the enqueue to queue its job, the drains
through `wb_lock_budget`, `efs_wb_pending` to count), so moving the check
inside it costs nothing on the hot path; an acquire/release atomic would add
a second rule for one flag. `tests/test_wb_drain_budget.py` splices
`efs_wb_sync` and `efs_wb_sync_ino` by text against a stub `g_wb`; the
changed drains use only fields the stub has, and it passes.

- `wb_join_locked` (L382): entered with the mutex; sets `shutdown`,
  broadcasts `not_empty`, drops the mutex (the workers need it to finish
  their jobs and leave), joins `workers[0..nworkers)`, retakes it, clears
  `ready` and `shutdown`, broadcasts `idle`, returns unlocked. `workers[]`
  and `nworkers` are written only by `wb_start_locked` with `shutdown`
  clear, so reading them unlocked during the join is safe.
- `wb_start_locked` (L399) replaces `efs_wb_ensure`: called under the
  mutex by `efs_wb_enqueue_owned` (L424) when the pool is neither started
  nor stopping; on a failed `pthread_create` it sets `nworkers = i` and
  calls `wb_join_locked`, and the enqueue returns `EFS_ERR_NOMEM` with the
  buffer released, as before.
- `efs_wb_enqueue_owned`: an enqueue that arrives while a stop or a failed
  start is joining finds `shutdown` set and returns `EFS_ERR_IO` (the
  existing "pool is stopping" answer), never starting into `workers[]`
  mid-join.
- `efs_wb_stop` (L553): takes the mutex, waits on `idle` while another
  stop or a failed start is joining, then `wb_join_locked` if started.
- `efs_wb_sync` (L504), `efs_wb_sync_ino` (L566), `efs_wb_pending`
  (L541): check `ready` after taking the mutex.
- `writeback.h`: the Lock paragraph says the started and stopping flags are
  under the pool mutex and when it is dropped (only to join); the
  enqueue's `EFS_ERR_IO` covers a failed start being undone; `efs_wb_stop`
  says a racing stop waits for the first join. The rest of the contract is
  unchanged and still true.

Behaviour notes: a drain whose deadline has already passed on an unstarted
pool now returns `EFS_ERR_BUSY` from `wb_lock_budget` where it returned
`EFS_OK` without locking (covered by the contract's "BUSY when the lock ...
runs past the caller's deadline"). Callers: `main.c:34` drains without a
deadline; `append.c:77` returns `EFS_ERR_BUSY` itself on a past deadline
just before its drain; `write.c:236` drains after its own enqueue, so the
pool is started; `write.c:149` (unit-begin retry with the append lock held)
is the one new window, on a mount whose pool never started and whose
deadline is already spent, where it now gets `EFS_ERR_BUSY`, the same
answer a started pool gives there. Not separately tested. A writer racing a failed start gets
`EFS_ERR_IO`, or starts a fresh pool once the undo is over.

## Tests (`tests/test_wb_interface.c` +83 −2, `Makefile` +2 −1, `tests/test_write_queue.py` +2 −2)

The binary links with `-Wl,--wrap=pthread_create`; `__wrap_pthread_create`
calls `__real_pthread_create` (so TSan's interceptor still sees every
thread) unless armed to fail one numbered call with `EAGAIN`.

1. Failed start: the second worker start fails; from inside that call a
   racer thread starts and enqueues while the first start is being undone.
   Checks: the writer gets `EFS_ERR_NOMEM`, the racer `EFS_OK` or
   `EFS_ERR_IO`, `put_calls` grows by the racer's PUT only, stop joins,
   `efs_wb_pending(0) == 0`, the drain `EFS_OK`, a later write `EFS_OK`.
2. Readers vs stop: four threads loop `efs_wb_pending(0)`,
   `efs_wb_pending(ino)`, `efs_wb_sync_ino(ino)` and `efs_wb_sync()`, one
   loops `efs_wb_stop()`, while the main thread runs 50 rounds of write
   (`EFS_OK` or `EFS_ERR_IO`) and stop. No sleeps; the loops end on a flag.

`tests/test_write_queue.py` splices `efs_wb_enqueue_owned` by text against a
stub pool and stubbed `efs_wb_ensure`; it failed to compile after the fix
(`-Werror`: no `ready` field, `wb_start_locked` undeclared). Its stub pool
gained `ready` and the stub start is now `wb_start_locked` (sets `ready`,
returns 0); its four cases are unchanged and PASS.

## Results (node9900)

Before the fix (tests only), `EFS_SAN_ONLY=wb_interface bash
tests/sanitizer_check.sh tsan`, three runs: FAIL 3 of 3.

| Run | Reports | Races (write vs read, devel line numbers) |
|---|---|---|
| r1 | 2 + SEGV | `shutdown`: start-failure undo L390 unlocked vs enqueue L455; `ready`: start L394 vs unlocked read; SEGV in `__pthread_clockjoin_ex` (two stops joining one thread) |
| r2 | 2 | the same two |
| r3 | 3 | the same two, and `nworkers`: start L381 vs a racing stop's join loop L545 |

Plain (non-TSan) build before: run 1 timed out at 60 s (rc 124), run 2
SIGSEGV (rc 139).

After the fix (`dfd8bd684`):

| Check | Result |
|---|---|
| TSan `wb_interface`, 10 runs | 10 of 10 PASS, 0 reports, 0 to 1 s each |
| `./tests/test_wb_interface`, three solo runs | rc 0, 0.091 / 0.094 / 0.088 s |
| `tests/test_wb_drain_budget.py` (splices the drains) | PASS |
| `tests/test_stop_dirty_drain.py` | PASS |
| `tests/test_k90_unmount_enospc.py` | PASS, "refusal at close 6.5 s, full member exit 1 in 6.6 s, room back exit 0 in 0.7 s, exact" (18 s) |
| `python3 scripts/tests_for.py client/efs_fuse --run` | rc 0, 53 commands (81 s); the first run stopped at `tests/test_write_queue.py`, see below |
| `make test` | rc 0 (113 s; the first run failed at the same splice) |

Budget: `tests/budgets.tsv` row `./tests/test_wb_interface` stays 5 s. The
solo runs (about 0.09 s, up from DM's 0.04 s with two more cases) are far
below it, and 5 s is the row granularity (1.25x rounded up to 5 s); nothing
to lower. The row's note and description now record this measurement and
the two cases.

## K row

None. The `ready` race is reachable in production (the first write starts
the pool while another FUSE thread reads `ready` through `efs_wb_pending` in
`append.c` or the control drain in `release.c`), but its outcome there is
benign: a reader that sees the stale 0 answers "nothing pending", which is
true because the starting writer has not queued its job yet. The
start-failure and double-stop races need `pthread_create` to fail or two
stops; `efs_fuse_destroy` is the only stop and runs after the FUSE loop.
TSan reported these only under the test's synthetic concurrency. blocks
alpha: no.

## Reproduce

```
make tests/test_wb_interface && ./tests/test_wb_interface
EFS_SAN_ONLY=wb_interface bash tests/sanitizer_check.sh tsan
python3 tests/test_wb_drain_budget.py && python3 tests/test_stop_dirty_drain.py
python3 tests/test_k90_unmount_enospc.py
python3 scripts/tests_for.py client/efs_fuse --run && make test
```

For the before result, revert `src/client/efs_fuse/writeback.[ch]` to devel
`7d5e56026` and keep the new test and `Makefile` rule. Cleanup: the
sanitizer gate deletes its log dir on PASS; `make clean`.

Raw captures: `~erbmi1/efs-artifacts/taskDT-20261107/` on node9900 (TSan
reports before and after, run logs), `SHA256SUMS` sha256
`b92b6c61c7d4d1e4adfd4f7068d16a350a2963499a6b4c1ea5905f4800496892` (95 files, 1.5 MB).
The scripts used are `run.sh` and `verify.sh` there.
`/tmp/efs-taskDT*` removed.

## Not checked

- ASan list entry `wb_interface` not run (TSan only, as the task asks).
- No full `make test-all` gate and no second host; fstor007 not used.
- No live mount with a failing `pthread_create`; the failure is injected
  at link time in the unit test only.
- TSan cannot prove absence; 10 clean runs of the two cases and the
  existing ones.
