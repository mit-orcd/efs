# Reading set after the FUSE interfaces and routing (W171 phase 3 (d), Task DD)

**Verdict: FAIL against the plan's condition** (measured Nov 5, 2026). The pilot did not reduce the lines an agent must read: lines needed rose on both FUSE fixes and total lines read fell only on fix 1, by an amount that depends on one skipped range. Dead ends fell on both, validation lines fell on both, and the guide now names every second file. The `meta_apply.c`, `write.c` and `raft_host.c` splits stay held by the plan's wording until the user decides. Purpose: the "after" half of [the baseline](reading-set-baseline-20261104.md), same fixes, same method. No K row, blocks alpha: no.

- Source: after DC, `devel` at `42bc2f046` (wave 56, per-area headers, `raft_host.c` unsplit). After routing, `8555dd4b4` on this branch (subsystem column and component README only). Baseline: `628e402f6`.
- Environment: a checkout only. No build, no test run, no cluster, no host. Buffered/direct I/O, transport and fixture size do not apply.
- Counting: as the baseline. Lines are what was printed into the agent's context; "needed" is the reviewer's call; headers count as printed, with needed marked per block.

## Summary

The baseline's table, at the after-routing state:

| Fix | Guide took me to the right file first time | Lines to understand (needed / opened) | Lines to validate | Files opened | Dead ends | Verdict against the target |
|---|---|---|---|---|---|---|
| 1 release cleanup | handler yes, drain yes | 434 / 528 | 143 | 12 | 2 | guide yes; interface no (`release.h` and `open.h`); implementation no (`release.c`, `open.c`, one client file); routed tests yes for the spin, `.open` for the table |
| 2 ReadIndex refusal | file only (26,112 lines) | 646 / 689 | 138 | 7 | 1 | unchanged by design: no guide below file level, no interface, implementation area built by grep |
| 3 write-back error | yes, second channel too | 460 / 566 | 49 | 16 | 1 | guide yes; interface no (`writeback.h`, `write.h`, slices of `support.h` and `append.h`); implementation no (three module files, one outside); routed tests yes for the pool and the helper, none drives a handler through the real helper |

Before and after, per fix:

| Measure | Fix 1 before | after DC | after routing | Fix 2 before | after | Fix 3 before | after DC | after routing |
|---|---|---:|---:|---|---:|---|---:|---:|
| Lines needed | 405 | 433 | 434 | 646 | 646 | 450 | 459 | 460 |
| Lines opened | 650 | 527 | 528 | 689 | 689 | 540 | 565 | 566 |
| Lines to validate | 187 | 184 | 143 | 138 | 138 | 74 | 52 | 49 |
| Opened plus validate | 837 | 711 | 671 | 827 | 827 | 614 | 617 | 615 |
| Files opened | 13 | 14 | 12 | 7 | 7 | 13 | 17 | 16 |
| Dead ends | 3 | 3 | 2 | 1 | 1 | 2 | 2 | 1 |
| Whole-file upper bound | 1,633 | 1,313 | 1,314 | 26,112 | 26,112 | 3,460 | 3,314 | 3,315 |

Verdict in one paragraph: the pilot moved reading rather than removing it. Contract text that was scattered or unstated now sits at the top of each header, so interface lines rose from 39 to 96 on fix 1 and from 62 to 110 on fix 3, and those lines replace less implementation than they add. Lines read fell on fix 1 (837 to 671) but 163 of that is not opening the rest of `release.c`; fix 3 is flat (614 to 615). Files fell by one on fix 1 and rose by three on fix 3, because one header per area means several small headers where one large one was. Dead ends fell from 3 to 2 and from 2 to 1, validation lines fell by 44 and 25 (routing did most of that), and the whole-file bound a reader without grep pays fell 20% and 4%. The target "one guide, one interface, one implementation area, the routed tests" is met in two of four parts by fixes 1 and 3 (guide, routed tests) and in none by fix 2.

## The two gaps

Both had a text half that DC had already closed and a routing half that it had not.

- Gap 1. The `open.c` row already named "retained last closes and their background drain" and the open-ref table, and `release.h` L8–16 names the retained row and the retry thread. Added: the `release.c` row now points at them. Routing moved `test_open_lease.py` from `.open` and `test_flock_description_release.py` from `.plock_init` to `.release`.
- Gap 2. `writeback.h` L34–36, `write.h` L19–22, `support.h` L46–49 and the README already named the second channel. Added: "reached through `efs_file_sync_error`" in the README contract. Routing moved `test_wb_runtime.py` from `client/write` to `.write`, so the bare `client/efs_fuse` route lists 51 rows and `client/write` keeps 40.

```
$ python3 scripts/tests_for.py client/efs_fuse.release
python3 tests/test_rename_flags.py
python3 tests/test_flock_description_release.py
python3 tests/test_open_lease.py
$ python3 scripts/tests_for.py client/efs_fuse.write
python3 tests/test_wb_runtime.py
python3 tests/test_write_budget.py
```

Consequences of the single-valued subsystem column:

- `tests_for.py client/efs_fuse.plock_init` now exits 1, "no catalogue row". The flock test was that route's only row; the README row says so instead of a count. Nothing else names a `plock_init.c` function.
- The baseline's two gaps both name `test_flock_description_release.py`. It went to `.release`: it stubs `efs_file_sync_error` (L46), so it is not a surfacing test.
- `test_wb_runtime.py` also splices `src/client/write.c` and `writer_state.c` directly, so it spans two subsystems; the brief put it under `.write`, and `client/write` no longer lists it.

## Fix 1: RELEASE drains retained lease releases

Entry: `src/client/efs_fuse/README.md`. RELEASE goes to `release.c` and `release.h`; `release.h` says the row is retained for the retry thread, and the callees are in `open.h`. The second file now comes from the guide.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–85 | guide | yes |
| `src/client/efs_fuse/release.h` | 1–33 | interface: contract, `CLOSE_ADMIT_SPINS` | yes |
| `src/client/efs_fuse/release.h` | 34–41 | other prototypes | no |
| `src/client/efs_fuse/open.h` | 6–61, 69–75 | interface: contract, locks, `struct efs_open_ref`, edge prototypes | yes (63) |
| `src/client/efs_fuse/open.h` | 1–5, 62–68, 76–92 | guard, `struct efs_pending_create`, OPEN and READ prototypes | no (29) |
| `src/client/efs_fuse/fuse_internal.h` | grep | 8 hits, the handler's helpers | no: dead end |
| `src/client/efs_fuse/release.c` | 1–52 | implementation: includes, `efs_fuse_release_ino`, the spin | yes |
| `src/client/efs_fuse/open.c` | 1–22 | includes and table comment | no: `open.h` now states the locks and the row's life |
| `src/client/efs_fuse/open.c` | 129–132 | tail of `efs_open_retry_order` | no |
| `src/client/efs_fuse/open.c` | 133–266 | implementation: `efs_close_note`, `efs_open_release_pending_run`, `efs_open_release_pending`, `efs_open_close_unadmitted` | yes (134) |
| `src/client/efs_fuse/open.c` | 267–283 | `wb_lock_budget` | no |
| `src/client/efs_fuse/open.c` | 284–335 | implementation: `efs_open_drain_pending` | yes (52) |
| `src/client/inode_rpc.c` | 497–510 | shared state: `efs_client_opid_wait` | yes |
| `src/client/inode_rpc.c` | 511–516 | next function | no |

Understand after DC: needed 433, opened 527. Categories (needed): guide 85, interface 96, implementation 238, shared state 14. Against the baseline: `client_internal.h` is no longer opened (`open.h` states the rule), and `open.c` L1–22 is no longer needed. After routing the README is one line longer: 434 / 528.

The 123-line fall in opened lines is more than explained by not opening `release.c` past the handler (163 lines in the baseline). Counted without that range, the baseline is 487 and the after is 527: the headers add 40 lines.

Validate:

| File | Lines | After DC | After routing |
|---|---|---:|---:|
| `tests/budgets.tsv` | route rows | 9 (`.release`, `.open`, `.plock_init`) | 3 (`.release`) |
| `tests/csrc.py` | 20–80: the module list and `hblock` (37 needed, 24 overshoot) | 61 | 61 |
| `tests/test_open_lease.py` | 74–134; asserts the at-release wait (L113–116) | 61 | 61 |
| grep over the tests for the functions under change | hits | 53 over 9 files, `test_fuse_lease_retention.py` capped at 14 of 50 | 18 over 3 files |
| Total | | 184 | 143 |

Tests relied on, after DC: `test_rename_flags.py`, `test_fuse_lease_retention.py`, `test_open_lease.py`, `test_open_busy.py`, `test_flock_description_release.py`. After routing: the first, third and fifth. `test_open_busy.py` and `test_fuse_lease_retention.py` remain under `.open`, and a change to `efs_open_release_pending` itself still runs that route. All splice by text; none compiles the module through its header.

Dead ends after DC: `test_rename_flags.py` (no function under change, one grep line), `test_fuse_lease_retention.py` (stubs the handler, L106), the `fuse_internal.h` grep. After routing: the first and third.

## Fix 2: when a read-index wait is refused or retried

Unchanged by design. `raft_host.c` (blob `30bb04464`), `src/server/README.md`, `apply_wait.h`, `test_readindex_admission.py` and `test_raft_read_admission.py` are byte-identical to the baseline's commit, and no test on the route changed. The routed tests still number 17 and the anchors of the baseline's ranges sit at their recorded lines (`host_view_get` 4382, `host_waiter_sleep` 4512, `host_read_admit` 5278, `host_read_probe_needed` 5303, `host_read_index_admitted` 5400, `host_wait_applied_mode` 5713). The baseline's ranges were re-summed, and give 689 and 138. Its needed/not-needed judgements were carried over, not re-judged line by line, because the text is the same.

Two baseline counts do not reproduce on the identical file. The method's definition grep gives 29 hits, not 27, which would make the row 648 / 691 if corrected. Grepping the 17 tests for `host_read_admit`, `host_read_admit_view`, `host_read_index_admitted`, `host_read_index`, `host_wait_applied_mode` finds 7 tests (result 23, tickets 10, admission 9, read_admission 6, operation 3, pair 3, remote_pair 1), not the baseline's 6; the baseline did not record its five names. The rows above keep 646 / 689 so the pages compare.

## Fix 3: how `g_wb.err` and `err_ino` are set, retained and surfaced on FLUSH and FSYNC

Entry: the same README. "Write-back pool" to `writeback.c` and `writeback.h`, FSYNC and FLUSH to `write.c` and `write.h`. The contract now says what the baseline had to discover: the second channel in `../write.c`, read by `efs_file_sync_error`. The `wb_error_get` name-collision check is gone.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–85 | guide | yes |
| `src/client/efs_fuse/writeback.h` | 6–36, 48–86, 93–95 | interface: contract, job, state, drains | yes (73) |
| `src/client/efs_fuse/writeback.h` | 1–5, 37–47, 87–92, 96–97 | guard, depth macros, bounce prototypes | no (24) |
| `src/client/efs_fuse/write.h` | 6–23, 29–31 | interface: contract, handlers | yes (21) |
| `src/client/efs_fuse/write.h` | the rest | guard, WRITE prototypes | no (11) |
| grep `efs_file_sync_error` in the module | 7 hits | navigation | yes |
| `src/client/efs_fuse/support.h` | 44–50 | interface: the two channels | yes |
| `src/client/efs_fuse/append.h` | 16–19, 26–29, 32 | interface: flush-report contract | yes (9) |
| `src/client/efs_fuse/append.h` | the rest | reservation | no (32) |
| `src/client/efs_fuse/writeback.c` | 268–304 | implementation: worker keeps the first failure | yes |
| `src/client/efs_fuse/writeback.c` | 364–385 | enqueue | no: `writeback.h` L32 now states why a later write is not refused |
| `src/client/efs_fuse/writeback.c` | 444–502 | implementation: `efs_wb_sync`, `efs_wb_ino_pending_locked`, `efs_wb_sync_ino` | yes |
| `src/client/efs_fuse/write.c` | grep for the handlers | navigation (4 hits; the baseline needed 40) | yes |
| `src/client/efs_fuse/write.c` | 140–160, 566–642 | implementation: `fuse_unit_begin`, `efs_fuse_fsync_ino`, `efs_fuse_flush_ino` | yes (98) |
| `src/client/efs_fuse/support.c` | 476–484 | implementation: `efs_file_sync_error` | yes |
| `src/client/efs_fuse/support.c` | 485–486 | next function | no |
| grep in `src/client/write.c` and `client_internal.h` | 12 hits | navigation | yes |
| `src/client/write.c` | 120–138 | shared state: `efs_wb_description_sync`, `efs_wb_inode_stalled` | yes |
| `src/client/write.c` | 139–150 | record add | no |
| grep in `src/client/efs_fuse/append.c` | 5 hits, the `efs_wb_sync_ino` call | navigation | yes |
| `src/client/efs_fuse/append.c` | 120–133 | `efs_append_flush_report` | yes |
| `src/client/efs_fuse/append.c` | 134–136 | next comment | no |

Understand after DC: needed 459, opened 565. Categories (needed): guide 85, interface 110, implementation 217, shared state 19, navigation 28. Navigation fell from 40 to 28; interface rose by 48. After routing: 460 / 566.

Validate:

| File | Lines | After DC | After routing |
|---|---|---:|---:|
| `tests/budgets.tsv` | route rows | 4 | 5 |
| grep over the tests for the error names | hits | 28 (`test_wb_runtime.py` 13, `test_wb_drain_budget.py` 8, `test_flock_description_release.py` 4, `test_wb_worker_budget.py` 2, `test_write_budget.py` 1) | 24 (without the flock test) |
| `tests/test_wb_drain_budget.py` | 18–37; asserts retention and clearing | 20 | 20 |
| Total | | 52 | 49 |

Dead ends after DC: `test_write_budget.py` (stubs `efs_fuse_fsync_ino`), `test_flock_description_release.py` (stubs `efs_file_sync_error`). After routing: the first. `test_write_queue.py` has no hit and was not opened.

Test gap this measurement exposes: `test_wb_runtime.py` runs `efs_file_sync_error` and `test_wb_drain_budget.py` runs the drains, but no test in `tests/` drives `efs_fuse_flush_ino` or `efs_fuse_fsync_ino` through the real helper, so the handlers' `-EIO` mapping is pinned by nothing. Routing cannot fix that.

## Graph

| Command | Before | After | Useful |
|---|---|---|---|
| `graphify query "RELEASE drains retained lease releases open ref table CLOSE_ADMIT_SPINS"` | 550 nodes | 551 (64 shown) | no: start nodes `release()`, `table()`, `admit()` |
| `graphify query "read-index wait refused or retried host_read host_wait raft_host"` | 662 | 637 (59 shown) | no: start nodes `refused`, `index`, `wait` |
| `graphify query "write-back error g_wb.err err_ino surfaced on FLUSH FSYNC"` | 718 | 718 (58 shown) | no: start nodes `error()`, `fsync()`, `wb_error_get()` |
| `graphify explain efs_fuse_release` | 22 edges | 22 | yes |
| `graphify explain CLOSE_ADMIT_SPINS` | none | none | macros have no node |
| `graphify explain host_read_wait` | 6 | 6 | partial |
| `graphify explain host_read_index_admitted` | 37 | 37 | yes, once the name is known |
| `graphify explain wb_error_get` | 7 | 7 | points at the second channel |
| `graphify explain efs_wb_sync_ino` | 9 | 9 | yes |
| `graphify explain efs_fuse_flush_ino` | 10 | 10 | yes |

The graph was not what moved these numbers; the broad query still reaches no fix.

## Limitations

- One reader, one pass, three fixes. "Needed" is a judgement.
- The reader was not blind: this pass read the baseline's ranges first, as the method requires for a row-for-row page, so avoiding a dead end may owe something to knowing it. The largest case is `release.c` past the handler: if a reader still opens it, fix 1 opens 691 lines against 650, and the fall in opened lines becomes a rise. `release.h` does make the skip defensible, since it lists those operations separately.
- The after-routing validate figures keep the `csrc.py` overshoot of the after-DC pass, so the difference between those columns is routing alone.
- The moved tests were not run here. `tests/test_make_test_recipe.py` passes on the edited catalogue; the commands and their budgets did not change.

## Remaining gates

`meta_apply.c`, `write.c` and the `raft_host.c` pilot stay behind the plan's condition. Whether lower validation cost, fewer dead ends and stated contracts justify going ahead anyway is the user's call; this measurement does not make it.

## Reproduce

Any host with the checkout, no cluster, at `8555dd4b4` (or `42bc2f046` for the after-DC routes):

```
graphify query "RELEASE drains retained lease releases open ref table CLOSE_ADMIT_SPINS"
graphify query "read-index wait refused or retried host_read host_wait raft_host"
graphify query "write-back error g_wb.err err_ino surfaced on FLUSH FSYNC"
python3 scripts/tests_for.py client/efs_fuse.release
python3 scripts/tests_for.py client/efs_fuse.open
python3 scripts/tests_for.py server/raft_host.read_index
python3 scripts/tests_for.py client/efs_fuse.writeback
python3 scripts/tests_for.py client/efs_fuse.write
grep -n "^static.*host_read_[a-z_]*(\|^static.*host_wait_[a-z_]*(" src/server/raft_host.c | grep -v ";"
grep -cE 'CLOSE_ADMIT_SPINS|efs_close_note|efs_open_release_pending|efs_open_close_unadmitted|efs_fuse_release_ino|efs_open_drain_pending|hblock|csrc\.|background drain|release\.h' tests/test_open_lease.py
grep -nE 'efs_file_sync_error|efs_fuse_flush_ino|efs_fuse_fsync_ino|err_ino|g_wb\.err|efs_wb_sync|efs_wb_inode_stalled' tests/test_wb_runtime.py
git rev-parse 628e402f6:src/server/raft_host.c HEAD:src/server/raft_host.c
```

Then read the ranges in the tables above and repeat the [baseline's method](reading-set-baseline-20261104.md#method-repeat-after-the-per-file-interfaces-land). Expected: the counts above; raw output is not kept.
