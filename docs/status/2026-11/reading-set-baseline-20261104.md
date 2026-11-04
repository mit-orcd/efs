# Reading-set baseline for three representative fixes (W171 phase 3, Task DB)

**Verdict: baseline recorded** (Nov 4, 2026). Purpose: the "before" measurement for the FUSE per-file interfaces ([structure plan](structure-plan-20261102.md), phase 3 (d)). The same method is repeated after (c) lands. No product code or test changed; no K row.

- Source: `devel` at `628e402f6` (wave 55 merged, `efs_fuse.c` split, component README and per-file catalogue routes in place; `raft_host.c` unsplit).
- Target per fix: one guide, one interface, one implementation area, the routed tests.
- "Needed" is the reviewer's call after making the change on paper: a line is needed when the change could not be written or checked without it. Lines are what was printed into the agent's context; ranges were located by the guide, a graph `explain`, or one grep, so whole-file totals (given per fix) are the upper bound a reader without grep pays.

## Summary

| Fix | Guide took me to the right file first time | Lines to understand (needed / opened) | Lines to validate | Files opened | Dead ends | Verdict against the target |
|---|---|---|---|---|---|---|
| 1 release cleanup | handler yes, drain no | 405 / 650 | 187 | 13 | 3 | guide yes; interface: a 39-line slice of `fuse_internal.h`; implementation: two module files plus two client files; routed tests miss the change |
| 2 ReadIndex refusal | file only (26,112 lines) | 646 / 689 | 138 | 7 | 1 | no guide below file level, no interface, implementation area built by grep; 17 routed tests, 6 touch the functions |
| 3 write-back error | yes | 450 / 540 | 74 | 13 | 2 | guide yes and its contract line states the retention rule; interface yes; implementation: three module files plus two outside; routed tests cover the pool, not FLUSH/FSYNC |

Whole-file upper bounds: fix 1 `README.md` + `release.c` + `fuse_internal.h` + `open.c` = 1,633 lines; fix 2 `raft_host.c` alone = 26,112; fix 3 `README.md` + `fuse_internal.h` + `writeback.c` + `write.c` + `support.c` + `append.c` = 3,460.

## Fix 1: RELEASE drains retained lease releases

Concrete instance: the spin in `efs_fuse_release_ino` (`release.c` L35–41) that waits `CLOSE_ADMIT_SPINS` positions for a CLOSE identity before leaving the release to the background drain (`efs_open_drain_pending`, `open.c`).

Entry: `src/client/efs_fuse/README.md`. Its symptom map sends RELEASE to `release.c` (right, first time) and says to read `fuse_internal.h` first. The map's `open.c` row names "OPEN / CREATE references, lease acquire, retry fairness, BUSY, READ": it does not say that the retained-release table and the drain live there, so the second file came from the handler's callees (`efs_close_note`, `efs_open_release_pending`, `efs_open_close_unadmitted`), not from the guide.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–82 | guide | yes |
| `src/client/efs_fuse/release.c` | 1–47 | implementation: `efs_fuse_release_ino`, the spin loop | yes |
| `src/client/efs_fuse/release.c` | 48–210 | control drain, RENAME, LSEEK | no: same file, other operations |
| `src/client/efs_fuse/fuse_internal.h` | 120–142 | interface: `struct efs_open_ref`, `CLOSE_ADMIT_SPINS` and its comment | yes |
| `src/client/efs_fuse/fuse_internal.h` | 285–300 | interface: `open.c` prototypes | yes |
| `src/client/efs_fuse/fuse_internal.h` | 100–119 | readdir structs | no: range overshoot |
| `src/client/efs_fuse/open.c` | 1–21 | shared state and lock comment: `g_open_mu`, the table, the hash | yes |
| `src/client/efs_fuse/open.c` | 22–60 | hash link and unlink helpers | no: only `open_ref_find` matters |
| `src/client/efs_fuse/open.c` | 120–127 | tail of `efs_open_retry_order` | no: overshoot |
| `src/client/efs_fuse/open.c` | 128–155 | implementation: `efs_close_note` sets `close_pending` | yes |
| `src/client/efs_fuse/open.c` | 157–261 | implementation: `efs_open_release_pending_run`, `efs_open_release_pending`, `efs_open_close_unadmitted` | yes |
| `src/client/efs_fuse/open.c` | 263–277 | `wb_lock_budget` | no |
| `src/client/efs_fuse/open.c` | 279–330 | implementation: `efs_open_drain_pending`, the background pass the spin trades against | yes |
| `src/client/inode_rpc.c` | 493–512 | shared state: `efs_client_opid_wait`, the identity window the spin waits on | yes |
| `src/client/client_internal.h` | 30–40 | the lock rule the README cites ("never held across an RPC") | yes, but it is one line inside a staged-writeback comment |

Understand: needed 405, opened 650. Categories (needed): guide 82, interface 39, implementation 232, shared state and locks 52.

Validate: `python3 scripts/tests_for.py client/efs_fuse.release` routes one test, `test_rename_flags.py`, which exercises RENAME dispatch. Every test that exercises `efs_close_note`, `efs_open_release_pending` or `CLOSE_ADMIT_SPINS` is under `.open` (7 rows) or `.plock_init`:

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests/budgets.tsv` | the `.release`, `.open`, `.plock_init` rows (grep) | routing | yes (10) |
| `tests/csrc.py` | 24–44 | module order of the splice | yes (21) |
| `tests/test_rename_flags.py` | 1–20 | routed `.release`; RENAME flags | no: dead end (20) |
| `tests/test_fuse_lease_retention.py` | 1–32, 70–80, grep hits | `.open`; splices the `open.c` range by text and stubs `efs_fuse_release_ino` at L106 | partly: validates the table, not the spin (60) |
| `tests/test_open_lease.py` | 74–134 | `.open`; splices `efs_fuse_release_ino` and the `open.c` functions by name; asserts the at-release drain ("not left to the 10/s background drain") | yes (61) |
| `tests/test_open_busy.py` | grep | `.open`; splices the struct, the macro and the `open.c` range | yes (3) |
| `tests/test_flock_description_release.py` | grep | routed `.plock_init`; redefines `CLOSE_ADMIT_SPINS 250u` itself and splices the handler | yes (12) |

Validate: 187 lines; 4 of the 8 routed tests opened plus 1 found by grep outside the route. All splice by text (`csrc.module('efs_fuse')` plus a regex per function); none compiles the module.

Dead ends: `test_rename_flags.py` (the only `.release` route), `release.c` L48–210 (same file, unrelated operations), `test_fuse_lease_retention.py` stubs the handler under test.

## Fix 2: when a read-index wait is refused or retried

Concrete instance: the `EFS_ERR_BUSY` on deadline in `host_read_index_admitted` (`raft_host.c` L5555–5559) and the `efs_apply_wait_expired` refusal in `host_wait_applied_mode` (L5754–5759), with the admission refusals in `host_read_admit` and `host_read_admit_view` (`EFS_ERR_NOT_PRIMARY`, `EFS_ERR_BUSY` at round overflow).

Entry: `src/server/README.md` names `raft_host.c` and says ReadIndex is in it and that "the host's lock order is not stated in one place". It has no symptom map below file level and lists `server/raft_host` as the test tag (the dotted `server/raft_host.read_index` works but is not in the README). The function index came from the plan's prefix map (`host_read_` 22, `host_wait_` 8) and one grep for their definitions.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/server/README.md` | 1–54 | guide | yes |
| `docs/status/2026-11/structure-plan-20261102.md` | 26–33 | prefix map | yes |
| grep: `host_read_*` / `host_wait_*` definitions in `raft_host.c` | 27 hits | navigation | yes |
| `src/server/raft_host.c` | 59, 95, 102 | interface: `HOST_TICK_US`, `HOST_READ_TRIES`, `HOST_APPLY_WAIT_CAP_US` | yes |
| `src/server/raft_host.c` | 378 | `struct host_read_floor` | yes |
| `src/server/raft_host.c` | 430–440, 518–528 | lock comments: `h->mu`, `snap_mu`, `cv_mu` and `applied_cv` | yes |
| `src/server/raft_host.c` | 4374–4415 | shared state: `struct host_view`, `host_view_get` | yes |
| `src/server/raft_host.c` | 4417–4423 | `host_halted_hint` | no |
| `src/server/raft_host.c` | 4426–4500 | predicates: `host_view_covers`, `host_view_quorum_index`, `host_view_wait_apply`, `host_read_wait_ready`, `host_read_floor_ready` | yes |
| `src/server/raft_host.c` | 4512–4550 | `host_waiter_sleep` | yes |
| `src/server/raft_host.c` | 4552–4560 | pump wake comment | no: overshoot |
| `src/server/raft_host.c` | 5272–5297 | implementation: `struct host_read_admission`, `host_read_admit` | yes |
| `src/server/raft_host.c` | 5299–5318 | `host_read_probe_needed` (pair path) | no |
| `src/server/raft_host.c` | 5320–5397 | implementation: `host_read_note_begin`, `host_read_begin_tracked`, `host_read_admit_view`, `host_read_request`, `host_read_start_requested` | yes |
| `src/server/raft_host.c` | 5400–5592 | implementation: `host_read_index_admitted`, `host_read_index` | yes |
| `src/server/raft_host.c` | 5594–5600 | pair comment | no |
| `src/server/raft_host.c` | 5713–5790 | implementation: `host_wait_applied_mode` and its two wrappers | yes |

Understand: needed 646, opened 689. Categories (needed): guide 62, navigation 27, interface 4, implementation 375, shared state and locks 178. Not opened but on the path: `host_remote_read_index`, `host_follower_read_result`, `host_group_incompatible`, `host_past_deadline`, and `apply_wait.h` (`efs_apply_wait_*` decides the BUSY in `host_wait_applied_mode`); a real change to the retry would open them.

Validate: `python3 scripts/tests_for.py server/raft_host.read_index` routes 17 tests. All 17 read `csrc.module('raft_host')` and slice by text; 6 name the read or wait functions (`test_readindex_result` 22 mentions, `test_readindex_tickets` 10, `test_readindex_admission` 9, `test_raft_read_admission` 6, `test_readindex_pair` 2, `test_read_waiters` 1); 11 do not.

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests/budgets.tsv` | the 17 `.read_index` rows | routing | yes (17) |
| grep over the 17 tests for the five function names | 17 lines | navigation | yes (17) |
| `tests/csrc.py` | 24 | module | yes (1) |
| `tests/test_readindex_admission.py` | 1–58 | slices `host_read_admit` by `s.index(...)`; asserts the `EFS_ERR_BUSY`, `EFS_ERR_NOT_PRIMARY` and propagated `EFS_ERR_IO` refusals | yes (58) |
| `tests/test_raft_read_admission.py` | 1–45 | slices `host_read_note_begin` through `host_read_index_admitted` with its own `struct host_view` | yes (45) |

Validate: 138 lines; 2 of 17 opened. None compiles `raft_host.c`.

Dead ends: `host_read_probe_needed` (pair path, not on the refusal). The graph's broad query was a dead end (below).

## Fix 3: how `g_wb.err` and `err_ino` are set, retained and surfaced on FLUSH and FSYNC

Concrete instance: the worker records the first failure (`writeback.c` L282–299), `efs_wb_sync` and `efs_wb_sync_ino` return and clear it (L440–498), and the FLUSH and FSYNC handlers in `write.c` surface a result through `efs_file_sync_error`.

Entry: `src/client/efs_fuse/README.md`. "Write-back pool" to `writeback.c` and "FSYNC, FLUSH" to `write.c`, both right first time, and the contract line for `g_wb` states the rule under change ("`err` and `err_ino` retain the first failure until drained"). What the guide does not say: the handlers consult a second error channel, the per-inode record in `src/client/write.c` (`efs_wb_inode_stalled`, `efs_wb_description_sync`) via `efs_file_sync_error` in `support.c`; `g_wb.err_ino` reaches a handler only through `efs_wb_sync_ino` inside `efs_append_flush_report_run` (`append.c` L86) and `fuse_unit_begin`.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–82 | guide | yes |
| `src/client/efs_fuse/fuse_internal.h` | 150–200 | interface: `efs_wb_job`, `efs_wb_state` with `err`, `err_ino` | yes |
| `src/client/efs_fuse/fuse_internal.h` | 313–323 | interface: `writeback.c` prototypes | yes |
| `src/client/efs_fuse/writeback.c` | 265–300 | implementation: worker keeps the first failure, logs follow-ons | yes |
| `src/client/efs_fuse/writeback.c` | 360–380 | implementation: enqueue, the comment on why the error is not sticky per file | yes |
| `src/client/efs_fuse/writeback.c` | 440–498 | implementation: `efs_wb_sync`, `efs_wb_ino_pending_locked`, `efs_wb_sync_ino` | yes |
| `src/client/efs_fuse/write.c` | grep for `efs_wb_`, `fsync`, `flush` | navigation (40 hits) | yes |
| `src/client/efs_fuse/write.c` | 135–155 | `fuse_unit_begin` drains with `efs_wb_sync_ino` | yes |
| `src/client/efs_fuse/write.c` | 120–134, 157–160, 555–560 | neighbours | no: overshoot |
| `src/client/efs_fuse/write.c` | 561–637 | implementation: `efs_fuse_fsync_ino`, `efs_fuse_flush_ino` | yes |
| `src/client/efs_fuse/support.c` | 479–487 | implementation: `efs_file_sync_error` | yes |
| `src/client/efs_fuse/support.c` | 470–478, 489–500 | neighbours | no: overshoot |
| `src/client/write.c` | 125–150 | shared state outside the module: `efs_wb_inode_stalled`, the per-inode error record under `g_wb_error_mu` | yes |
| `src/client/efs_fuse/append.c` | 118–130 | `efs_append_flush_report` deadline wrapper | yes |
| `src/client/efs_fuse/append.c` | 45–48, 86 (grep) | the flush-report run takes `g_wb.mu` and calls `efs_wb_sync_ino` | yes |
| `src/client/efs_fuse/append.c` | 132–175 | pressure flush and its worker | no |

Understand: needed 450, opened 540. Categories (needed): guide 82, interface 62, implementation 240, shared state 26, navigation 40.

Validate: `.writeback` routes 3 tests, `.write` routes 1.

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests/budgets.tsv` | 4 rows | routing | yes (4) |
| `tests/test_write_queue.py` | grep | `.writeback`; splices `efs_wb_enqueue_owned`; no error assertions | partly (12) |
| `tests/test_wb_worker_budget.py` | grep | `.writeback`; own `g_wb` with `err`, `err_ino`; splices `efs_wb_thread` | yes (12) |
| `tests/test_wb_drain_budget.py` | 18–37 | `.writeback`; asserts `err` and `err_ino` retention and clearing in `efs_wb_sync` and `efs_wb_sync_ino` | yes (20) |
| `tests/test_write_budget.py` | 10–30 | `.write`; stubs `efs_fuse_fsync_ino` | no for surfacing (21) |
| `tests/test_wb_runtime.py` | grep | tagged `client/write`; splices `efs_file_sync_error` | yes, not routed (5) |
| `tests/test_flock_description_release.py` | (fix 1) | tagged `.plock_init`; splices `efs_fuse_flush_ino` | yes, not routed |

Validate: 74 lines; 4 of 4 routed tests opened plus 2 found by grep under other tags. All splice by text. The pool half of the change is pinned by `test_wb_drain_budget.py`; the FLUSH/FSYNC half has no routed test.

Dead ends: `test_write_budget.py` for surfacing (it mocks the handler); the graph's `wb_error_get` hit landed in `src/client/write.c`, the other `write.c`, which turned out to be the second channel rather than a miss, but the name collision cost one check.

## Graph

| Command | Nodes | Useful |
|---|---|---|
| `graphify query "RELEASE drains retained lease releases open ref table CLOSE_ADMIT_SPINS"` | 550 (63 shown) | no: start nodes `release()`, `table()`, `admit()` in test helpers |
| `graphify query "read-index wait refused or retried host_read host_wait raft_host"` | 662 (59 shown) | no: start nodes `refused`, `index`, `wait` |
| `graphify query "write-back error g_wb.err err_ino surfaced on FLUSH FSYNC"` | 718 (58 shown) | no: start nodes `error()`, `fsync()` in tests; `wb_error_get()` in `src/client/write.c` by accident |
| `graphify explain efs_fuse_release` | 1 node, 22 edges | yes: callees name `efs_open_release_pending`, `efs_close_note`, `open_edge_mu`, which is the `open.c` route the README lacks |
| `graphify explain CLOSE_ADMIT_SPINS` | none | macros have no node |
| `graphify explain host_read_wait` | 1, 6 | partial: the predicate's neighbours only |
| `graphify explain host_read_index_admitted` | 1, 37 | yes, once the name is known |
| `graphify explain wb_error_get` | 1, 7 | points at the per-inode record in `src/client/write.c` |
| `graphify explain efs_wb_sync_ino` | 1, 9 | yes |
| `graphify explain efs_fuse_flush_ino` | 1, 10 | yes |

Would a ranked starting set have replaced reading? Only after the symptom-to-symbol step: `explain` on an exact function name gives a correct neighbourhood (for fix 1 it would have saved the guess that the drain is in `open.c`), but the broad query picks start nodes by token match and never reached a fix. None of the explains replaces reading the implementation lines; they replace the navigation lines (27 for fix 2, 40 for fix 3).

## Method (repeat after the per-file interfaces land)

Same three fixes, same concrete instances, same devel-only tree, nothing built or run on a host.

1. Run the three broad queries and the explains above; record node counts.
2. Enter from the component README (`src/client/efs_fuse/README.md`, `src/server/README.md`); note whether the symptom map names the first file and the second.
3. Read as an agent making the change: the handler, its callees in the module, the header slice the handler needs, the lock and lifetime comments, anything outside the module the handler consults. Print only grep-located ranges; record each range with category and needed yes/no.
4. Route tests with `python3 scripts/tests_for.py <tag>` (`client/efs_fuse.release`, `.open`, `server/raft_host.read_index`, `client/efs_fuse.writeback`, `.write`); grep the routed files for the functions under change; open the ones that name them; record which splice by text (`csrc.module`) and which compile the real module through an interface.
5. Fill the tables; dead ends are opened files that did not matter, with the reason.

Reproduce the counts here (any host with the checkout, no cluster):

```
graphify query "RELEASE drains retained lease releases open ref table CLOSE_ADMIT_SPINS"
graphify explain efs_fuse_release
python3 scripts/tests_for.py client/efs_fuse.release
python3 scripts/tests_for.py client/efs_fuse.open
python3 scripts/tests_for.py server/raft_host.read_index
python3 scripts/tests_for.py client/efs_fuse.writeback
python3 scripts/tests_for.py client/efs_fuse.write
grep -n "^static.*host_read_[a-z_]*(\|^static.*host_wait_[a-z_]*(" src/server/raft_host.c | grep -v ";"
```

What (c) must move for the after-measurement to show a reduction: fix 1 needs the retained-release table and drain named in the guide and a `.release` route that reaches `test_open_lease.py` and `test_flock_description_release.py`; fix 3 needs the FLUSH/FSYNC surfacing tests routed under `.write` and the second error channel named in the contract; fix 2 is the unsplit state and changes only with the ReadIndex pilot (after W123).
