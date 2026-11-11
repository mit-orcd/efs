# Lifecycle inventory of `src/server/raft_host.c` for one substantial extraction (Task V2)

**Merge state (Nov 11):** merged into devel in wave 80. Since then: candidate D, the published group view, was extracted in wave 81 ([group view ownership](group-view-ownership-20261111.md)), and the reader trial on the committed question was run before and after that extraction ([reader trials](reader-trials-20261111.md)): the reading was not reduced, because the question also follows the read loop that is still in `raft_host.c`. The next server boundary is therefore candidate A, the read-proof lifecycle. The line counts below are from devel `68e2778a4`, before the K113 and K109 fixes and before the extraction.

*The paragraph above is the current state. Everything below is the task's report as it was written before the merge: where it says "unmerged", "not merged" or names a next action, the paragraph above replaces it.*

**Verdict: PASS, inventory recorded; no extraction chosen** (Nov 11, 2026). Purpose: the numbers for the coordinator's choice of ONE lifecycle extraction from `raft_host.c`, from the structure review's four candidates (read proof, apply and persistence, publication, published group view). Read-and-measure only: no file under `src/`, `include/` or `tests/` changed, nothing built, no host used. No K row; blocks alpha: no. Status Nov 11: remaining failures none; active run none; next action: the coordinator chooses a candidate; the reader trial in [its own page](raft-host-lifecycle-reader-trial-20261111.md) is written and not run.

- Tested tree: `devel` at `68e2778a4`; `src/server/raft_host.c` is 26,348 lines. The script is [scripts/evidence/v2/lifecycle_inventory.py](../../../scripts/evidence/v2/lifecycle_inventory.py) with the four function lists beside it: `scripts/evidence/v2/candidate_A_readindex.txt`, `scripts/evidence/v2/candidate_B_apply_persist.txt`, `scripts/evidence/v2/candidate_C_publication.txt` and `scripts/evidence/v2/candidate_D_view.txt` (one function name per line). Line numbers on this page are a snapshot of that commit.
- Environment: macOS checkout, Python 3 standard library only. The script reads `src/`, `include/` and `tests/` and writes nothing. It runs in about 15 s.
- Not determined: (1) whether the apply handlers (35 `apply_*_cmd` functions, 1,311 lines) belong to candidate B; B is measured without them and the page says what that changes; (2) the cost of a call that stops being inlined: the file is built with plain `static` calls and this page did not build or measure anything; (3) how long a reader needs today for the trial question; (4) the bytes of any code size or instruction change. What would decide (2): the `scripts/evidence/dl/instruction_stream.sh` method used for the PUT-window pilot, on the candidate chosen, with inlining off.

## Method

```bash
python3 scripts/evidence/v2/lifecycle_inventory.py src/server/raft_host.c                      # all numbers in the table
python3 scripts/evidence/v2/lifecycle_inventory.py src/server/raft_host.c --sites --field-sites --functions   # every outside use and field reference, per function
```

The script exits 1 when a listed name is not defined exactly once with a body, or a list has a duplicate. How it counts:

- **Functions and lines.** A definition is a column-0 line the concern-map checker (`scripts/check_concern_map.py`) also matches, whose first `{` comes before any `;`. Its last line is the next column-0 `}`. The file has 641 definitions, the same 641 names as the map.
- **Fields.** The field names of `struct efs_raft_host` (169) and `struct host_group` (67). A reference is `->name` or `.name` in code (comments and strings blanked). 54 of the 236 names also occur in another struct of `src/` or `include/` (for example `mu`, `s`, `g`, `r`, `n`, `kv`, `group`, `running`). For these the script counts a reference only when the base is a known `struct efs_raft_host *` or `struct host_group *` variable, `h->g[..]`, or `group_slot(..)`. Other bases are dropped: 480 of 2,180 references to ambiguous names, 477 of them because the base is unknown (for example `x->y->mu`). The table also shows the raw by-name share.
- **Hub fields.** A field with 100 or more references in the file (`mu`, `kv`, `g`, `r`, `s`, `group`, ...) is shared infrastructure. One column drops them.
- **File-scope variables.** Column-0 `static` or `__thread` variables, counted the same way (51 in the file).
- **Locks and atomics.** `pthread_mutex_lock`/`trylock`, `pthread_rwlock_*`, `pthread_cond_wait`/`timedwait` and `__atomic_*` calls inside the candidate's functions. A lock is named by its last member (`mu` joins `h->mu`, `c->mu` and `w->mu`).
- **Callers.** Every use of a candidate function's name outside the candidate: a call, or a mention as a value (callback registration), in the body of another function or at file scope (macros). Declarations are skipped. "Entry points" are non-static candidate functions and how many other `src/` files name them.
- **Callees.** `static` functions of the file outside the candidate that the candidate calls.
- **Tests.** Files under `tests/` that contain the function name as a word, and of those the files that also mention `raft_host.c` (they read the source as text or are catalogue rows; they break when a function moves).

## Candidates

| | Concerns of the [map](../../how-it-works/raft-host-map.md) | List |
|---|---|---|
| A. Read proof | "ReadIndex round" (21 functions) + `host_read_probe` (Outbox), `host_pending_reads`, `host_note_queued_reads`, `host_read_control` (Pump and inbox) | 25 |
| B. Apply and persistence | "Apply: core" (8), the applied-checkpoint part of "Pump, inbox and applied checkpoint" (9), "KV and checkpoint workers" (10), the apply-result ring and durable-sync functions of "Proposal, admission and verdict wait" (8) | 35 |
| C. Publication | "Publication and REPORT" (33) + `host_report_primary_pick`, `host_fwd_report`, `host_report_session_worker`, `host_report_session_admit` | 36 |
| D. Group view | "Group view, waiters and leader state" without `group_raft` and `group_slot` | 17 |

Choices that change the numbers: B leaves out the 35 `apply_*_cmd` handlers (1,311 lines), `apply_one_publish` and `host_dir_migrate_local` (they are the state machine; including them takes B to about 2,250 lines, makes it the second largest and removes 36 of its 53 outside callees); `group_raft` (33 call sites) and `group_slot` (18) are lookup helpers and are counted as callees of D, not members. `host_publication_committed` is in C because the map puts it there, but it is a ReadIndex consumer: it takes `h->mu` and `cv_mu` (L25366-L25410), calls `host_read_begin_tracked`, `host_view_get` and `host_waiter_sleep`.

## Table

| | A read proof | B apply and persist | C publication | D group view |
|---|---|---|---|---|
| functions | 25 | 35 | 36 | 17 |
| lines | 735 | 930 | 2,038 | 323 |
| share of the file | 2.8% | 3.5% | 7.7% | 1.2% |
| struct fields used | 20 | 35 | 16 | 31 |
| fields used only inside (private already) | 2 | 5 | 0 | 19 |
| file-scope variables used / only inside | 2 / 1 | 1 / 0 | 11 / 9 | 1 / 0 |
| share of refs to its fields that are inside | 6% | 12% | 6% | 13% |
| same, raw name match | 6% | 14% | 7% | 11% |
| same, without hub fields | 22% | 34% | 13% | 37% |
| distinct outside callers in the file | 73 | 16 | 3 | 28 |
| functions with an outside caller | 13 of 25 | 24 of 35 | 3 of 36 | 13 of 17 |
| entry points named in other `src/` files | 0 | 0 | 4 | 4 |
| outside static callees | 24 | 53 (36 are apply handlers) | 58 | 5 |
| lock calls / atomic calls | 12 / 13 | 10 / 16 | 6 / 28 | 7 / 10 |
| test files naming it | 55 | 28 | 17 | 29 |
| of those, read `raft_host.c` as text | 3 | 4 | 5 | 5 |
| functions no test names | 1 | 4 | 11 | 4 |
| hot functions (next section) | 13 | 11 | the REPORT path, all per request | 11 |

The private fields of D are `v_seq`, `v_stamp`, 14 other `v_*` fields, `cv_overflow`, `waiter_active` and `waiter_hint`. B's five are `applied_saved_us` and four `incompat_*` fields. C's nine file-scope variables are `g_report_receipts`, `_once`, `_ok`, `g_report_receipt_join_ms`, `g_report_receipt_trace`, `host_report_note`, `host_resolve_why`, `host_pub_unit_active`, `host_report_session_parallel`. A's one is `g_ri_coal`; its fields `ri_begun` and `ri_request`.

## What the numbers say

- **Most private state for its size: D.** 19 fields no other function touches, in 323 lines (5.9 per 100 lines). The next are B with 5 fields in 930 lines (0.5) and C with 9 file-scope variables and no field in 2,038 lines (0.4). Without the hub fields D has 27 fields used and 19 private; A 14 and 2; B 30 and 5; C 12 and 0.
- **Narrowest interface: C inbound, D outbound.** C has 3 outside callers in the file (`host_command_cohort_flush`, `host_txn_commit`, `server_raft_host_getchunks`) and 4 public entry points that `handler.c` calls (`server_internal.h` L590-L609); but it calls 58 outside statics. D calls 5 outside statics and has 28 outside callers in the file plus 4 public entry points called from five other files. A has the widest inbound interface: `host_read_index` alone is called 88 times from 56 functions. B's inbound is 16 callers, 12 of its 24 called functions are called only from `host_pump`; its outbound is 53, of which 36 are the apply handlers reached through `host_apply_dispatch`.
- **Ranking rule, stated so it can be argued with.** Rank the four on three numbers, equal weight: private state (fields plus variables used only inside), inbound width (distinct outside callers plus entry points), outbound width (outside callees). Private: D 19, C 9, B 5, A 3. Inbound: C 7, B 16, D 32, A 73. Outbound: D 5, A 24, B 53, C 58. Sum of ranks: D 5, C 7, B 8, A 10. D wins on private state and outbound; C wins only on inbound, and is the only candidate that moves more than 3.5% of the file. Another weighting changes the order of C and B.
- **A and D are one knot.** `host_waiters_wake` (D) calls `host_read_floor_ready` (A), and A calls nine of D's functions. A D owner has to take `host_read_wait_ready` and `host_read_floor_ready` (32 lines, pure over `struct host_view`) with it, or the wake takes a predicate callback.
- **Tests do not discriminate.** A has the most test files (55) and C the most untested functions (11 of 36); the five files that read `raft_host.c` as text are the real cost of a move for D (see below).

## Hot path

Hot means the function runs once per client request or once per applied entry. Decided from the callers the script lists.

| Candidate | Hot functions | Caller that makes them hot |
|---|---|---|
| A | `host_read_index` (88 calls from 56 handlers: getattr, lookup, create, rename, ...), `host_read_index_admitted`, `host_read_admit_view`, `host_read_admit`, `host_read_request`, `host_read_index_pair`, `host_wait_applied_mode`, `host_wait_applied_own` (verdict waits of write requests: `host_wait_verdict`, `host_wait_settled`, `host_wait_refs`), `host_remote_read_index`, `host_follower_read_result`, `host_wait_applied` (follower reads), `host_read_wait_ready`, `host_read_floor_ready` (each loop pass). 13 in all | every metadata read and write RPC. Not hot: `host_read_probe` (per probe sent), `host_pending_reads`, `host_read_start_requested`, `host_note_queued_reads`, `host_read_control` (per pump cycle or inbox batch) |
| B | `host_apply`, `host_apply_dispatch`, `host_kv_cursor` (L3704), `host_apply_coord` (per APPEND reserve entry) | the Raft core's apply callback: `src/raft/raft.c` L628 `r->apply(...)`, registered at `attach_replica` L9221. Also per request: `host_arc_read`, `host_arc_verdict`, `host_apply_rc`, `host_arc_apply_us` (verdict waits), `host_note_durable_synced`, `host_mark_durable_covered` (`host_propose_impl`), `host_group_incompatible` (read admission and propose): 11 in all. Not hot: `host_capture_cursor`, `host_publish_cursor`, `host_persist_durable_tail`, the checkpoint and KV worker dispatch (per pump cycle), `host_receive_persist` (per inbox batch) |
| C | none per applied entry. Per REPORT request: `server_raft_host_report_receipted`, `host_report_exec`, `server_raft_host_report`, `host_pub_*`, `host_report_release`; the exact count of the functions reachable from `server_raft_host_report` was not computed | one REPORT RPC per published file (`handler.c` L1312-L1321); `host_report_release` also per cohort flush |
| D | `host_view_get` (33 calls: 13 in `server_raft_host_submit`, 7 in the read admission, 3 each in `host_wait_applied_mode` and `host_publication_committed`), `host_view_covers`, `host_view_quorum_index`, `host_view_wait_apply`, `host_publish_group` (13 calls; per propose and per read round) and through it `host_publish_view`, `host_waiter_sleep`, `host_waiters_wake` (per blocking read and per pump cycle), `host_local_leader`, `host_halted_hint`; `server_raft_host_active` (`handler.c` L763 and 11 more, every RPC): 11 in all | every metadata RPC. `host_publish_view` also runs once per pump cycle |

Moving a `static` function to another file removes inlining and adds a call through the symbol. D and A are the most exposed: the small predicates (`host_view_covers` 7 lines, `host_view_quorum_index` 15, `host_view_wait_apply` 9) are called in the read loop. They can stay `static inline` in the owner's header. B's hot function `host_apply` is already reached through a function pointer, so a move costs nothing there. The cost was not measured (see Not determined).

## Boundary drafts

Drafts for the two top candidates, on this page only. No header exists.

### D. Group view owner (`group_view.c`, `group_view.h`)

**Private state (moves into one `struct group_view` per group and the owner's own fields).** The 19 fields only D uses, and with a rewrite of the outside references below: `v_has`, `view_mu`, `cv_mu`, `applied_cv`, `waiters[]`. The owner allocates the per-group view, so `struct host_group` keeps one pointer. `struct host_view`, `struct host_waiter` and `struct host_read_floor` move to the header.

**Public interface.**

| Function | The caller hands over | The caller gets back |
|---|---|---|
| `view_init(h)`, `view_destroy(h)` | the host before the threads start, or after they stop | the locks, condition variables and slots made or destroyed (replaces direct use at L13987, L14018-L14023, L14288-L14301) |
| `view_attach(h, gi)` | a group slot after `attach_group` | the view marked absent (replaces the write at L9273) |
| `host_publish_view(h, gi)`, `host_publish_group(h, group)` | the host with `h->mu` held | nothing; the owner reads the core and stores the new view only if a field changed |
| `host_view_get(h, group, &v)` | a buffer | a coherent copy; no ownership, no lock held on return |
| `host_view_covers`, `host_view_quorum_index`, `host_view_wait_apply`, `host_read_wait_ready`, `host_read_floor_ready` | a copy and the request's want, round, term | a verdict; pure; the last takes the caller's `struct host_read_floor`, which it updates |
| `host_waiter_sleep(...)` or one `view_wait(...)` | the request, the deadline and the floor | the caller's floor updated after the sleep. Today the caller holds `cv_mu` across "re-read view, test, sleep" (L5657-L5679, L5859-L5872, L25400-L25405); that is the one lock the owner must absorb |
| `host_waiters_wake(h)` | the host with `h->mu` held (pump, 5 sites) | slots signalled after the owner releases `cv_mu` |
| `host_local_leader`, `host_local_view`, `host_is_leader`, `host_leader_hint`, `host_halted_hint` | host and group | a hint or a copy |
| `view_waiters_busy(h)` (new) | the host | whether a waiter slot is active; replaces `host_gc_metadata_waiting` reading `cv_mu` and `waiters` (L10890-L10893) |

Ownership transfers: none moves heap memory. The floor is caller memory, copied into the slot at sleep (L4611) and copied back at wake (L4619). That is the part a test must pin.

**Lock rules.** `h->mu` then `view_mu` in the publisher (L4344, L4397). `host_waiters_wake` takes `cv_mu` and calls `host_view_get` under it (L4642, L4663), which can take `view_mu` after 64 odd loads (L4456), so `cv_mu` then `view_mu` also occurs; with the pump calling the wake under `h->mu` the order is `h->mu`, `cv_mu`, `view_mu`. The [map's lock order](../../how-it-works/raft-host-map.md) lists `h->mu` then `cv_mu` and `h->mu` then `view_mu` but not the `cv_mu` then `view_mu` pair. No other lock may be taken while `view_mu` is held. Callers hold neither `view_mu` nor `cv_mu` after the change.

**Outside references that must change** (function and line; the full call-site list is `--sites`):

- Fields: `attach_group` L9273 (`v_has`); `server_raft_host_start` L13987 (`view_mu`), L14018-L14019 (`cv_mu`, `applied_cv`), L14023 (`waiters`); `server_raft_host_stop` L14288 (`view_mu`), L14295 (`applied_cv`), L14299 (`waiters`), L14301 (`cv_mu`); `host_gc_metadata_waiting` L10890-L10893; `host_read_index_admitted` L5657, L5679; `host_wait_applied_mode` L5859, L5872; `host_publication_committed` L25400, L25405.
- Calls that keep their text but become extern: `host_publish_view` from `host_read_start_requested` L5488, `host_receive_persist` L7850, `host_inbox_batch_handoff` L7900, `host_pump` L9005, `attach_replica` L9252; `host_publish_group` 13 sites in 8 functions; `host_view_get` 33 sites in 11 functions; `host_waiters_wake` L7851, L7901, L8785, L9006, L14359; `host_local_leader` L4750, L5005, L14973, L15140, L20806, L20821; `host_halted_hint` L5513, L5529, L6094, L15569.
- Moved predicates: `host_read_wait_ready` and `host_read_floor_ready` (callers `host_read_index_admitted` L5595 and `host_waiters_wake` L4666).
- D's own five outside callees: `group_raft` (3), `group_slot` (2), `host_group_retired` (2), `host_read_floor_ready` (1), `membership_copy` (1): the first two and the last need a shared internal header or a pointer passed at `view_init`.

**Tests an extraction needs.** Owner interface test `tests/test_group_view.c`: an unchanged publish leaves `v_seq` and `v_stamp` alone; a changed publish bumps the stamp last; an absent replica clears `has` and the proof fields; readers racing a publisher never see a mixed round, term and completion; wake predicates for `any` 0, 1, 2 and 3; the 512-slot overflow falls back to `applied_cv`; registration after a missed publish does not sleep (lost wakeup). Caller tests in `raft_host.c`: the five files that read `raft_host.c` as text and name D functions (`test_forward_once.py`, `test_host_batch_handoff.py`, `test_host_view_spin.py`, `test_remote_cmd_halted_peer.py`, the catalogue `budgets.tsv`) must read the new file, and `test_read_waiters.py`, `test_readindex_publish.py`, `test_readindex_result.py` (the map's tests for the concern) must still reach the owner. Whether those three read source text: not determined here; the script's text test lists only five.

### C. Publication owner (`report.c`, `report.h`)

**Private state.** The nine file-scope variables listed under the table; `obs_pub_n`, `obs_pub_ring`, `obs_pub_max_us`, `obs_pub_units`, `obs_pub_sets`, `obs_pub_set_splits`, `obs_pub_unit_aborts` (fields of `struct efs_raft_host`, written in C; read in `host_obs_render` L8279-L8305 and `host_periodic_capture` L8367-L8387). The receipt table locks itself (`src/common/request_receipt.c` L167-L278) and runs `host_report_exec` with its mutex released (unlock at L261, call at L264). The other 9 fields C uses (`kv`, `mu`, `disk`, `running`, `cv_mu`, `s`, `n`, `obs_arc_*`) are shared and stay.

**Public interface.**

| Function | The caller hands over | The caller gets back |
|---|---|---|
| `server_raft_host_report`, `_report_receipted`, `_report_receipt`, `server_raft_host_publication` | unchanged: the request arrays and the reply buffer (caller memory) | the reply filled; the receipt table answers a retry from the recorded outcome. Already public in `server_internal.h` L590-L609; `handler.c` L1312, L1321, L1358 stay as they are |
| `host_report_release(h, held)` | the host and the `held` count, from `host_command_cohort_flush` L18818 | an rc (`sync_rc`); also used inside C at L25104 |
| `host_pub_fault(h, stage)` | the host and a stage name, from `host_txn_commit` L19184 | an int; also used inside C at L24576 |
| `host_holds_chunks(mode)` | a row mode, from `server_raft_host_getchunks` L25941 | an int used as a yes or no; also used inside C at L23726 |
| `report_stats(h, out)` (new) | a buffer | the `obs_pub_*` counters, for the two observation functions |

Ownership transfers: the reply and the request arrays stay with the handler; `struct host_pub_batch` and `host_pub_unit` are per call on the stack; the receipt slot is owned by the table.

**Lock rules.** A REPORT arrives with no host lock. C takes `h->mu` in `host_report_release` (L24180) and, for the publication query, in `host_publication_committed` (L25366, L25389, L25410, with `cv_mu` at L25400); takes a trylock on `host_report_session_parallel` in `host_report_session_admit`; the proposal calls it makes take `h->mu` inside `host_propose_impl`. Not determined: the lock state when `host_pub_fault` runs inside `host_txn_commit` (the thread-local `host_pub_unit_active` is its guard); a read of `host_txn_commit` would decide.

**Outside references that must change.** The three calls above; the two observation reads above; and the 58 outside statics C calls, which the owner would need through a shared internal header: `now_us_` 12, `wr64be` 11, `wr32be` 10, `host_propose_wait` 8, `host_read_index` 7, `raft_dbg_on` 6, `set_inode_rc` 5, `host_fin_add` 4, `now_ns` 4, `wprof_now` 4 and 48 others with 3 calls or fewer (full list: section `e.` of the script output). That header is the cost of this candidate.

**Tests an extraction needs.** Owner interface test `tests/test_report_owner.c`: receipt replay, join and refusal; the stale verdict (`host_report_stale_verdict`); `host_resolve_caught_up` outcomes; table allocation failure runs without receipts. Caller tests: the five source-reading files (`tests/budgets.tsv`, `test_apply_ring.py`, `test_k32_entry_count.py`, `test_report_zero_eof.py`, `test_w155_report_resolve.py`) must read the new file. 11 of C's 36 functions are named by no test (`--functions` output, line `no test names`).

## Reader trial

The question and the answer key are in [the trial page](raft-host-lifecycle-reader-trial-20261111.md), committed before this page and before any extraction. The question, for D: a group's replica loses leadership while one handler is inside `host_read_index_admitted` with an old-term view and another sleeps in a waiter slot; which code publishes the change, what does each handler observe and return, what wakes the sleeper, and which locks are held in which order. The key has eight facts with file and line. Today a reader must understand 510 lines in eight places (1.9% of the file, range table on the trial page). Not run.

## Limitations

- Field matching is by name. 2,180 of 3,102 references use one of 54 ambiguous names; 480 of them are dropped by the base check (477 because the base is not a known variable or call). A dropped reference to a real field makes the share inside look larger or smaller by at most that many references in the whole file; per candidate the effect is the gap between the refined and the raw row (at most 2 points).
- Macros and header-only helpers are not expanded: `apply_ring.h`, `apply_wait.h`, `read_index_pair_remote.h` and `publication_session_fast.h` touch `arc`, `waiters` and the read state of the file but are not scanned for field use; their function names are not in the lists.
- `h->mu`, `c->mu` and `w->mu` count as `mu` in the lock column.
- A test counts when the name appears as a word in the file; a comment is enough. "Reads `raft_host.c` as text" means the file mentions `raft_host.c` and names a function; it does not prove the test slices the source.
- Function sizes are lines between the definition and its closing brace, comments included.
- Static analysis only. Nothing was built, run or timed.
