# Group view ownership (Task V3)

**Current state (Nov 11):** the published group view of the metadata server is owned by `src/server/group_view.c`, and since Task V6 its header is split in two: `src/server/group_view.h` (181 lines) is the contract a caller reads (contract comment, the three constants, `struct host_view`, `struct host_read_floor`, the prototypes of all operations including the inline ones) and ends by including `src/server/group_view_impl.h` (208 lines), which is private and holds `struct host_waiter`, `struct group_view_slot`, `struct group_views`, `V_LOAD`/`V_STORE` and the bodies of the nine inline functions, so the hot path stays inline. The compiled code is unchanged (disassembly of `raft_host.o` and `group_view.o` identical before and after). Extraction merged in wave 81; the owner interface test, the privacy check (now also: no include of the impl header outside `group_view.h`, no slot-table access through a view outside the owner) and the splice tests follow the split. The older merge-state text is under History at the end.

Linked item: structure review ([structure-review-20261110.md](structure-review-20261110.md)), candidate D of the [lifecycle inventory](raft-host-lifecycle-inventory-20261111.md). Model: [put-window-ownership-20261110.md](put-window-ownership-20261110.md). The question and answer key for a later reader trial are in [raft-host-lifecycle-reader-trial-20261111.md](raft-host-lifecycle-reader-trial-20261111.md); this page does not measure a reading set.

## What moved, and what stayed

Moved to `group_view.c` (non-inline): `host_publish_view`, `host_waiter_sleep`, `host_waiters_wake`.

Moved to `group_view.h` (`static inline`, compiled into their callers as before): `host_view_get`, `host_view_covers`, `host_view_quorum_index`, `host_view_wait_apply`, `host_read_wait_ready`, `host_read_floor_ready`, the lookup `group_view_slot`, and the lock bracket `group_view_wait_lock` / `group_view_wait_unlock`. `struct host_view`, `struct host_waiter` and `struct host_read_floor` moved with them.

New operations (replace direct field use): `group_view_init`, `group_view_destroy` (start and stop of the host), `group_view_attach` (`attach_group`), `group_view_waiting` (`host_gc_metadata_waiting`).

Fields: the 19 private fields of the inventory (`v_seq`, `v_stamp`, the other `v_*`, `view_mu`, `cv_mu`, `applied_cv`, the waiter table, `waiter_hint`, `waiter_active`, `cv_overflow`) are now in `struct group_views` (one per host, embedded as `h->gv`) and `struct group_view_slot` (one per group). The slot carries its own group id, set in `attach_group`; `struct host_group` keeps its own.

Stayed in `raft_host.c`, with the reason:

| Function | Reason |
|---|---|
| `host_publish_group` | needs each slot's `hosted` flag and core pointer, which belong to `struct host_group`; it calls the owner's `host_publish_view` with the core passed in |
| `host_halted_hint` | reads view state only through `host_view_get`; needs `h->raft_id` |
| `host_local_leader`, `host_local_view` | only `host_view_get` plus `host_group_retired` (host state) |
| `host_is_leader` | only `host_view_get` |
| `host_leader_hint` | reads the core under `h->mu`, not the view |
| `server_raft_host_active`, `server_raft_host_formed`, `server_raft_host_unresponsive_voters`, `server_raft_host_group_commit` | read `h->running`, the membership copy or the core under `h->mu`; no view state |

These eight and `host_halted_hint` reach the owner only through `host_view_get`. The inventory counted them in candidate D by name; they are callers, not owners.

The helpers the owner needed from `raft_host.c` (`group_raft`, `group_slot`, `host_group_retired`, `membership_copy`) are not needed: the core is passed into `host_publish_view`, and the group lookup is the owner's own `group_view_slot`. That was the smaller change (no internal header).

## The contract in short

Full text: `group_view.h`. Lock order `h->mu`, then `cv_mu`, then `view_mu`. `view_mu` is a leaf. `host_view_get` returns a coherent copy of exactly one publish with no lock held; after 64 odd sequence loads it waits on `view_mu` (K107; the constant and the loop are unchanged). An unchanged publish leaves the sequence and the stamp alone; the stamp is stored last. No operation returns holding `view_mu`; `cv_mu` is held across a return only between `group_view_wait_lock` and `group_view_wait_unlock`.

**The wait bracket.** The three waits, `host_read_index_admitted`, `host_wait_applied_mode` and `host_publication_committed`, hold `cv_mu` across "re-read the view, test, sleep", and each has its own predicate and, at two of them, timing and logging done under the lock; site 1 also re-reads the view after the unlock. One absorbed wait operation would have to run that code after the unlock or through callbacks, which changes what the callers do or makes the owner harder to read than them. So the boundary is honest about it: the owner exposes the lock (`group_view_wait_lock`, `group_view_wait_unlock`, two `static inline` wrappers of `pthread_mutex_lock` and `unlock` on `cv_mu`), the callers keep their bodies exactly as they were, and `host_waiter_sleep` is documented as callable only inside the bracket. Between lock and unlock a caller may call `host_view_get`, test its own predicate, call `host_waiter_sleep`, read the clock and log; it must not take another lock, make an RPC, or return without the unlock. (Coordinator decision, Task V3 message.)

## Proof of no behaviour change

All on intel2 (8 cpus, gcc 14.3.1, `-O3` plus the build's flags), trees under `~/efs-taskV3/` (`before` = `git archive v3-base`, `after` = this branch), raw output in `~/efs-artifacts/taskV3-20261111/` (`SHA256SUMS`), not committed.

| Check | Result |
|---|---|
| `nm src/server/raft_host.o` for `host_publish_view`, `host_waiter_sleep`, `host_waiters_wake`, `group_view_*` | none defined there |
| `make -j8 all` | rc 0 |
| `./tests/test_group_view` and each of its 11 cases by name | rc 0; `--list` prints 11 names; an unknown case exits 2 |
| `test_forward_once`, `test_host_batch_handoff`, `test_host_view_spin`, `test_remote_cmd_halted_peer`, `test_readindex_publish`, `test_read_waiters`, `test_readindex_completion`, `test_readindex_result`, `test_reap_fairness`, `test_k96_status_slots_live` | all rc 0 |
| `scripts/check_group_view_privacy.py`, `check_concern_map.py` (map, `raft_host.c`, `group_view.c`, `group_view.h`) | rc 0 |
| `scripts/tests_for.py server/group_view --run` (2 commands), `scripts/tests_for.py server/raft_host --run` | rc 0 both |
| `make test`, `python3 tests/test_make_test_recipe.py` | rc 0 both |
| `tests/sanitizer_check.sh asan` | VERDICT PASS (76 run, 0 failed, 479 s); `PASS group_view_interface (0s)` |
| `tests/sanitizer_check.sh tsan` | VERDICT PASS (52 run, 0 failed, 663 s); `PASS group_view_interface (0s)`, `PASS k96_status_slots_live (17s)` (the K107 site); the known intermittent `k37_segment_rebuild` did not occur |

**Instruction counts, inlining off.** Method: the Makefile's own compile command plus `-fno-inline -fno-inline-small-functions -fno-inline-functions-called-once -fno-partial-inlining -fno-ipa-cp -fno-ipa-sra -fno-ipa-icf -fno-ipa-pure-const -fno-ipa-reference -fno-ipa-modref -fno-ipa-vrp -fno-ipa-bit-cp -fno-ipa-ra -fno-ipa-profile -fno-ipa-stack-alignment -fno-ipa-cp-clone -fno-optimize-sibling-calls`, instruction lines of `objdump -d --no-show-raw-insn`, alignment `nop` lines excluded: `scripts/evidence/v3/instruction_counts.sh`. Before: `raft_host.o` of `v3-base` (`scripts/evidence/v3/instructions_before.txt`). After: `group_view.o` and `raft_host.o`.

| Function | Before | After | Difference |
|---|---|---|---|
| `host_view_get` | 89 | 89 | identical |
| `host_view_covers` | 22 | 22 | identical |
| `host_view_quorum_index` | 33 | 33 | identical |
| `host_view_wait_apply` | 33 | 33 | identical |
| `host_read_wait_ready` | 61 | 61 | identical |
| `host_read_floor_ready` | 78 | 78 | identical |
| `host_waiter_sleep` | 93 | 93 | identical |
| `host_waiters_wake` | 171 | 170 | one register move fewer (the host pointer is now the view pointer) |
| `host_publish_view` | 229 | 232 | address arithmetic: a slot is 120 bytes (shifts and a subtract, 3 instructions) where a `host_group` slot was `0x532d0` bytes (one `imul`); one `movslq` of the index; one load of `g->r` fewer because the core is passed in. All other lines identical in kind |
| `host_read_index_admitted` (caller) | 1054 | 1053 | register allocation of the host pointer against the view pointer (the calls to `host_view_get` now take `&h->gv`) |
| `host_wait_applied_mode` (caller) | 247 | 248 | one `lea` for `&h->gv` |
| `host_publication_committed` (caller) | 408 | 408 | identical count |
| `host_halted_hint`, `host_local_leader`, `host_local_view`, `host_is_leader` (callers) | 18, 26, 29, 14 | 19, 27, 30, 15 | one `lea` for `&h->gv` each |
| `host_publish_group` (caller) | 27 | 32 | the `lea`, and the load of `h->g[i].r` it now hands over |
| `host_gc_metadata_waiting` | 55 | 43 | the slot scan is now `group_view_waiting` in the owner (its loop is in `group_view.c`) |

The caller counts are not all identical: the callers pass `&h->gv` where they passed `h`, which costs one `lea` where `gv` is not at offset 0, and the compiler's register choice moves a few `mov` lines. `gv` was not made the first member of `struct efs_raft_host` to hide this (that would move every other field's offset by about 50 KB). No instruction was added or removed in the lock, wait or publish sequences themselves.

## Before and after numbers

| | Before (`v3-base`) | After |
|---|---|---|
| Lines of `src/server/raft_host.c` | 26,712 | 26,314 |
| Lines of `group_view.c`, `group_view.h` | 0 | 233, 342 |
| D's private fields named outside the owner | 19 private fields, plus outside uses of the locks: `view_mu` 2, `cv_mu` 10, `applied_cv` 2, the waiter table 3 (inventory) | 0 (`check_group_view_privacy.py` over `src/` and `tests/`) |
| Outside uses of the owner's operations in `raft_host.c` | | `host_view_get` 37 (33 in the inventory's functions plus 4 in `host_halted_hint`, `host_local_leader`, `host_local_view`, `host_is_leader`), `host_publish_view` 6 (5 sites plus `host_publish_group`), `host_publish_group` 13 sites in 8 functions, `host_waiters_wake` 5, `host_waiter_sleep` 3, `group_view_wait_lock` 3, `group_view_wait_unlock` 3, `group_view_init`, `_destroy`, `_attach`, `_waiting` 1 each |

The inventory output of the D candidate before and after is committed as `scripts/evidence/v3/inventory_before_D.txt` and `scripts/evidence/v3/inventory_after_D.txt`. The script reads one file, so the after run is on `raft_host.c`, `group_view.h` and `group_view.c` concatenated: D's functions are then 17 in 322 lines with 0 fields used only inside (the 9 fields it still uses are host and group fields). Section f (tests) of that run is not meaningful on a concatenation.

## Tests, and what they do not cover

`tests/test_group_view.c` (fast suite, 5 s budget, both sanitizer lanes) links the real `group_view.c` over twelve stub Raft core getters it defines itself. Cases: `test_unchanged_publish_keeps_stamp`, `test_changed_publish_moves_stamp_and_shows_fields`, `test_absent_replica_clears_has_and_proof`, `test_unknown_group_reads_as_absent`, `test_reader_never_sees_a_mixed_publish` (a reader thread against a publisher, bounded by counts: 20,000 publishes, 200,000 reads), `test_predicates`, `test_floor_ready_modes_2_and_3`, `test_bracket_true_predicate_returns_at_once`, `test_publish_before_bracket_is_seen_no_sleep` (the lost-wakeup case, single-threaded), `test_passed_deadline_sleep_leaves_no_slot`, `test_wake_without_sleeper_changes_nothing`. No sleeps; the contract-to-case table is in `src/server/README.md`.

Not covered by the interface test, because each needs to know that another thread is blocked, which cannot be known without polling or a sleep: the wake of a blocked sleeper in modes 0 and 1 (modes 2 and 3 are covered by `tests/test_read_waiters.py`, which runs the owner's wake text over a mock), the signal after `cv_mu` is released (also in `test_read_waiters.py`), and the fall-back to `applied_cv` when all 512 slots are busy (`test_read_waiters.py` sets the table by hand). The K107 starvation case is `tests/test_host_view_spin.py`; the TSan entry `k96_status_slots_live` is the live K107 site.

Splice tests. Twenty Python tests cut product text out of `raft_host.c` and had to be ported (a first sweep named nine and missed eleven that `make test` then found). They still compile the product text, not a copy. `test_host_view_spin.py`, `test_readindex_publish.py`, `test_read_waiters.py` and `test_readindex_completion.py` cut `group_view.h` and `group_view.c` through two new helpers in `tests/csrc.py` (`group_view_header`, `group_view_func`) and so name the private fields; they are the exemption list of the privacy check. The other sixteen cut callers from `raft_host.c` and stub the owner with a small `struct group_views` of their own that names no private field: `test_readindex_result.py`, `test_reap_fairness.py`, `test_forward_once.py`, `test_host_batch_handoff.py`, `test_applied_interval.py`, `test_host_cover_route.py`, `test_lookup_read_group.py`, `test_raft_pending_reads.py`, `test_query_target_route.py`, `test_raft_read_admission.py`, `test_read_control_host.py`, `test_readindex_operation.py`, `test_readindex_pair.py`, `test_readindex_tickets.py`, `test_receive_persist.py`, `test_session_cleanup_cli.py`. `test_remote_cmd_halted_peer.py` needed no change; `test_pump_logging.py` only had a comment reworded.

## Limitations

- `EFS_HANDOFF_SOURCE_REF` of `tests/test_host_batch_handoff.py` (a negative control that compiles an older `raft_host.c`) no longer works for refs before this change: the stubs match the new call signatures.
- Instruction counts are not performance. No latency or throughput measurement was taken; the hot predicates and `host_view_get` are `static inline` in the header as before, but a build with a different compiler may inline differently. The live gates (cluster suites) were not run here.
- `group_view_slot` maps a group id to a slot by the id set in `attach_group`; `struct host_group` keeps a second copy of the id. The two are set in the same function and nowhere else.
- The owner's header defines the owner's structs, so a change to `struct group_views` rebuilds `raft_host.o` and the tests that include it.
- The privacy check exempts the four splice tests named above by path.

## Contract and implementation split (Task V6)

**Purpose:** the structure review found `group_view.h` too heavy for a caller (a trial reader read all 347 lines to use the owner). Verdict: PASS (see below for what was not run).

| File | Lines | Holds |
|---|---|---|
| `src/server/group_view.h` | 181 (was 347) | contract comment (lock order, operations, wait bracket, what a reader may assume), `GROUP_VIEW_NGROUPS`, `HOST_WAITERS`, `HOST_VIEW_TRIES`, `struct host_read_floor`, `struct host_view`, prototypes of all operations (the inline ones declared `static inline`, defined in the impl header), last line before the guard: `#include "group_view_impl.h"` |
| `src/server/group_view_impl.h` | 208 | private: `V_LOAD`/`V_STORE`, `struct host_waiter`, `struct group_view_slot`, `struct group_views`, bodies of `group_view_slot`, `host_view_covers`, `host_view_quorum_index`, `host_view_wait_apply`, `host_read_wait_ready`, `host_read_floor_ready`, `host_view_get`, `group_view_wait_lock`, `group_view_wait_unlock`; `#error` unless `group_view.h` is being included |

`group_view.h` is over the 150-line aim by 31 lines. What could not move: the contract comment is 108 lines and was kept as written (only the "marked private below" sentence now names the impl header, and one over-long line is re-wrapped); the `struct host_view` comment (K107 rationale, 8 lines) stays with the type a caller declares; the eight inline prototypes are 24 lines. Nothing private is left in it.

**No behaviour change.** Text moved between two headers; no function body, struct layout, constant, memory order or lock changed. Proof on intel1 (gcc `-O3`, the Makefile flags), `objdump -d --no-show-raw-insn` with addresses removed (`sed 's/^ *[0-9a-f]*:\t//'`) of `src/server/raft_host.o` (112,224 lines) and `src/server/group_view.o` (948 lines), built from the tree at `0bffb131d` and from this tree: `cmp` reports both identical.

**Checker** (`scripts/check_group_view_privacy.py`): the owner set gains `group_view_impl.h`; new rule 1: no file other than `group_view.h` (and `tests/csrc.py`, which cuts the include line out of the text) includes `group_view_impl.h`, the splice tests included; new rule 2: no file other than the owner writes `gv.slot` or `gv->slot` (the earlier review found `h->gv.slot[gi].group = x` passed). `--self-test` feeds three bad snippets (a private field, a direct include of the impl header, a slot access) and one good one and fails unless exactly the three are flagged; `make test` runs it next to the check.

**Tests.** `tests/csrc.py` now concatenates `group_view.h`, `group_view_impl.h`, `group_view.c`; `group_view_header()` returns the .h text from the first `#define` up to the impl include, then the impl text from `V_LOAD`; `tests/test_read_waiters.py` and `tests/test_readindex_completion.py` cut the inline bodies from the impl text. They still compile the real product text.

**Reproduce:** copy the tree to a Linux host, `make test-bins tests/faults/efsd`, then the commands of the task brief (step 7): `./tests/test_group_view`, the privacy check and its `--self-test`, every `tests/*.py` that names `group_view`, `python3 scripts/tests_for.py server/group_view --run`, `server/raft_host --run`, `make test`, `tests/sanitizer_check.sh asan` and `tsan`. Disassembly: build `src/server/raft_host.o src/server/group_view.o` at both trees and compare as above. Raw output on intel1 in `~/efs-artifacts/taskV6-20261111/` (removed from the run tree; hashes in `SHA256SUMS` there).

**Results (intel1, Nov 11, tree = `0bffb131d` plus this change; tree copied with a piped macOS tar, whose `._*` files broke one `make test` run until deleted; the later runs are clean):** `make`, `./tests/test_group_view`, the privacy check and `--self-test`, all 20 Python tests that name `group_view`, `tests_for server/group_view --run`, `tests_for server/raft_host --run`, `make test` and `tests/test_make_test_recipe.py` return 0; `tests/sanitizer_check.sh asan` and `tsan` return 0 (`k37_segment_rebuild` and `k96_status_slots_live` PASS in both). Two fixes the split needed outside the headers: `scripts/env_inventory.py` skips an `#ifndef EFS_*_H` followed by `#error` (the impl header's refusal) instead of counting it as a build mode, and `tests/budgets.tsv` has a row for the self-test. Limits: the Mac `cc` (clang) rejects the unused inline bodies in the splice tests, as before; the tests were run with gcc on intel1 only. The graph shows `group_view.h` as the component's contract; `group_view_impl.h` has no node.

## Reproduce

```
git archive <commit> | ssh intel2 'mkdir -p ~/efs-taskV3/after && tar -xm -C ~/efs-taskV3/after'
cd ~/efs-taskV3/after && make -j8 all && make -j8 test-bins tests/faults/efsd
./tests/test_group_view; python3 scripts/check_group_view_privacy.py
python3 scripts/tests_for.py server/group_view --run; python3 scripts/tests_for.py server/raft_host --run
scripts/evidence/v3/instruction_counts.sh . src/server/group_view.c out.txt host_publish_view host_view_get host_waiter_sleep host_waiters_wake
```

Counts of the before tree: the same script on `git archive v3-base` with `src/server/raft_host.c` and the function list of `scripts/evidence/v3/instructions_before.txt`. Clean up: `edelete --force --delete ~/efs-taskV3`.

## Results and how the run went

The first full run found eleven splice tests the first sweep had missed (`make test` stopped at `test_applied_interval.py`; the ASan lane failed `readindex_pair` and `applied_interval` for the same reason). They were ported (see "Splice tests"), and the commands that depend on them, `scripts/tests_for.py server/raft_host --run`, `make test` and both sanitizer lanes, were run again from a clean rebuild of the committed tree; the table above is that second run. The first run's logs are kept next to it (`asan_stale_run1.log`, `rc_run1.txt`). Product code did not change between the two runs; only tests. Raw output: `~/efs-artifacts/taskV3-20261111/` on intel2 (27 files, `SHA256SUMS`); `~/efs-taskV3` was removed after the run. Not run: the live cluster gates (NOTRUN), and a latency or throughput comparison.

## History

**Merge state (Nov 11):** merged into devel in wave 81 after an independent review that compared every moved body with the old text and found no behaviour change. The merge added one test case (a blocked sleeper is counted by `group_view_waiting`, which the reaper's yield test uses, and is woken by a publish; the port of `tests/test_reap_fairness.py` had left that unpinned) and corrected five contract sentences in `group_view.h` (a NULL core, the leader field of an absent group, the bracket and the logger's own try-lock, when the stamp moves last, and that the 64 loads are per call). Known gaps: the K107 bound is pinned only by `tests/test_host_view_spin.py`, the race case can pass without a race, and the privacy check does not guard the slot's group id. The reader trial after the extraction was run ([reader trials](reader-trials-20261111.md), trial 4): the reader opened 1,391 lines against 1,167 before and needed 1,028 against 907, and gave the more complete answer; the contract helped with the lock rules, but the question also needs the publishers and the read loop in `raft_host.c`.

*The paragraph above is the current state. Everything below is the task's report as it was written before the merge: where it says "unmerged", "not merged" or names a next action, the paragraph above replaces it.*

**Current state (Nov 11):** the published group view (the per-group copy of commit, applied, term, role, leader and read proof that handlers read without `h->mu`, and the sleeper table that waits for it to change) is out of `src/server/raft_host.c` and in `src/server/group_view.c` with its contract and inline predicates in `src/server/group_view.h`, an owner interface test `tests/test_group_view.c` and a privacy check that fails when another file names the owner's fields. This is a refactor: no function body changed except for how it reaches the state. It is on branch `claude/task-v3-group-view-ceb2d8` (base `v3-base`, `1d93d6965`), not merged. `raft_host.c` went from 26,712 to 26,314 lines. The boundary exposes a lock bracket instead of one wait operation, because the three waits have three different predicates. Verdict: PASS on intel2 for the build, the owner test, the 20 ported tests, `make test`, the catalogue selectors and both sanitizer lanes; NOTRUN for the live cluster gates and for any latency measurement.
