# efs_fuse per-area interfaces (Task DC, W171 phase 3 (c))

Task DC, Nov 4 2026. Purpose: [W171 phase 3 (c)](structure-plan-20261102.md): give each file of `src/client/efs_fuse/` a narrow interface with its ownership, lock, lifetime and failure contract, so an agent changing one area reads `src/client/efs_fuse/README.md`, that area's header and that file, not the whole shared `fuse_internal.h`. Behaviour-preserving by construction: declarations moved verbatim, no function body changed, and the object code of ten of the twelve files is identical to the baseline (below). Verdict: **PASS** (fstor007 gate `TOTAL 585 FAIL 0 NOTRUN 3`, the three NOTRUN rows expected on that host, one over-budget pass under load; `make tsan-check` 22 of 22 PASS; see Gate).

## Shape

- Source: devel `25e602bed` (wave 55 split, component README, per-file catalogue routes, reading-set baseline). Branch `claude/task-dc-fuse-interfaces`, twelve commits: one per header carved out, then the README, evidence and csrc order.
- `fuse_internal.h`: **465 → 80 lines**. It keeps the include block (57 lines), `struct efs_file_handle`, the per-request thread-local `t_req`, and the six helpers every handler calls (`efs_file_handle`, `efs_file_ino`, `stats_now_ms`, `virt_kind`, `fuse_stat_errno`, `efs_rc_to_errno`).
- Eleven `<area>.h` headers (722 lines in all, 802 with `fuse_internal.h`; the 465 declaration lines became 802 because each header now opens with a 10 to 40 line contract). Each includes `fuse_internal.h`, states its contract, then its declarations. `main.c` exports nothing and has no header.
- A `.c` file includes `fuse_internal.h`, its own header and the headers of the areas it calls (computed from the symbol-to-file matrix; the compiler checks every declaration against its definition). The include block at the top of a file is now its dependency list: `support.c` needs only `lookup.h`; `ll_run.c`, the dispatcher, needs nine.
- Reading set for a change in one area, lines: before = README 82 + `fuse_internal.h` 465 + the file; after = README 85 + `fuse_internal.h` 80 + the area header + the file. For `open.c` that is 1,423 → 1,133; for `release.c` 757 → 416; for `writeback.c` 1,045 → 760. Task DB's after-measurement ([baseline](reading-set-baseline-20261104.md)) is the real number.

## Per-header table

| Header | Lines | Owner | Structs and macros | Globals declared | Lock | Lifetime |
|---|---|---|---|---|---|---|
| `support.h` | 137 | `support.c` | `struct stats_ent`, `EFS_STATS_TEXT`, `struct efs_find_res`, `EFS_FIND_RES_N` | `g_fuse_se`, `g_find_res`, `g_find_res_mu` | `g_find_res_mu` over every slot field, never across an RPC; `g_fuse_se` no lock (written by `main()` outside the session loop) | find-result text belongs to its slot until evicted or freed by `efs_fuse_destroy`; file handles from OPEN to RELEASE |
| `getattr.h` | 80 | `getattr.c` | `struct readdir_ent`, `struct readdir_collect_arg`, `struct efs_dirh` | `t_lookup_memo_serial`, `t_getattr_row`, `t_getattr_ino` | memo under a private mutex; thread-locals need none | memo rows 50 ms, one shot; thread-locals valid inside the setting handler only; `efs_dirh` from OPENDIR to RELEASEDIR |
| `open.h` | 92 | `open.c` | `struct efs_open_ref`, `struct efs_pending_create` | `g_open_mu` | stripe (`open_edge_mu`) first, then `g_open_mu`; `g_open_mu` never across an RPC, the stripe may be | a row from first open until the last close is confirmed; `close_pending` rows drained by the retry thread, joined at destroy |
| `writeback.h` | 97 | `writeback.c` | `struct efs_wb_job`, `struct efs_wb_state`, `EFS_WB_DEPTH`, `EFS_WB_WORKERS_MAX`, `EFS_WB_RESERVED` | `g_wb` | `g_wb.mu` over every field, with `not_empty` / `not_full` / `idle` | workers from first enqueue to `efs_fuse_destroy`; `err` / `err_ino` retained until `efs_wb_sync` or `efs_wb_sync_ino` consumes them; the second per-inode channel is `efs_wb_inode_stalled` in `../write.c` |
| `append.h` | 41 | `append.c` | — | — | `append_mu(ino)` stripes; row append fields under `g_open_mu` | pressure worker from `fuse_pressure_kick` to `fuse_pressure_stop` |
| `write.h` | 32 | `write.c` | — | — | none of its own | none |
| `namespace.h` | 41 | `namespace.c` | — | — | none of its own | a lost CREATE reply becomes a pending create owned by `open.c` |
| `release.h` | 41 | `release.c` | `CLOSE_ADMIT_SPINS` | — | the inode stripe from `open.h` | none |
| `plock_init.h` | 40 | `plock_init.c` | `OFF_MAX` | `g_fuse_ready_wr` | lock table under a private mutex and condvar; `g_fuse_ready_wr` no lock (single-threaded phases) | a lock row pins its inode until unlocked; the readiness fd from `main()` to `FUSE_INIT` |
| `lookup.h` | 46 | `lookup.c` | `struct deep_callback_sample` | — | none | a sample lives on the handler's stack |
| `ll_run.h` | 75 | `ll_run.c` | — | — | none | sets and clears `t_req` and `t_lookup_memo_serial` around each body |

Failure behaviour is in each header's contract block (what the caller gets on BUSY, timeout and I/O failure); the README's Contract section points at the headers instead of listing the globals.

## The eleven globals

| Global | Was | Now | Why not static |
|---|---|---|---|
| `g_mountpoint` | `extern`, defined in `support.c` | **`static` in `main.c`** | only `main()` wrote and read it; `support.c` never used it |
| `t_req` | `extern` | `extern` in `fuse_internal.h` | set by every `ll_*` body (`ll_run.c`), read by `ll_ctx()` (`support.c`) |
| `g_fuse_se` | `extern` | `extern` in `support.h` | `main.c` creates, mounts, loops and destroys the session (twenty uses); `support.c` reads it in the invalidation thread |
| `g_find_res`, `g_find_res_mu` | `extern` | `extern` in `support.h` | `lookup.c` and `open.c` match a term under the mutex, `main.c` frees the slots at destroy; an accessor would wrap the lock around a field scan |
| `t_lookup_memo_serial` | `extern` | `extern` in `getattr.h` | `ll_run.c` sets it from `lookup_memo_start()`, `lookup.c` reads it in the fault barrier |
| `t_getattr_row`, `t_getattr_ino` | `extern` | `extern` in `getattr.h` | `getattr.c` sets, `lookup.c` reads both in one line |
| `g_open_mu` | `extern` | `extern` in `open.h` | `append.c` takes it directly around row updates (three critical sections) |
| `g_wb` | `extern` | `extern` in `writeback.h` | `append.c`, `release.c` test `ready` / `count` / `inflight` under `g_wb.mu`; `main.c` shuts the pool down field by field |
| `g_fuse_ready_wr` | `extern` | `extern` in `plock_init.h` | `main.c` sets it and closes it at exit, `FUSE_INIT` consumes it |

Ten stay shared. Every one is read or written by two or more files inside their own critical sections; an accessor would have to rewrite those function bodies (the task forbids changes inside functions, and the splice tests cut those bodies out by text) and would hide nothing that the header contract does not now state.

## Object code

On fstor007, `objdump -d -r --no-show-raw-insn` of each of the twelve objects, baseline (`25e602bed`) against the final tree, same compiler and flags:

- `getattr.o`, `open.o`, `writeback.o`, `append.o`, `write.o`, `namespace.o`, `release.o`, `plock_init.o`, `lookup.o`, `ll_run.o`: identical.
- `support.o`: two relocation addends into `.data` change (the `g_mountpoint` definition left this file); no instruction differs.
- `main.o`: 159 relocation addends into `.data` change (`g_mountpoint` joined this file's data section): 318 differing lines, all of them `R_X86_64_*` relocation lines, no instruction line differs.

## Source diff

The only lines added or removed inside the twelve `.c` files, apart from `#include` lines (`git diff 25e602bed HEAD -- 'src/client/efs_fuse/*.c' | grep '^[+-]' | grep -v '^[+-][+-]' | grep -v '#include'`), are the `g_mountpoint` definition and its comment leaving `support.c` and arriving in `main.c` as `static`.

## Tests and the harness

- No test assertion changed and no test search pattern changed. 55 test files read the module through `csrc.module('efs_fuse')`; the twelve `.c` files still come first in the same order, so every first-match `index` into a function body lands where it did.
- `tests/csrc.py`: `MODULES['efs_fuse']` gained the eleven headers after `fuse_internal.h`, in the order of the README's Files table; `hblock()` now searches every `.h` entry of the module in order instead of the last entry (six tests take `struct efs_open_ref`, `CLOSE_ADMIT_SPINS`, `struct efs_file_handle`, `struct efs_pending_create`, `EFS_FIND_RES_N` / `struct efs_find_res` from it, which now live in four headers). Self-check `python3 tests/csrc.py` passes.
- `Makefile`: the client objects depend on `$(wildcard src/client/efs_fuse/*.h)` instead of `fuse_internal.h` alone.
- Routed FUSE tests (`python3 scripts/tests_for.py client/efs_fuse --run`, 50 rows) and the nine other module readers (`test_client_memory`, `test_create_errors`, `test_declaration_gate_handler`, `test_faults_env_ignored`, `test_pressure_cohort`, `test_put_drain`, `test_recorder_stop`, `test_stop_dirty_drain`, `test_wb_runtime`): all 59 pass on fstor007 (`fusetests.log` and `extra-*.log` in the artifacts), run after `make all` and `make test-bins`.

## Checks

- `python3 scripts/check_include_boundary.py`: `PASS include boundary: 22 includes, 8 pairs` (the per-area headers include only `fuse_internal.h`, inside one directory).
- `python3 tests/csrc.py`: `csrc self-check ok`; `python3 tests/test_make_test_recipe.py`: 1023 commands, budgeted gate 1750 s of 1750 s, PASS (the budget file is untouched).
- `make docs-check`: all 6 architecture checks pass (the component README gate finds the five headings in order); `python3 scripts/check_doc_links.py`: 0 dangling links.
- `sh scripts/rebuild-code-graph.sh` then `python3 tests/test_code_graph.py`: PASS, 9 tests, after the docs were final.
- Every carve commit was built on fstor007 in turn (`make efs-fuse`, 0 warnings each) before the full `make all` of the final tree.

## Gate

Host fstor007 (tree copied with `git archive` to `/tmp/efs-taskDC/tree`, libfuse shim under `/tmp/efs-taskDC/fuse`, `TMPDIR=/tmp/efs-taskDC-tmp`), `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` after `make all` and `make test-bins`:

    TOTAL 585 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (930s) OVERBUDGET 1 BUILD 1s

- The three NOTRUN rows are the ones expected on this host: `test_cluster_stop.py` (no `~/git/cluster`), `test_rdma_memcheck.py` (no `valgrind/memcheck.h`), `test_qualify_rdma_live.py` (no RDMA hardware). INCOMPLETE is the gate's status for NOTRUN with 0 FAIL (exit 3).
- OVERBUDGET: `tests/test_pump_logging.py` passed in 112.9 s against its 95 s budget as command 517 of 585, under eight parallel jobs next to the two `test_b6_unavailable` modes (229 s and 202 s). Solo on the same host afterwards: 111.2 s, 114.5 s and 112.7 s, all passing (`solo-*.log`), so it is not load. It is also not this branch: the devel gates of merge waves 53 to 55 on fstor007 and node9900 (`~/efs-artifacts/merge-wave5*/timings.tsv`) show the same test at 108.4 to 115.3 s against the same 95 s budget, and Task DA's branch measures it at 85.4 to 89.6 s (`~/efs-artifacts/taskDA-20261104/gate-*/timings.tsv`). The budget was not raised (`docs/how-it-works/developing.md` section 4); the test exercises `server/raft_host` and reads no FUSE code. Task DA owns the fix.
- `make tsan-check` (default list): `VERDICT: PASS (22 run, 0 failed, 644s)`. The known intermittent `d4_progress` race in the `src/client/write.c` dcache reclaim did not reproduce in this run, so no report was written.
- Artifacts: `~/efs-artifacts/taskDC-20261104/` on fstor007 (`summary.txt`, `timings.tsv`, `build.log`, `gate.log`, `tsan.log`, `solo-*.log`, `fusetests.log`, `extra-*.log`, `steps/`, `objcmp/`) with `SHA256SUMS`; sha256 of `SHA256SUMS`: `433c1850ed1addc110bf6be3c564e79f8c71c123cbfffa46f464eecce6fc92f0`.

## Follow-ups (not done here)

- Task DB's baseline asks the after-measurement to find the retained-release drain tests under `.release`: the catalogue rows for `test_open_lease.py`, `test_open_busy.py`, `test_fuse_lease_retention.py` and the FLUSH/FSYNC surfacing tests (`.plock_init`, `client/write`) were not rerouted; `tests/budgets.tsv` is untouched.
- The splice tests still read the module as text; migrating them to compile against the area headers is the plan's open item.
