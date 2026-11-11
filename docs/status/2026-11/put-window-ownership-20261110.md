# PUT window ownership (Task U1)

**Status block.** Source commit: devel `67fc3b4a1`; before-half `7a0b1bf19`; ownership `696378a6e`. Verdict: PASS. Remaining failures: none. Active run: none. Next action: coordinator review and merge; `read.c` should include `put_window.h` and drop the duplicate `efs_dcache_put_win_wait` declaration in `client_internal.h` (two files outside this task). Merge state: branch `claude/task-u1-put-window-72984d`, unmerged.

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

## What changed

- New `src/client/put_window.c` and `put_window.h`: the state (`put_win[512]`, `put_win_overflow`, `put_win_total`, `put_win_mu`, `put_win_cv`) and `put_win_open`, `put_win_close`, `put_win_busy_locked` (private), `put_win_wait`, `efs_dcache_put_win_wait` moved out of `write.c` byte for byte. The one edit inside a function: `put_win_wait` reads the clock through `put_win_clock_ms`, a private copy of `write.c`'s static `report_clock_ms` (the other 42 uses stay in `write.c`). The header carries the contract (owner, lock and its order against `dcache_io_mu`/`dcache_mu`, what each operation guarantees). Operation names unchanged, so the 12 call sites in `write.c` and the five splicing or stubbing tests are untouched.
- `tests/test_put_window_interface.c` (10 cases, case filter as `test_find_res_interface`), Makefile rule and `test` recipe line, `tests/budgets.tsv` row (5 s; solo 0.28/0.28/0.28 s on intel1), both lists of `tests/sanitizer_check.sh`, `MODULES['write']` in `tests/csrc.py`, the write-path map, the `src/client/README.md` guide.
- `tests/test_put_window_deadline.py` now splices `put_win_wait` from `put_window.c` and stubs `put_win_clock_ms`.
- `efs_dcache_put_win_wait` is declared in both `put_window.h` and `client_internal.h` (identical prototype). `read.c` calls it through `client_internal.h`, and neither file was in this task's list; the duplicate goes away when `read.c` includes `put_window.h`.

## After (ownership tree)

Same question, same key, read from the guide, header, owner and call sites. The reader located the call sites with `grep -n put_win` and read them with 2 to 3 lines of context.

| Reading log (after) | Lines opened | Needed | Category |
|---|---|---|---|
| `graphify explain client/write` | 31 | 0 | navigation (dead end again: put_window.c is not its own component, the page lists budget rows) |
| `grep -n put_win src/client/write.c` | 13 | 13 | navigation |
| `put_window.h` | 45 | 42 | interface (facts 3, 4, 5 and the lock) |
| `put_window.c` | 138 | 118 | implementation |
| write.c `grep -B3 -A2 put_win_open/close` and `-A8 put_win_wait`, plus 12 lines of `efs_dcache_flush_ino` | 98 | 72 | callers (facts 1, 2, 5) |
| README guide (13) and `--list` (10), two test cases (39) | 62 | 62 | validation |

| Measure | Before | After |
|---|---|---|
| Understanding lines needed | 244 | 245 |
| Validation lines needed | 25 | 62 |
| Total needed | 269 | 307 |
| Lines opened (understanding / validation / total) | 479 / 30 / 509 | 325 / 62 / 387 |
| Irrelevant reading (opened − needed) | 240 | 80 |
| Dead ends | 1 | 1 |
| Source files opened | 1 (`write.c`) | 3 (`write.c`, `put_window.h`, `put_window.c`) |

Reading of the table: the needed understanding is the same, because facts 1 and 2 live in `write.c`'s call sites and the extraction does not move them; what falls is the irrelevant reading (240 to 80), since the state is no longer found inside a 9,818-line file, and the contract states facts 3, 4 and 5 without reading the implementation. Validation needed rises from 25 to 62 because the facts about the wait and the overflow are now proved by named cases that run the real code (before, only the wait was proved, by a test running a spliced copy). Source files opened rise from 1 to 3. Facts 1 and 2 (every exit of a flusher closes its window) remain unproved by any interface test: they are in `write.c` and covered by the routed write tests only. Measured by the implementing agent with the answer key in hand, not a blind reader.

## Commands run and results

All on intel1 (8 cpus, gcc 14.3, load from the user's live cluster not touched), source `696378a6e`, trees under `~/efs-taskU1/` (`src` = devel `67fc3b4a1`, `new` = this branch, `git archive`, no `.git`, `EFS_GIT_ID` unknown).

| Check | Result |
|---|---|
| `nm src/client/write.o \| grep put_win` | only `U put_win_close`, `U put_win_open`, `U put_win_wait`: none defined |
| Instruction count, inlining off | identical, see below |
| `make -j8 all` | rc 0 (warnings are the existing ones in `put_upload.c`, `packer.c`, `reclaim.c`, `write.c` L2485) |
| `./tests/test_put_window_interface` | rc 0, PASS line; `--list` prints the 10 cases; `... slots reader` rc 0; `... overflow` and `... test_nope` exit 2 with "no case matches" |
| Three solo runs | 0.28 / 0.28 / 0.28 s, budget 5 s |
| `make test` | rc 0 |
| `python3 scripts/tests_for.py client/write --run` (53 commands) | rc 0 |
| `tests/sanitizer_check.sh asan`, `tsan` | asan: VERDICT PASS (63 run, 0 failed, 392 s), `PASS put_window_interface (0s)`; tsan: VERDICT PASS (33 run, 0 failed, 552 s), `PASS put_window_interface (0s)`; the known intermittents `k37_segment_rebuild` and `reclaim` did not occur |
| `python3 scripts/check_concern_map.py docs/how-it-works/write-path-map.md src/client/write.c src/client/put_window.c` | rc 0 |
| Mac: `check_doc_links.py`, `status_layout.py`, `make docs-check`, `check_k_register.py`, `check_guide_counts.py` (scans `src/client/README.md`: 13 guides), `test_make_test_recipe.py`, `test_tests_for.py`, `csrc.py` | all rc 0 |
| Mac: `sh scripts/rebuild-code-graph.sh`, `python3 tests/test_code_graph.py` | rebuild PASS (19,201 nodes, 80,989 edges, 8 known-bug navigation checks); `test_code_graph.py` 10 tests OK |

Instruction count, per function, gcc `-O3` plus the build's flags and `-fno-inline -fno-ipa-cp -fno-ipa-sra -fno-ipa-cp-clone -fno-partial-inlining`, `objdump -d --no-show-raw-insn` of `write.o` (before) and `put_window.o` (after), alignment `nop` lines excluded (the first count differed by one `nopw` padding line in `efs_dcache_put_win_wait`):

| Function | Before | After |
|---|---|---|
| `put_win_open` | 55 | 55 |
| `put_win_close` | 41 | 41 |
| `put_win_busy_locked` | 20 | 20 |
| `put_win_wait` | 83 | 83 |
| `efs_dcache_put_win_wait` | 7 | 7 |
| `report_clock_ms` / `put_win_clock_ms` | 16 | 16 |

Reproduce: `git archive <commit> | ssh intel1 'tar -x -C <dir>'`, compile `src/client/write.c` (before) or `src/client/put_window.c` (after) with the flags above and `-Isrc/client`, and count the instruction lines per function symbol.

## Limitations

- The interface test sleeps nowhere in product code; `test_close_wakes_wait` busy-waits 20 ms (yielding) so the waiter is blocked before the close, and asserts a result of 1.
- `EFS_ERR_IO` (a failed condition wait) has no test: it needs a wrapper around `pthread_cond_timedwait`.
- The window's open and close discipline at the 12 call sites (facts 1, 2) is not covered by the interface test; only the routed write tests exercise it.
- The sanitizer lanes are the whole lists; failures that are not the new entry are recorded above, not fixed.
- `screen` on intel1 needed `SCREENDIR=~/efs-taskU1/screens` (`/run/screen` has the wrong mode).
- Raw output: `~/efs-artifacts/taskU1-20261110/` on intel1 (logs, instruction-count objects' listings, `SHA256SUMS`); not committed.
