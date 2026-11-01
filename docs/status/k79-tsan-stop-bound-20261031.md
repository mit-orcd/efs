# K79 follow-up: TSan `stop_blocked_stderr` stop is stuck, task CH — November 1, 2026

**K79 was incomplete under ThreadSanitizer: the stop was genuinely stuck (not slow). Fixed: K79 OPEN → FIXED, blocks alpha: no. TSan `stop_blocked_stderr` 5 of 5 PASS (was 6 of 6 FAIL), ASan PASS, plain 3 of 3 PASS; gate result below.**
Linked items: K79 ([BP page](k79-stop-blocked-stderr-20261031.md)), Task CA finding (`batch-f-devel-8e53122cd-20261031.md` on branch `claude/ca-batch-f-0f33e2`).
Tested source: devel `8e53122cd` plus the one-file change in `src/server/efsd.c` (build id `unknown`, tree shipped with `git archive`).

## What n2 (any node) was doing

Task CA saw one daemon miss the 55 s bound after SIGTERM, in the gate run and 3 reruns. Two reruns here
reproduced it (a different node each time: the one whose report line was still unwritten). The exit
record (`log/efsd.exit`) of every node reads `outcome=orderly rc=0 elapsed_ms=356..441`: the teardown
finished in under half a second. The process nevertheless stayed alive; 12 s after the record appeared,
`gdb -p` on the straggler showed:

- main thread: `shutdown_bounded` (`efsd.c`, orderly-report path) -> `_exit` -> libtsan `__interceptor__exit` -> `fflush` -> `__lll_lock_wait_private` (waiting for a stdio lock);
- the detached report helper `shutdown_line_thread`: `fputs(stderr)` -> `write_all` -> `writev(2)` blocked, `wchan=pipe_write` (the pipe is full by design of the test).

So the stop was stuck, not slow: no bound could have been met. K79's exit path avoids stdio on purpose
(a blocked report is abandoned after `EFS_SHUTDOWN_REPORT_MS` and the process leaves with `_exit`), but
libtsan's `_exit` interceptor calls `fflush(NULL)` first, which needs the lock the blocked helper holds.
The `_exit` in the forced path (`shutdown_forced`, rc 3) has the same hazard. ASan's interceptor does not
flush, hence the ASan and plain PASS; a TSan build is the only one affected, so K79 stays `blocks alpha: no`.

## Fix

`shutdown_leave(rc)` in `src/server/efsd.c` calls `syscall(SYS_exit_group, rc)` (falling back to `_exit`
where the syscall number is absent) and replaces both `_exit` calls on the bounded stop paths. No
timeout was scaled or raised: the bound (`STOP_BOUND_S` 55, `EFS_SHUTDOWN_*`) is unchanged, and the
test did not need a sanitizer multiplier because the daemons now exit in the same 0.5-5.5 s as without TSan.

## Before / after (host xorinox, 16 CPU, XFS, gcc 14.3, `-march=x86-64-v3` sanitizer lanes, TCP)

| Run | Before (devel `8e53122cd`) | After (this change) |
| --- | --- | --- |
| TSan `EFS_SAN_ONLY=stop_blocked_stderr bash tests/sanitizer_check.sh tsan` | FAIL: 4 of 4 (Task CA) + 2 of 2 here, `TimeoutExpired` at 55.0 s, no TSan report | PASS 5 of 5, every daemon exit 5.5 s after SIGTERM |
| ASan | PASS (Task CA) | PASS 1 of 1 |
| plain `python3 tests/test_stop_blocked_stderr.py` | PASS | PASS 3 of 3 (exit 0.5-5.5 s) |
| `make test-all` budgeted gate (`EFS_TEST_JOBS=8`) | | FAIL 1 of 583 (NOTRUN 11, OVERBUDGET 5, 1095 s): the one FAIL was `tests/test_shutdown_bounded.py`, a source scan that pinned the literal `_exit(EFS_SHUTDOWN_FORCED_RC);`; the test now pins `shutdown_leave(...)` and passes alone (not rerun inside a second full gate) |

## Reproduce

```
git archive HEAD | ssh HOST "mkdir -p ~/efs-taskCH/src && tar -x -C ~/efs-taskCH/src"
cd ~/efs-taskCH/src && EFS_SAN_ONLY=stop_blocked_stderr TMPDIR=$HOME/efs-taskCH/tmp bash tests/sanitizer_check.sh tsan
```

Before the change, while the entry runs, poll for an `efsd` that has written `log/efsd.exit` and still lives,
then `gdb -p PID -batch -ex "thread apply all bt 12"`. Cleanup: remove `~/efs-taskCH`. Needs gcc with libtsan, gdb, no other use of ports 19000-19099.

## Limits

One host. The OVERBUDGET rows are `k38_protection_now`, `b_geometry grow-b`, `pump_logging`, `pack_dup_candidate`, `ao_readdir_live` (load at 8 jobs; budgets untouched). The exit-record timestamps and gdb stacks are from two stuck daemons; the raw captures
(watch logs, per-run logs, gate log) are on xorinox in `~/efs-artifacts/taskCH-20261031/` (`SHA256SUMS`
sha256 `a95e2bebae88758ce1c3104fb2e3f2a86862bf37b7e84522b6163073d15ab7ff`), not in Git.
