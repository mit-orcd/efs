# Write-back pool ownership pilot (W171 phase 3, Task DM)

**Verdict: PASS (Nov 6). The investigation stopped at the boundary: one
guide, one interface, one implementation file and the four routed tests,
plus the 14 lines of `efs_fuse_destroy` the question names. Lines needed
691 to 593 (−98), lines opened 1,074 to 767, source files 7 to 4; lines to
validate rose 306 to 361 (the new compiled test).** `g_wb` is private to
`writeback.c`; `main.c`, `release.c` and `append.c` call `efs_wb_stop` and
`efs_wb_pending` instead of locking the pool. node9900 gate `TOTAL 592 FAIL
0 NOTRUN 4 STATUS INCOMPLETE (644s)` (the four expected NOTRUN), no
OVERBUDGET; TSan FUSE lane 3 of 3 PASS. No K row, blocks alpha: no.
Merged into devel in wave 60 (Nov 7); its `ready` race was fixed by Task DT and merged in wave 61 (Nov 7). Status Nov 9: remaining failures none; active run none; next action none on this page, W171 continues on the [structure plan](structure-plan-20261102.md).

**Headline by the [reading-set measures](reading-set-baseline-20261104.md#measures-task-ev-nov-9)** (Task EV, Nov 9; numbers from the sections below, before → after):

| Measure | Before | After |
|---|---|---|
| Understanding lines needed | 691 | 593 |
| Validation lines needed | 306 | 361 |
| Total needed | 997 | 954 |
| Lines opened (understanding / validation / total) | 1,074 / 306 / 1,380 | 767 / 405 / 1,172 |
| Irrelevant reading (opened − total needed) | 383 | 218 |
| Dead ends | 3 | 2 |
| Source files opened | 7 | 4 |

Missed dependencies, correctness and necessary expansion cannot be derived: the reading was the implementer's own, with no reference answer to compare against.

> **Current status (Nov 8, Task EB):** merged into devel in wave 60
> ([ledger](ledger.md)). The `ready` / start-failure finding left open at
> the end of this page was fixed by Task DT ([write-back `ready` flag and
> start-failure join](writeback-ready-20261107.md)) and merged in wave 61.
> The text below is the Nov 6 record, left as it was.

- Purpose: W171 phase 3 ownership pilot ([structure plan](structure-plan-20261102.md)):
  make the FUSE write-back pool own its state behind a small interface, prove
  it with a test that compiles the real module, and measure whether an
  unfamiliar investigation can stop at that boundary. No K row; blocks alpha: no.
- Source: base devel `cb51d1cd6` (wave 59). Branch
  `claude/wonderful-almeida-42f374`; before-half recorded at `44b75f278`
  before any source change; code `8cbc042ec` (tested, `EFS_GIT_ID=8cbc042ec`).
- Environment: node9900 (192 cpus, `/data1/efs/deps/libfuse`, build in
  `/tmp/efs-taskDM`, `TMPDIR=/tmp/efs-taskDM-tmp`), gcc, TCP loopback,
  buffered I/O; load average 0.2-0.4 before each run.

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

Understand: opened 1,074, needed 691. Categories (needed): guide 92,
interface 97, implementation 325 (`writeback.c`), callers outside the owner
94 (`main.c` 37, `release.c` 26, `append.c` 16, `open.c` 15), navigation 83
(35 of the first grep's 62 lines, all 48 of the second). Source files
opened: 7. (The before-commit `44b75f278` summed this as 656; its own rows
sum to 691, corrected when the after-half was counted.)

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

## The interface (`src/client/efs_fuse/writeback.h`, 65 lines, was 97)

The pool struct, its job ring, its mutex and condition variables, the
worker table and the `EFS_WB_*` sizes moved into `writeback.c`; `g_wb` is
`static` and `efs_wb_ino_pending_locked` is private. The header opens with
the contract and declares five pool operations plus the bounce buffers and
the error logger:

| Operation | Returns |
|---|---|
| `efs_wb_enqueue_owned` | the job's PUT rc after waiting for it; `EFS_ERR_NOMEM` workers could not start, `EFS_ERR_BUSY` ring full 8 s or past the caller's deadline, `EFS_ERR_IO` pool stopping (buffer released, nothing queued in those three) |
| `efs_wb_pending(ino)` (new) | `ino` 0: queued plus in-flight jobs in the pool; else 1 when a job of `ino` is queued or running; 0 before the pool starts |
| `efs_wb_sync` | waits for idle, returns and clears the sticky error (first failed PUT, any inode) |
| `efs_wb_sync_ino(ino)` | waits until no job of `ino` is pending, returns and clears the sticky error only if `ino` set it |
| `efs_wb_stop` (new) | stops and joins the workers after every queued job and the in-flight PUT finish; does not consume the sticky error; the pool is unstarted afterwards |

Both drains: `EFS_ERR_BUSY` past the caller's RPC deadline (lock or idle
wait), `EFS_ERR_IO` when a wait fails; the work and the error stay, and
`efs_wb_pending` says what was left. Lock: one private pool mutex, never
held across a call boundary. The second, per-inode error channel in
`../write.c` is named as before.

Behaviour preserved: the two new operations are the caller blocks moved
verbatim into `writeback.c` (`main.c` L41–50 becomes `efs_wb_stop`,
`release.c` L72–76 and `append.c` L48–52 become `efs_wb_pending`), so the
K90 drain in `efs_fuse_destroy` (`efs_wb_sync`, then stop, then the one-pass
dirty-chunk drain that a fresh `ENOSPC` ends, exit 1) is unchanged:
`tests/test_k90_unmount_enospc.py` and `tests/test_stop_dirty_drain.py` PASS
on the tree. No timeout, budget or sleep changed.

## Callers: before and after

From the graph's `writeback.c` neighbourhood and one grep (the brief's
"`support.c` stats" reader does not exist; `support.c` reaches only the
`../write.c` channel):

| Caller | Needed | Touched before | Calls now |
|---|---|---|---|
| `main.c` `efs_fuse_destroy` | stop the workers after the flush | `g_wb.ready`, `g_wb.mu`, `shutdown`, `not_empty`, `nworkers`, `workers[]` | `efs_wb_sync`, `efs_wb_stop` |
| `release.c` `efs_control_drain` | pool idle? | `g_wb.ready`, `g_wb.mu`, `count`, `inflight` | `efs_wb_pending(0)` |
| `append.c` `efs_ino_has_queued_write` (removed) | job pending for this inode? | `g_wb.ready`, `g_wb.mu`, `efs_wb_ino_pending_locked` | `efs_wb_pending(ino)` in `efs_ino_has_unpublished` |
| `append.c` flush-report run | drain this inode, its error | `efs_wb_sync_ino` | unchanged |
| `write.c` WRITE, `fuse_unit_begin` | submit a job, drain an inode | `efs_wb_enqueue_owned`, `efs_wb_sync_ino` | unchanged |
| `open.c` | nothing | duplicate prototypes of `efs_wb_sync` and `efs_fuse_log_err` (unused) | removed |

Comments in `append.h` and `release.h` that named `g_wb` now name the
operation. `tests/test_stop_dirty_drain.py` stubs `efs_wb_pending` instead
of declaring its own `g_wb`. The three `.writeback` splice tests keep their
own `g_wb` and pass unchanged; `tests/test_inode_lock_stripes.py`'s slice
still ends at `g_wb`'s definition (the structs sit above the stripes).

## Compiled test: `tests/test_wb_interface.c`

Links the real `src/client/efs_fuse/writeback.c` (with the libfuse headers;
it calls no libfuse function) and no `libefs.a`. The module offers no PUT
hook (no function pointer, no weak symbol), so the PUT path and the
helpers it takes from other files are link-time definitions in the test:
`efs_client_write_no_replicate` (a gate the test opens with a chosen rc),
the thread-local RPC deadline, `wb_lock_budget` (`open.c`'s
trylock-until-deadline rule, yielding instead of sleeping),
`fuse_chunk_size`, `stats_now_ms`, the lookup-memo hooks, `efs_strerror`.

| Case | Asserts |
|---|---|
| unstarted pool | `efs_wb_pending` 0 overall and per inode, both drains `EFS_OK`, stop a no-op |
| start, stop, restart | first enqueue starts workers; after stop nothing pending and drains `EFS_OK`; the next enqueue starts fresh workers |
| sticky error | a failed PUT on ino 42 returns `EFS_ERR_IO`, a write to ino 7 still succeeds, `efs_wb_sync_ino(7)` keeps the error, `efs_wb_sync_ino(42)` returns it once, `efs_wb_sync` returns it once |
| pending and deadline drains | a PUT held at the gate: pending 1 for its inode and overall, 0 for another; `efs_wb_sync` and `efs_wb_sync_ino` with a 20 ms deadline return `EFS_ERR_BUSY` within 500 ms leaving the job pending; another inode's drain `EFS_OK`; after the PUT fails the error survived the BUSY drains |
| drain across a worker stop | PUT held, a drain waiting and `efs_wb_stop` called on other threads; the stop returns only after the PUT; the waiter gets `EFS_ERR_IO`; the drain returns the error, or, if it ran after the stop reset the pool, the restarted pool's drain still returns it (the stop does not consume it) |

In `make test` (after `test_stage_evict`), in `TEST_SRCS`/`TEST_BINS`, an
explicit Makefile rule, the TSan list of `tests/sanitizer_check.sh`
(`wb_interface`) and `tests/budgets.tsv` (5 s, `client/efs_fuse.writeback`;
three runs on node9900 0.04 / 0.04 / 0.04 s, 1.25x rounded up to 5 s). The
three existing `.writeback` rows stay.

## Routing

`src/client/efs_fuse/README.md`'s writeback row names the header's five
operations and the new test; `.writeback` routes 4 tests. FUSE tests that
also exercise `src/client/write.c`, for the coverage-tag task (Task DN):
`test_wb_runtime.py` (`.write`; splices `src/client/write.c` text), and the
live mounts that write through `efs-fuse`: `test_k42_peer_stop_open.py`,
`test_stop_progress.py`, `test_k54_member2_unmount.py`,
`test_k90_unmount_enospc.py` (all `.main`). The reverse direction:
`test_stop_dirty_drain.py` is tagged `client/write` but splices
`release.c`'s `efs_control_drain`. `scripts/tests_for.py` and the
catalogue's columns are unchanged.

## After (tree `8cbc042ec`), same scenario, same counting

Entry is the same README; its writeback row now names the five operations.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–94 | guide | yes |
| `src/client/efs_fuse/writeback.h` | 1–65 | interface: contract (owner, lock, lifetime, each operation's BUSY / timeout / I/O answer, "stop does not consume the sticky error: drain with `efs_wb_sync` first"), declarations | yes |
| `src/client/efs_fuse/writeback.c` | 8–53 | private state: sizes, `struct efs_wb_job`, `struct efs_wb_state` | yes |
| `src/client/efs_fuse/writeback.c` | 73–78 | `static g_wb` | yes |
| `src/client/efs_fuse/writeback.c` | 219–241, 245–360 | `wb_wait_idle_budget`, `efs_wb_thread` (exit at `shutdown && count == 0`, error record) | yes |
| `src/client/efs_fuse/writeback.c` | 362–398, 400–488 | `wb_worker_count`, `efs_wb_ensure`, `efs_wb_enqueue_owned` | yes |
| `src/client/efs_fuse/writeback.c` | 490–576 | `efs_wb_sync`, `efs_wb_ino_pending_locked`, `efs_wb_pending`, `efs_wb_stop`, `efs_wb_sync_ino` | yes |
| `src/client/efs_fuse/writeback.c` | 1–7, 54–72, 79–218, 242–244 | stripes, bounce pool, log, overlap helpers | no: same file, other concerns |
| grep `g_wb` over `src/` | 0 hits outside `writeback.c` | navigation | yes (one step, no lines) |
| grep `efs_wb_` over the module outside `writeback.c` | 18 hit lines | navigation: every caller is on an operation | yes |
| `src/client/efs_fuse/main.c` | 28–41 | destroy calls `efs_wb_sync`, then `efs_wb_stop` | yes (the caller the question names) |

Understand: opened 767 (was 1,074, −29 %), needed 593 (was 691, −98 lines,
−14 %). Categories (needed): guide 94, interface 65 (was 97),
implementation 402 (was 325: the 46 lines of structs moved in from the
header, plus `efs_wb_pending` and `efs_wb_stop`), callers 14 (was 94),
navigation 18 (was 83). Source files opened: 4 (was 7); `release.c`,
`append.c` and `open.c` are no longer needed: the first two call
`efs_wb_pending`, whose contract says it never blocks past the pool mutex,
and the drain lock budget is stated in the header and exercised by the
compiled test.

Validate:

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests_for.py client/efs_fuse.writeback` output, the four `budgets.tsv` rows | 8 | routing | yes |
| `tests/test_wb_interface.c` | 1–198 | `.writeback`; the real `writeback.c`; stop with a drain waiting, the stranded-error rule, deadline drains | yes |
| `tests/test_wb_worker_budget.py` | 1–68 | `.writeback`; a queued job with `shutdown = 1` is run before the worker exits | yes (the interface test stops only with the job in flight) |
| `tests/test_write_queue.py` | 1–87 | `.writeback`; an enqueue during the stop gets `EFS_ERR_IO` | yes |
| `tests/test_wb_drain_budget.py` | 1–44 | `.writeback`; sticky error kept on BUSY | no: dead end, the interface test asserts the same on the real module |

Validate: opened 405 (was 306), needed 361 (was 306). Routed tests: 4 of 4
opened, none found outside the route (was 3 routed plus 2 by grep); one
compiles the module. The rise is the 198-line compiled test.

Dead ends: 2 (was 3): `test_wb_drain_budget.py` (above) and the same
`graphify query` on the rebuilt graph (255 nodes, the same test-helper start
nodes, truncated). A narrower `graphify query "efs_wb_stop efs_wb_pending
callers"` returned 12 nodes without the callers.

**Could the investigation stop at the boundary? Yes, with one 14-line
look outside it.** One guide, one interface, one implementation file and
the four routed tests answer both halves of the question; the only lines
read outside them are the 14 of `efs_fuse_destroy` that the question names,
to confirm it drains before it stops. Lines needed fell by 98 (691 to 593),
lines opened by 307; validation lines rose by 55 because the routed tests
now include a compiled one. The second half of the question (can another
reader race the stop) is now decided inside `writeback.c` alone: every
read and write of `ready` and `shutdown` is there.

Finding left open (not fixed: behaviour preserved, TSan did not report it
in `wb_interface`, `d4_progress` or `k38_swept_hint_live`): `g_wb.ready` is
read without the pool mutex by `efs_wb_ensure`, `efs_wb_sync`,
`efs_wb_sync_ino` and `efs_wb_pending`, and `efs_wb_stop` writes `ready`
and `shutdown` after the join without it; `efs_wb_ensure`'s start-failure
path unlocks before joining with `ready == 0`, so a second starter could
reuse `workers[]` mid-join. In production `efs_fuse_destroy` runs after the
FUSE loop has ended, so only the control drain can overlap the stop. The
fix is now confined to `writeback.c` (atomic `ready`, a start/stop lock). Fixed Nov 7 by Task DT
([write-back `ready` flag and start-failure join](writeback-ready-20261107.md)):
both flags under the pool mutex, one join helper for stop and failed start.

## Proof

| Check | Result |
|---|---|
| `make -j32 all` (`EFS_GIT_ID=8cbc042ec`) | rc 0, 10 s; 4 warnings, all in files this task did not touch (`src/client/write.c` L2484, `src/server/put_upload.c` L181, `src/server/packer.c` L1322, `tests/test_kv_compact_manifest_view.c` L1); none in `src/client/efs_fuse/` or the new test |
| `./tests/test_wb_interface` ×3 | PASS, 0.04 / 0.04 / 0.04 s |
| `make test` | rc 0 |
| `python3 scripts/tests_for.py client/efs_fuse --run` (53 rows, libfuse at `/data1/efs/deps/libfuse`) | rc 0 |
| `tests/test_stop_dirty_drain.py`, `tests/test_k90_unmount_enospc.py` | PASS, PASS |
| `EFS_SAN_ONLY="wb_interface d4_progress k38_swept_hint_live" make tsan-check` | `PASS wb_interface (0s)`, `PASS d4_progress (119s)`, `PASS k38_swept_hint_live (18s)`, `VERDICT: PASS (3 run, 0 failed, 137s)` |
| `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` (`make test-all`) | `TOTAL 592 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (644s)`; NOTRUN: `test_cluster_stop` (no cluster scripts), `test_rdma_memcheck` and `test_leak_gate` (no valgrind), `test_qualify_rdma_live` (no RDMA hardware); no OVERBUDGET; `test_wb_interface` 0.0 s of 5, `test_k90_unmount_enospc` 19.0 s of 30, `test_stop_dirty_drain` 0.1 s of 30 |

Load: 0.2-0.4 before the build, `make test` and TSan; the gate started at
4.5 (the TSan tail) and fell below 1 by its end, so no timing claim beyond
"no row over budget" is made from it.

Reproduce (Linux host with gcc, libfuse >= 3.12, and the repository at the
tested commit):

```
git archive --prefix=efs-taskDM/ 8cbc042ec | ssh <host> 'tar -C /tmp -xf -'
cd /tmp/efs-taskDM && export TMPDIR=/tmp/efs-taskDM-tmp EFS_GIT_ID=8cbc042ec
mkdir -p $TMPDIR && make -j32 all && ./tests/test_wb_interface && make test
python3 scripts/tests_for.py client/efs_fuse --run
EFS_SAN_ONLY="wb_interface d4_progress k38_swept_hint_live" make tsan-check
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh   # summary.txt line 1
rm -rf /tmp/efs-taskDM /tmp/efs-taskDM-tmp           # cleanup
```

The reading-set counts are reproduced by opening the files and ranges in the
two tables on `cb51d1cd6` (before) and `8cbc042ec` (after); the method is
[the baseline's](reading-set-baseline-20261104.md).

## Limits

- One investigation, counted by the agent that then made the change: the
  after-reader was not blind to the before-reading or to the code.
- The compiled test stubs the PUT path at link time; the module has no
  hook. It proves the pool's own lifecycle, error and deadline rules, not
  the PUT itself (the live FUSE tests and TSan's `d4_progress` cover that).
- The stop with a queued but not yet running job is covered by the splice
  test `test_wb_worker_budget.py`, not by the compiled test.
- The `ready` / start-failure race above is recorded, not fixed.
- One gate on one host (node9900); fstor007 not run.

## Raw data

On node9900: `~erbmi1/efs-artifacts/taskDM-20261105/` (23 files, 604 KiB:
`summary` of the run script, `make-test.log`, `fuse-list.txt`,
`fuse-run.log`, `stop_dirty.log`, `k90.log`, `wbi.{1,2,3}.{out,time}`,
`tsan.log`, `gate.log`, the gate's `summary.txt`, `progress.txt`,
`timings.tsv`, `gate-tmp-logs.tar.gz`, `build.log`, load snapshots,
`git-head.txt`), `SHA256SUMS` sha256
`116c9cab87ce50f2d23997aa2d888118b4898a0255271490cf5028cc26a8354b`.
`/tmp/efs-taskDM*` removed from the host.
