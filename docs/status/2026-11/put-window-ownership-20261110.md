# PUT window ownership (Task U1)

**Status block.** Source commit: devel `67fc3b4a1`. Verdict: NOTRUN (before-half only; the extraction is not yet made). Remaining failures: none known. Active run: none. Next action: move the PUT window into `put_window.c` behind `put_window.h`, then add the After column. Merge state: branch `claude/task-u1-put-window-72984d`, unmerged.

## Purpose

Next ownership extraction asked for by the [structure review](structure-review-20261110.md): the PUT window (`put_win[]`, `put_win_overflow`, `put_win_total`, `put_win_mu`, `put_win_cv` and five functions) sits in the middle of `src/client/write.c` and is only called through `put_win_open`, `put_win_close` and `put_win_wait`. The recipe is that of the [`.find` pilot](find-res-ownership-20261108.md): private state behind a small contract header, an interface test per operation, and a reading comparison measured by the [reading-set method](reading-set-baseline-20261104.md#measures-task-ev-nov-9).

## Before (devel `67fc3b4a1`, fixed before any source change)

Measured by the implementing agent, not a blind reader. A line is "needed" when the answer depends on it, "opened" when it was printed into the agent's context. Counts are approximate to a few lines.

**Question.** Can a PUT wait in the window past the flush bound when one member has stopped, and what wakes it?

**Answer key.**

1. The window opens inside the PUT's own flusher, after the slot is marked clean and still under the slot and I/O locks: `dcache_steal_dirty` and `dcache_flush_slot_inner_budgeted` call `put_win_open(ino)` after `dcache_dirty_unlink`.
2. The window closes on every exit of that flusher and of the batch: `flush_pipe_drain` (each job, success, failure or retired inode), `flush_pipe_add` (drain failure) and `dcache_flush_slot_inner_budgeted` (after the put-record, with the locks held again, or after a retired inode). A PUT that fails to a stopped member closes its window when the PUT returns its error; the window does not outlive the PUT call.
3. What wakes a waiter is `put_win_close`: it decrements the inode's slot (or the overflow count), the all-inode total, and broadcasts `put_win_cv` under `put_win_mu`.
4. A waiter is bounded: `put_win_wait` ends at the earlier of now plus `max_ms` and the caller's RPC deadline (`efs_client_rpc_deadline_ms`), re-checks at least every 50 ms by a timed wait, and returns 0 (nothing open), 1 (waited, windows closed), `EFS_ERR_BUSY` (still open at the bound) or `EFS_ERR_IO` (wait error); it releases `put_win_mu` on every return.
5. Both waiters use 8000 ms: `efs_dcache_flush_ino` logs `stage=put-window` and returns `EFS_ERR_BUSY` on timeout, and runs one more pass when it waited (`w == 1`); `efs_dcache_put_win_wait` (readers) is lock-free when `put_win_total` is 0.
6. So the answer: the waiter never waits past 8 s or the RPC deadline; the PUT itself, and so the window, can outlast that bound while it talks to a stopped member (the waiter then gets BUSY), and what ends the window is the PUT returning, not the waiter timing out. With more than 512 inodes in a window at once `put_win_overflow` is non-zero and `put_win_busy_locked` reports busy for every inode, so an unrelated inode's waiter also waits until the overflow clears.

**Reading log (before).**

| Range | Lines opened | Needed | Category |
|---|---|---|---|
| `graphify explain client/write` | 35 | 0 | navigation (dead end: lists budget rows) |
| `grep -n put_win` write.c, client_internal.h, tests | 50 | 46 | navigation |
| `write.c` 5195–5335 (comment, state, five functions) | 141 | 122 | implementation |
| `write.c` 7118–7170, 7295–7340, 7455–7540, 7760–7800 (call sites) | 226 | 66 | callers |
| `write-path-map.md` 63–76, 287–300 | 28 | 10 | guide |
| `tests/test_put_window_deadline.py` | 30 | 25 | validation |

Understanding lines: opened 479, needed 244. Validation lines: opened 30, needed 25 (`test_put_window_deadline.py` proves fact 4: expired, inherited and local bounds, wake on close, mutex released; no test proves facts 1, 2, 3 or the overflow in 6 by calling the real code, the other four tests stub `put_win_open`/`put_win_close`/`efs_dcache_put_win_wait`). Total: opened 509, needed 269, irrelevant 240. Source files opened: 1 (`write.c`, 9,818 lines).
