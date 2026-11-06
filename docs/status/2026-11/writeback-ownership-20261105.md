# Write-back pool ownership pilot (W171 phase 3, Task DM)

DRAFT: before-half recorded on `cb51d1cd6` before any source change.

## Scenario

"At unmount `efs_fuse_destroy` stops the write-back workers. Can it return
with a job still queued or in flight, or with the sticky PUT error never
reported, and can another reader of the pool (the explicit-stop control
drain, a close) race the stop?" Not one of the three baseline fixes.

Answer found by the before-reading: no job is abandoned (a worker leaves its
loop only at `shutdown && count == 0`, so queued jobs finish first; an
in-flight PUT finishes before `pthread_join` returns); the sticky error is
consumed by `efs_wb_sync` at `main.c` L34 and makes the stop unclean. Latent:
`g_wb.ready` is read without `g_wb.mu` by four readers (`efs_wb_ensure`,
`efs_wb_sync`, `efs_wb_sync_ino`, `release.c` control drain, `append.c`
`efs_ino_has_queued_write`) and written without it by `main.c` L48–49 after
the join; `efs_wb_ensure`'s start-failure path unlocks before joining with
`ready == 0`, so a second starter could reuse `workers[]` mid-join.

## Before (devel `cb51d1cd6`)

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–92 | guide | yes (writeback row, unmount contract) |
| `src/client/efs_fuse/writeback.h` | 1–97 | interface: contract, `struct efs_wb_job`, `struct efs_wb_state`, `extern g_wb` | yes |
| `src/client/efs_fuse/writeback.c` | 26–31 | `g_wb` initialiser | yes |
| `src/client/efs_fuse/writeback.c` | 172–194 | `wb_wait_idle_budget` | yes |
| `src/client/efs_fuse/writeback.c` | 198–313 | `efs_wb_thread`: exit condition, error record | yes |
| `src/client/efs_fuse/writeback.c` | 315–351 | `wb_worker_count`, `efs_wb_ensure` start/failure | yes |
| `src/client/efs_fuse/writeback.c` | 359–441 | `efs_wb_enqueue_owned`, shutdown refusal | yes |
| `src/client/efs_fuse/writeback.c` | 443–502 | `efs_wb_sync`, `efs_wb_ino_pending_locked`, `efs_wb_sync_ino` | yes |
| `src/client/efs_fuse/writeback.c` | 1–25, 33–170 | stripes, bounce pool, log, overlap helpers | no: same file, other concerns |
| `src/client/efs_fuse/main.c` | 28–50 | `efs_fuse_destroy`: flush, then stop (locks `g_wb.mu`, sets `shutdown`, joins `workers[]`, clears `ready`) | yes |
| `src/client/efs_fuse/main.c` | 120–133 | rest of destroy: no later pool use | yes |
| `src/client/efs_fuse/main.c` | 1–27, 51–119, 134–175 | unclean flag, K90 dcache drain, `ll_*` wrappers | no: dead end (K90 drain is not the pool) |
| `src/client/efs_fuse/release.c` | 55–80 | `efs_control_drain` polls `g_wb.count || g_wb.inflight` under `g_wb.mu` | yes |
| `src/client/efs_fuse/release.c` | 81–95 | rest of control drain | no |
| `src/client/efs_fuse/append.c` | 40–55 | `efs_ino_has_queued_write` takes `g_wb.mu`, calls `efs_wb_ino_pending_locked` | yes |
| `src/client/efs_fuse/append.c` | 30–39, 56–65 | neighbours | no: overshoot |
| `src/client/efs_fuse/open.c` | 268–282 | `wb_lock_budget` (drain lock budget) | yes |
| `src/client/efs_fuse/open.c` | 315–320 | second grep hit | no: dead end |
| grep `g_wb` over `src/client`, `tests` | 62 hit lines | navigation | partly (35 in `src`) |
| grep `efs_wb_\|g_wb\|EFS_WB` over the module outside writeback | 48 hit lines | navigation | yes |

Understand: opened 1,074, needed 656. Categories (needed): guide 92,
interface 97, implementation 325 (`writeback.c`), callers outside the owner
110 (`main.c` 37, `release.c` 26, `append.c` 16, `open.c` 15, plus 16 of
context), navigation 83 (one line per grep hit judged needed). Files opened:
7 source + 5 test-side.

Validate:

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests_for.py client/efs_fuse.writeback` and `.main` output, `budgets.tsv` grep | 26 | routing | yes |
| `tests/test_write_queue.py` | 1–87 | `.writeback`; own `g_wb`; splices `efs_wb_enqueue_owned`; asserts shutdown → `EFS_ERR_IO` | yes |
| `tests/test_wb_worker_budget.py` | 1–68 | `.writeback`; own `g_wb`; splices `efs_wb_thread` with `shutdown = 1` | yes |
| `tests/test_wb_drain_budget.py` | 1–44 | `.writeback`; own `g_wb`; sticky error retention and clear | yes |
| `tests/test_stop_dirty_drain.py` | 80–130 | tagged `client/write`, found by grep; own `g_wb` with `ready,count,inflight,mu`; splices `efs_control_drain` | yes, not routed |
| `tests/test_inode_lock_stripes.py` | 1–30 | bare `client/efs_fuse`; slices `writeback.c` up to `struct efs_wb_state g_wb = {` | yes (breaks if the struct moves), not routed |

Validate: 306 lines; 3 of 3 `.writeback` routes plus 2 found by grep. None
compiles `writeback.c`; every test re-declares its own `g_wb` and splices
bodies by text. No test exercises the stop sequence (`main.c` L41–50) at
all; `.main` routes ten tests, none opened, and only the live K90 test runs
destroy.

Dead ends: 3 (`main.c` K90 block, `open.c` second hit, the `graphify query`
for "who reads or writes g_wb outside writeback.c": 247 nodes, start nodes in
test helpers and `w66_readindex_latency.py`, truncated, the callers not
among the 62 shown). The brief's "`support.c`'s stats" reader does not exist:
`support.c` reaches only the second error channel (`efs_wb_inode_stalled`,
`efs_wb_description_*` in `../write.c`).

Could the investigation stop at one guide, one interface, one implementation
file and the routed tests? **No.** The header declares the whole mutable
structure and says three other files lock its mutex; the stop is
implemented in `main.c`, two pending-work checks in `release.c` and
`append.c`, the lock budget in `open.c`; the routed tests re-implement
`g_wb` and miss the stop entirely.
