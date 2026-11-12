# Read-proof ownership: design (Task V4, phase 1)

**Current state (Nov 11):** the read-proof owner is designed and accepted with the coordinator's decisions (section "Coordinator decisions"); stage A and A2 are done in the code: the direct mode (`h->s == NULL`) and every gate that existed only for it are deleted from `raft_host.c` and from the group-view owner, and its tests are ported to the server-mode path (commits listed under "Stage A2"); the extraction itself (stage B) has not started and waits for the coordinator. Verdict: PASS for stage A and A2 on host intel2 (results below); no K row; blocks alpha: no. Base: devel `88bc1189a`.

Linked item: structure review ([structure-review-20261110.md](structure-review-20261110.md)), candidate A of the [lifecycle inventory](raft-host-lifecycle-inventory-20261111.md); the finished model is [group-view-ownership-20261111.md](group-view-ownership-20261111.md). The older [entry map](readindex-entry-map-20261108.md) is the function-level table this page builds on.

## Before-numbers

`python3 scripts/evidence/v4/lifecycle_inventory.py src/server/raft_host.c` (a copy of the V2 script with its own candidate list, because the V3 extraction moved two of the 25 listed functions, `host_read_wait_ready` and `host_read_floor_ready`, to `group_view.h`; the V2 script exits 1 on the old list). Output committed as `scripts/evidence/v4/inventory_A_before.txt`; the 23 names are in `scripts/evidence/v4/candidate_A_readindex.txt`.

| | V2 page (68e2778a4) | Today (0bffb131d) |
|---|---|---|
| functions | 25 | 23 |
| lines of function bodies | about 735 | 703 |
| distinct outside callers | 73 | 72 (`host_read_index`: 87 calls from 55 functions) |
| outside `static` callees | 24 | 17 (the group-view helpers left; `now_us_` 13, `group_raft` 10, `read_prof_add` 8, `host_publish_group` 6, `host_pump_kick` 5, `outbox_pure_read_control` 4, `group_slot` 3, then 10 with 2 calls or fewer) |
| fields used / only inside | 14 / 2 | 20 / 2 (`ri_begun`, `ri_request`); file-scope variables 2 / 1 (`g_ri_coal`) |
| test files naming a function | 55 | 54 (51 splice `raft_host` through `tests/csrc.py`) |

## The lifecycle as a state table

A request is a call of `host_read_index_admitted` (L5196) with `admission == NULL`, or with an admission from the pair path. `delegated` means `h->s != NULL`. In production it always is: `h->s` is assigned only at L13703, from the only caller `efsd.c:1539`, which passes `&server`. The `!delegated` branches (L5274-L5279, L5323-L5350, L5447-L5500, and `host_read_admit` L5074-L5093, `host_read_probe_needed` L5099-L5114) serve only a host built without a server; see finding 7. Cells are written for `delegated`. "Return" means what the handler gets.

| # | State | Entered by | Event to the next state (function, line) | Role/term change published | Read context changed | Deadline | Replica gone (`has == 0`) | Host stop |
|---|---|---|---|---|---|---|---|---|
| S0 | not admitted | `host_read_index` L5385 | gate: `host_group_incompatible` L5213, then view copy `host_view_get` L5221 → S1 or S2 | incompatible: return NOT_PRIMARY with `host_halted_hint` L5213-L5216 | n/a | `end` is computed here, 80 × 5 ms = 400 ms (L5220, L97, L60) | `!v.has`: return NOT_PRIMARY L5222 | no check; see S6 |
| S1 | follower | `v.role != LEADER` L5237 | `proof_only`: return NOT_PRIMARY L5238; `v.leader == self`: NOT_PRIMARY L5244; else `host_remote_read_index` L5246 → S9 | n/a (already follower) | n/a | see S9 | n/a | see S9 |
| S2 | leader, not yet admitted | `round == 0` L5267 | `host_read_admit_view` L5273 (L5143-L5156): admission = `{v->commit, ri_begun+1, v->term}` → S3 | role not leader: NOT_PRIMARY L5148, then view re-read and loop L5280 → S1 | n/a | none | `!g \|\| !v->has`: NOT_PRIMARY L5148 | none |
| S3 | admitted with a view copy | L5282 | `host_read_request` L5289 (L5160-L5171): CAS-raise `ri_request`, kick the pump, once (`requested`) → S4 | next loop pass: role not leader goes to S1 (L5237 comes before everything below) | n/a | none | next loop pass: `!v.has` NOT_PRIMARY L5233 | none |
| S4 | round requested | L5291 | pump `host_read_start_requested` L5176 (from `host_pump` L8595, L8640, L8708; `host_read_control` L14080) begins the round via `host_read_begin_tracked` L5132 and publishes → S5 | pump skips a group not led (L5184) and leaves `ri_request` raised | n/a | none | pump skips `!g->hosted \|\| !r` (L5184) | pump stops starting rounds when the loop ends |
| S5 | checking the published view | top of each pass | term: `v.term != admitted_term` NOT_PRIMARY L5292; floor: `host_read_floor_ready` L5296 freezes `floor.context` and the first sufficient `floor.index`; context changed: NOT_PRIMARY L5297; `floor.index` and (`proof_only` or `applied >= floor.index`): return OK with `floor.index` L5298-L5303 (this is also "proof seen, applied wait" when applied is behind); `proof_only` quorum: OK L5305-L5312; `host_view_covers`: OK with `proven_index` L5313-L5321 → S8, otherwise → S6 | role change: S1 (L5237, before the term check); same role and new term: NOT_PRIMARY L5292 | `floor.context && v.read_context != floor.context`: NOT_PRIMARY L5297 | `host_past_deadline` L5351: return BUSY | `!v.has`: NOT_PRIMARY L5233 | none |
| S6 | waiting in a sleeper slot | bracket L5358 | `host_waiter_sleep` mode 3 (delegated) L5367, only when `host_read_floor_ready` is false under the lock L5363; ends at the pump's wake or at `end`; unlock, `host_view_get`, loop L5380-L5381 → S5 | wake predicate returns "ready" when `!has`, role not leader, or term differs (`group_view.h`, `host_read_wait_ready`); the sleeper then runs S5 → S1 or NOT_PRIMARY | `host_read_floor_ready` returns ready when `v->read_context != floor->context` (`group_view.h`); then S5 L5297 | sleep ends at `end`; S5 returns BUSY on the next pass | wake: `!g \|\| !v_has` (`group_view.c`, `host_waiters_wake`) | not woken (finding 1); ends at `end` |
| S7 | round start by the sleeping reader | only `!delegated`, L5323-L5350 | begins a round under `h->mu` | n/a in production | n/a | n/a | n/a | n/a |
| S8 | done OK | L5302, L5311, L5320 | handler continues with `*proof_index` and `*leader_hint` | n/a | n/a | n/a | n/a | n/a |
| S9 | follower read, asking the leader | `host_remote_read_index` L4986 | one leader RPC per round under `g_ri_coal[group>>1].mu` (L4999-L5033), waiters of the next round share it; RPC OK → `host_wait_applied` L5252 → S10; groups other than 0 and 2 call `host_remote_cmd` directly L4993-L4998 | not observed: the coalescer never reads the view; leader loss here is seen only when the RPC returns | n/a | `pthread_cond_timedwait(&c->cv, &c->mu, end)` L5027: BUSY; `c->started == UINT64_MAX`: BUSY L5001 | not observed | not woken (finding 1); ends at `end` |
| S10 | follower, applied wait | `host_wait_applied_mode(own=0)` L5509 | loop on `host_view_get`; `v.applied >= idx`: OK L5544; sleep mode 0 L5564 under the bracket L5560-L5573 → S11 | not checked; leader hint from `v.leader` L5543 | n/a | own budget: `efs_apply_wait_start(HOST_READ_TRIES × HOST_TICK_US)` L5524-L5529, renewed on progress only for `own=1`; expired: BUSY L5550-L5555 (not `end`, finding 4) | `!v.has`: return OK L5538-L5541 (finding 3); `host_group_retired` NOT_PRIMARY L5534 | not woken; ends at its grant |
| S11 | follower result | L5253 | `host_follower_read_result` L5057: BUSY with a remote hint other than self → NOT_PRIMARY plus hint; else rc → S8 or return | n/a | n/a | BUSY from S10 or S9 | n/a | n/a |

Where the change is published, from the code (these are the only places a handler can learn of a role or term change): the end of every pump cycle, `host_publish_view` for each hosted group then `host_waiters_wake` (L8708-L8712); `host_stop_leading` when the server is stopping (L8523-L8526, publish only; the wake follows at the end of the same cycle); `host_pending_reads` (L8485-L8486); `host_read_control` on a read ACK (L14080-L14082); the batch handoff (L7601-L7602). Step-downs that publish nothing themselves: `server_raft_host_step_down` L16676, `host_retire_check` L9098, `host_mark_incomplete` L3457, `host_mark_incompatible` L3487 (the pump publishes at L8526 after the incompatible mark, but only for `damaged_row == 2`), and the decide-lost test hook L18886. Each is picked up by the next pump cycle's publish. Not determined: the longest wait between those and the next cycle; the pump's idle timer decides (`host_pump`, not read for this design).

## The boundary

### a. Public operations

One struct `read_proof` per host, embedded as `h->rp` (as `h->gv` is). Files `src/server/read_proof.c` and `src/server/read_proof.h`. Nothing here changes a handler: `host_read_index(h, group, hint)` stays in `raft_host.c` as a four-line `static` wrapper over `read_proof_read` (the 55 callers, the two header-only helpers `read_index_pair_remote.h` and `publication_session_fast.h` and the 18 routed tests see the same name and signature).

| Operation | Replaces | Caller hands over | Gets back | Locks held by caller |
|---|---|---|---|---|
| `read_proof_init(rp, gv, hooks, ctx, ...)`, `read_proof_destroy`, `read_proof_attach(rp, gi, group)` | the start of `ri_begun`, `ri_request` in `attach_group` L8958-L8978 | the group views, the hooks, member count and apply-progress pointers | nothing | none, before any thread runs |
| `read_proof_read(rp, group, &hint)` | `host_read_index_admitted(.., NULL, NULL, 0)` | group; `hint` out | OK; NOT_PRIMARY with a leader hint; BUSY | none |
| `read_proof_read_pair(rp, first, second, &hint)` | `host_read_index_pair` | two groups | same | none |
| `read_proof_prove(rp, group, &hint, &index)` | `host_read_index_admitted(.., NULL, &idx, 1)`, the proof endpoint in `server_raft_host_submit` L16076 | group | OK with the quorum index (never forwards); NOT_PRIMARY; BUSY | none |
| `read_proof_wait_applied(rp, group, idx, &hint, own)` | `host_wait_applied_mode`; `host_wait_applied_own` stays as a one-line wrapper for its five write callers | group, index, `own` | OK; NOT_PRIMARY; BUSY | none |
| `read_proof_start_requested(rp, cores[])` | `host_read_start_requested` | the group cores (NULL when not hosted) | mask of groups whose view the call published | `h->mu` held |
| `read_proof_begin(rp, gi, r)` | `host_read_begin_tracked`, called by `host_publication_committed` L25361 | slot, core | `efs_raft_read_begin` result | `h->mu` held |
| `read_proof_stats(rp, &out)` | `obs_wait_timeouts`, `_own`, `obs_wait_extends` reads at L8027-L8029, L8083-L8086 | out struct | counters | none |

Of the 23 functions: **7 become the interface** (`host_read_index_admitted` as two operations, `host_read_index`, `host_read_index_pair`, `host_wait_applied_mode` with `host_wait_applied_own`, `host_read_start_requested`, `host_read_begin_tracked`); **12 become private** (`host_wait_applied`, `host_ri_complete`, `host_ri_remove`, `host_remote_read_index`, `host_follower_read_result`, `host_read_admit`, `host_read_probe_needed`, `host_read_note_begin`, `host_read_admit_view`, `host_read_request`, `host_read_pair_capacity`, `host_read_pair_note`); **4 stay in `raft_host.c`**: `host_read_probe`, `host_pending_reads`, `host_note_queued_reads`, `host_read_control`. The last four touch no read-request state. They read and write the outbox and inbox (`tx->read_probe_*`, `h->inbox`, `inbox_mu`, `read_noted`) and call `efs_raft_read_control` on the core; they only start rounds and wake sleepers through the owner. That is the same finding as in V3, where 8 of 17 group-view candidates touched no view state: the inventory counted by name. Counting by what a request does, 19 of 23 are the lifecycle. If direct mode goes first (decision 1), `host_read_admit` and `host_read_probe_needed` are deleted and 10 are private.

`read_index_pair_remote.h` (106 lines, four functions, included at L5406 with two macros defined around it) is included by `read_proof.c` instead; `publication_session_fast.h` (16 lines, L15352) stays included by `raft_host.c`.

### b. State

Moves into the owner (only these functions use them): `struct host_group.ri_begun`, `ri_request` (L252; 3 references inside, none outside), the file-scope `g_ri_coal`, `struct host_ri_coal`, `struct host_ri_wait` (L4944-L4961; process-global today, stays process-global), `obs_wait_timeouts`, `obs_wait_timeouts_own`, `obs_wait_extends` (L533-L535). The last three have 1 reference inside and 2 to 3 outside: the `host_periodic_render` text (L8027-L8029), the stats copy (L8083-L8086) and the struct copy (L7896-L7899). They move behind `read_proof_stats`, which changes six lines of the render code.

Shared, stay in `struct efs_raft_host`, and what the owner needs of each:

| Field | Others that read or write it | Owner needs |
|---|---|---|
| `gv` | 42 outside references (publish, wake, the other waits) | the pointer, set at init |
| `s` | 183 outside references | only `s != NULL`, a flag copied at init |
| `raft_id` | 47 | a copy at init (not determined: that it is final when `attach_group` runs; L13703 region decides) |
| `n` (member count, atomic) | 29 | a `const` pointer or a hook; it changes with membership |
| `telemetry` | 8 | a setter; it is published after `attach_group` (L13824) |
| `apply_progress` | written by apply L3670 | a `const` pointer |
| `mu` | 140 | not taken by the request side; the two pump-side operations require it held |
| `inbox`, `inbox_mu`, `inbox_n`, `inbox_bytes` | the inbox, 10-12 each | not used by the owner; used by the four staying functions |
| `g_read_prof` | the profile tick and 34 outside references | see d |

### c. Division with `group_view`

`group_view` publishes the view and owns the sleeper slots; it never decides what a request does. The owner calls: `host_view_get` (L5221, L5280, L5284, L5348, L5359, L5381, L5426, L5523, L5531, L5561), `host_view_covers` L5313, `host_view_quorum_index` L5306, `host_view_wait_apply` L5324 and L5364, `host_read_floor_ready` L5296 and L5363, `group_view_wait_lock` / `group_view_wait_unlock` L5358/L5380 and L5560/L5573, `host_waiter_sleep` L5367 and L5564, and `host_publish_view` in `read_proof_start_requested` (L5189). It does not call `host_waiters_wake`; the pump and the read-ACK path do (L8712, L14082, L8486). `host_waiter_sleep` mode 1 is used only by `host_publication_committed` and stays with it.

Where do `host_read_wait_ready` and `host_read_floor_ready` live? They define "this reader may stop sleeping", which is a rule of the read request, yet `host_waiters_wake` evaluates them for every sleeper from `group_view.c`.

| Choice | What it costs |
|---|---|
| A. Stay in `group_view.h`; `read_proof.h` names them as the wake and completion predicates and `read_proof.c` calls only them | no code moves, no hot-path change, `test_predicates` and `test_floor_ready_modes_2_and_3` stay where they are. The rule of completion is in two files: the predicates in `group_view.h`, their use in S5. A reader of the lifecycle opens both |
| B. Move to `read_proof.h`; `host_waiters_wake(gv, pred, ctx)` takes a function pointer | the contract of `group_view` becomes cleaner, but `struct host_waiter` still carries the request's `round`, `term`, `proof_only` and `floor` fields (`group_view.h`), so the coupling is not removed, only hidden; one indirect call per sleeping reader per pump cycle; the four `host_view_*` predicates they call must move or be passed too; two tests move |
| C. As A, plus the S5 repeats (`host_view_covers`, `host_view_quorum_index` inside the loop) are made one inline function in `group_view.h` | removes the duplicate rule, but changes the loop's text, which a no-behaviour-change refactor should not do |

Recommended: A. Not decided by taste: it is the only choice that moves no hot-path code and keeps the V3 tests unchanged; its cost is the two-file reading, which the contract in `read_proof.h` must remove by stating the rule in words with the line of each predicate.

### d. Outside helpers (17 `static` functions the candidate calls today)

| Helper | Calls | Proposal |
|---|---|---|
| `now_us_`, `host_deadline_us`, `host_past_deadline` | 13, 2, 1 | owner copies (a 5-line `static` each, stateless). Alternative: one shared clock header; not decided here |
| `host_group_incompatible` + `host_halted_hint` | 2 + 2 | pass in: one hook `gate(ctx, group, &hint)` that returns 1 and a hint when the group must not serve |
| `host_group_retired` + `host_fwd_leader_pick` | 1 + 1 | pass in: one hook `retired(ctx, group, &hint)` |
| `host_remote_cmd` | 2 | pass in: hook `remote_read(ctx, group, leader, &rep)` |
| `host_pump_kick` | 5 | pass in: hook `kick(ctx)` |
| `group_raft`, `group_slot`, `host_publish_group` | 10, 3, 6 | not needed once the owner has its own per-group slot and `read_proof_start_requested` receives the cores; needed (plus a lock hook) only by the direct-mode branches |
| `read_prof_on`, `read_prof_add` | 2, 8 | one small header `read_prof.h` declaring the two inline helpers and `g_read_prof` (shared with the profile tick). The only internal-header item |
| `outbox_pure_read_control`, `outbox_nentries`, `rd64be` | 4, 2, 1 | stay: used only by the four staying functions |

Result: 4 hooks, 3 owner copies, 1 internal header item, 3 not needed, 3 stay with their callers. That is below the line of about 8 for an internal header. With the direct-mode branches kept, `group_raft`, `host_publish_group` and a lock hook for `h->mu` join (7 hooks plus the header): that is the sign the boundary is wrong, and the smaller boundary is to remove the dead branches first (decision 1).

### e. Hot path

`host_read_index` is called 87 times from 55 handlers. It stays a `static` four-line wrapper in `raft_host.c`; the call into `read_proof_read` is the one new call. Not determined: whether `host_read_index_admitted` (188 lines) is inlined into it today; the `-O2` object decides, and phase 2 measures instruction counts as V3 did (`scripts/evidence/v3/instruction_counts.sh`). Stay `static inline` in `group_view.h`: `host_view_get`, `host_view_covers`, `host_view_quorum_index`, `host_view_wait_apply`, `host_read_wait_ready`, `host_read_floor_ready`, the bracket (unchanged by this task). Ordinary functions: everything in `read_proof.c`. The hooks add indirect calls: `gate` twice per loop pass (L5213, L5229), `kick` per round. They are the one measurable cost of the hook choice (decision 2).

### f. Lock rules

Order: `h->mu`, then `cv_mu`, then `view_mu` (the V3 order, unchanged). The request-side operations (`read_proof_read`, `read_proof_read_pair`, `read_proof_prove`, `read_proof_wait_applied`) must be called with no lock held: they sleep, and `host_remote_read_index` makes an RPC. Inside they take: `cv_mu` only through the bracket (no other lock inside it, per `group_view.h`); `g_ri_coal[].mu`, a leaf, released across `host_remote_cmd` (L5018-L5021) and held with `c->cv` waits; `view_mu` only inside `host_view_get` after 64 odd loads. They take `h->mu` nowhere once direct mode is gone (today at L5275 and L5330 only when `!delegated`). `read_proof_start_requested` and `read_proof_begin` require `h->mu` held and take `view_mu` through `host_publish_view`; they take no other lock. The pump calls the wake under `h->mu` (order `h->mu`, `cv_mu`). Not determined: that no one of the 55 callers holds `h->mu` or `cv_mu` when it calls `host_read_index`; a lock-order check over the callers would decide (the entry map says "no host lock held" only for the forwarding branch).

### g. Tests

Owner interface test `tests/test_read_proof.c`, linking the real `read_proof.c` and `group_view.c` over the V3 stub getters plus stubs for `efs_raft_read_begin`, `efs_raft_read_round`, `efs_raft_read_pending`, `efs_raft_read_quorum`, `efs_raft_role`, `efs_raft_commit`, `efs_raft_term` and fake hooks. Threads are needed only where a reader must be asleep; use the V3 trick (the main thread can take `cv_mu` only when the sleeper is inside its wait). No sleeps; the deadline is passed in the past or made short by the owner's `end` argument only if the owner is given a settable budget (a test-only parameter; otherwise the cases with a deadline use a pre-expired `end`, which needs one internal entry point; open question in decision 6).

One named case per transition, and one per state for the replica losing leadership:

| Case | Transition |
|---|---|
| `test_gate_refuses_incompatible_group` | S0 → NOT_PRIMARY with the hook's hint |
| `test_no_replica_refuses` | S0 `!has` → NOT_PRIMARY |
| `test_follower_asks_leader_and_applies` | S1 → S9 → S10 → S8; hook returns an index; view applied later |
| `test_follower_proof_endpoint_never_forwards` | S1 with `proof_only` → NOT_PRIMARY |
| `test_follower_leader_is_self_refuses` | S1 `leader == self` |
| `test_admission_takes_view_commit_and_next_round` | S2 → S3 admission fields |
| `test_admission_overflow_is_busy` | S2 `begun >= UINT64_MAX-1` → BUSY |
| `test_request_raises_ri_request_once_and_kicks` | S3 → S4; two readers, one raise, one kick |
| `test_start_requested_begins_when_previous_quorum_done` | S4 → S5, mask and publish |
| `test_start_requested_skips_unled_and_unhosted` | S4 skip cells |
| `test_covered_read_returns_proven_index` | S5 → S8 |
| `test_frozen_floor_survives_a_later_round` | S5 floor |
| `test_proof_only_returns_quorum_index_before_apply` | S5 proof-only |
| `test_waits_in_slot_and_wakes_on_publish` | S5 → S6 → S5 |
| `test_deadline_returns_busy` | S5/S6 deadline |
| `test_applied_wait_expires_busy_and_counts` | S10 deadline; `read_proof_stats` |
| `test_applied_wait_retired_group_refuses` | S10 retired hook |
| `test_follower_result_redirects_leader` | S11 |
| `test_coalescer_shares_next_round` | S9 two callers, one hook call |
| `test_loses_leadership_at_gate` (S0), `_at_admission` (S2), `_after_admission` (S3), `_after_request` (S4), `_while_checking_with_floor` (S5), `_while_asleep` (S6), `_while_asking_leader` (S9), `_during_applied_wait` (S10) | the replica leaves leadership in each state: higher term with role leader; role follower; each is one case, with the expected return and hint from the table |
| `test_context_change_refuses` | S5/S6 read context |
| `test_replica_gone_while_asleep` | S6 `!has` wake |

Tests that name these functions: 54 files; 51 read `raft_host` through `tests/csrc.py` (`MODULES['raft_host']`, L22-L23): the ones that cut a moved function's text break when it moves (V3 ported 20 of its own). Which break: not determined; `python3 scripts/tests_for.py server/raft_host --run` on the moved tree decides. The routed set that must reach the owner is the entry map's 18 rows. The three that inventory counted as including `raft_host.c` directly (`test_apply_ring.py`, `test_host_batch_handoff.py`, `test_put_ticket_batch.py`) and the text anchor of `test_read_retry_budget.py` (slice from `host_remote_cmd_impl` to the forward declaration of `host_wait_applied`) need the first look.

### h. Size

Lines that move: 540 of the 703 (19 functions) plus 18 (`host_ri_wait`, `host_ri_coal`, `g_ri_coal`) plus the block comments before them (about 40), so about 600; `raft_host.c` after: about 25,700 of 26,314. If direct mode is removed first, about 105 fewer lines move and `raft_host.c` is about 105 lines shorter before the move. `read_proof.c` about 500 lines, `read_proof.h` about 200 (contract, no more than `group_view.h`), `read_index_pair_remote.h` unchanged (106).

Expected reading set for the trial question (A group's local replica loses leadership...): `read_proof.h` (about 200), the request half of `read_proof.c` (about 330), `group_view.h` and `group_view.c` (580), and three excerpts of `raft_host.c` that the contract names as the publication points (`host_stop_leading`, the pump cycle end, `server_raft_host_step_down`; about 80): about 1,200 lines. The trial of Nov 11 needed the publishers, the read loop and the leadership handling in `raft_host.c`; whether it falls is not determined by this design. A reader trial after phase 2 decides. I did not read the key of the trial page.

## Findings in the code as it is (cells the code does not handle, and what does not fit)

No behaviour changes for any of them. The coordinator decides at the merge whether one becomes a K row (1, 3, 5, 6, 9, 10 are the documented ones).

1. **Host stop wakes no read sleeper.** `server_raft_host_stop` clears `running`, kicks the pump and broadcasts the POSIX lock-wait queue `wait_head` (L13977-L13980, a different queue: L421-L422), joins the pump, then frees the cores and sets `r = NULL` (L14004-L14007). No view is published for a NULL core and `host_waiters_wake` is not called after the pump exits. A reader in S6 or S9 sleeps until its `end` (at most 400 ms) and returns BUSY; it does not see the replica go. `host_stop_leading` (L8516-L8532) relinquishes leadership first, so readers asleep at stop normally wake with NOT_PRIMARY through S6's predicate; the stop path itself does not. Not determined: whether handler drain finishes before this point (the comment at L8508 says it closes new peer requests first); the shutdown order in `efsd.c` decides.
2. **Several step-downs publish nothing themselves** (list above). They rely on the pump's next cycle. A sleeping reader is then woken by the end-of-cycle wake; the bound is the pump cycle, not determined here.
3. **`host_wait_applied_mode` returns OK when `!v.has` (L5538-L5541)**, with the comment "the leader already waited in submit". That is true for a write verdict (`own=1`) and is not a statement about a follower read (`own=0`, S10). The cell cannot be reached while the host runs: `has` is cleared only by `host_publish_view(.., NULL)`, which `host_publish_group` calls only for a hosted slot whose core is NULL, and `g->r` is NULL only at attach (L8978) and after stop (L14006). A stale view at stop keeps `has = 1`. So the OK is dead for reads. The test `test_absent_replica_clears_has_and_proof` covers the publish, not the read.
4. **A follower read has two budgets.** `end` bounds the leader RPC wait (L5027); `host_wait_applied` starts its own grant of 400 ms (L5524-L5529). The coalescer waiters never read the local view (S9): a local leadership change while a follower read waits for its RPC is seen only after it returns.
5. **A request that holds a frozen floor loses to a role change, not to the term check.** The role test (L5237) is before the term test (L5292) and the floor test (L5296-L5303). A read that has seen its proof, and then the replica stops leading, does not return the proof; it asks the new leader (S1). Same role and a higher term (re-election): NOT_PRIMARY (L5292). The contract must state this order; the code does not say whether it is intended. Not determined: what `tests/test_readindex_result.py` pins ("term-change refusal" in the entry map).
6. **The read-context check (L5297) is reachable only while the role is leader.** The context is the core's `read_epoch`, 0 while not leader, when storage failed, or while receive-persisting (`raft.c:3584-3589`); it changes when `read_proof_forget` runs on a term or configuration change (`raft.c:254-258`, `raft.c:3396-3402`). Not determined: whether a leader can be `recv_persisting`, which would give a transient NOT_PRIMARY for a request with a frozen floor; `raft.c` decides.
7. **The direct (`!delegated`) mode is not reachable in a running server.** About 105 lines serve it: L5274-L5279, L5323-L5350, L5447-L5500 and the two functions `host_read_admit` and `host_read_probe_needed`. Tests that splice `host_read_admit` (`test_readindex_admission.py`) pin it. This is the largest reason the outside-helper list is long.
8. **No caller cancellation.** A request ends only by a result or by its deadline; there is no cell for a client that has gone. Not determined: whether the handler layer cancels; not in this code.
9. **Deadlines use `CLOCK_REALTIME`** (L4804-L4821, L761) while `host_remote_cmd` uses `CLOCK_MONOTONIC` for the thread deadline (L4764-L4768). `pthread_cond_timedwait` on a default condition variable is realtime too, so the sleeps agree; a step of the wall clock moves `end`. Observation only.
10. **Pair path**: the first admission's `ri_request` stays raised if the second admission fails (L5436-L5444); the round runs unused. Benign. Not determined: how the pair worker threads (`read_index_pair_remote.h` L29-L106) stop at host stop.
11. **No test joins a failed begin to the reader's BUSY.** `test_readindex_admission.py` pins that the pump retries an early refusal and publishes nothing; no test connects a failed begin to the reader returning BUSY at its bound.
12. **`host_publication_committed` still begins a round under `h->mu` on the reader thread** (`host_read_begin_tracked`, L25217). "Readers never take `h->mu` to begin" holds for `host_read_index_admitted` and `host_read_index_pair` only; `test_raft_read_admission.py` asserts it for those two bodies.

## Open decisions

1. **Delete the direct mode before the extraction?** (a) Yes, as its own commit first: about 105 lines gone, the owner needs no `h->mu`, no `group_raft`, no `host_publish_group`, 4 hooks instead of 7; cost: one splice test (`test_readindex_admission.py`) and parts of `test_readindex_operation.py` and `test_readindex_tickets.py` change; it is a change of what the product contains, though not of what it does. (b) No, keep it in the owner: 3 more hooks, an `h->mu` lock hook, and an owner that can take `h->mu`; the lock rules gain a case. Recommended: (a).
2. **Outside helpers as hooks or an internal header?** (a) 4 hooks plus 3 clock copies plus `read_prof.h`: indirect calls (the gate twice per loop pass), a fake-hook test is possible. (b) A `host_internal.h` declaring `host_group_incompatible`, `host_halted_hint`, `host_group_retired`, `host_fwd_leader_pick`, `host_remote_cmd`, `host_pump_kick`, `group_raft`, `host_publish_group`, `read_prof_*`, `now_us_`: no indirect calls, but 11 or more symbols made visible and the test needs the real `raft_host.c`. Recommended: (a).
3. **Predicates** A, B or C of section c. Recommended: A.
4. **Does the applied wait move?** (a) Yes, as `read_proof_wait_applied`: the read lifecycle is complete (S10 is inside it); five write-verdict waiters then depend on the read-proof owner for a function that is about applying, not proving. (b) No: S10 stays in `raft_host.c`; the follower path of the owner calls it through a hook, and the lifecycle is split again. Recommended: (a), with the `own` flag in the contract.
5. **The four transport functions stay in `raft_host.c`** (`host_read_probe`, `host_pending_reads`, `host_note_queued_reads`, `host_read_control`)? (a) Yes. (b) Move them as "read control" into the owner: they need the inbox and outbox structures through an internal header (more than 8 symbols). Recommended: (a).
6. **Test port and a settable budget.** (a) A new module `read_proof` in `tests/csrc.py` for the owner-text tests (as V3 did for `group_view`), and a test-only way to make the deadline short (an `end` argument to an internal entry point, or `read_proof_set_budget_us`): adds one function to the contract. (b) Add the new files to `MODULES['raft_host']`: fewer edits, but the first-match rule makes the order part of the contract. Recommended: (a) with a documented budget setter, since a test must not sleep.
7. **Do findings 1, 3, 5 and 6 get a K row now?** This is a refactor with no behaviour change, so the contract would document them as they are. (a) Document only. (b) Open K rows (blocks alpha: not determined; a live run would decide). Recommended: (a), and the coordinator decides about rows.
8. **Reading-set measurement**: run a fifth reader trial after phase 2 (one agent, the same question)? Cost: one agent run. Recommended: yes, as for V3.

## Reproduction

```
python3 scripts/evidence/v4/lifecycle_inventory.py src/server/raft_host.c            # inventory (exit 0)
python3 scripts/evidence/v4/lifecycle_inventory.py src/server/raft_host.c --functions # per-function line ranges
```

The state table was read from the code at the lines given; nothing was executed. Limitations: the table is for the delegated mode; pump idle timing, handler drain order and the tests' pinned behaviour were not read (marked "not determined" above).

## Coordinator decisions and stage A (Nov 11)

**Decisions.** (1) Delete the direct mode first, as its own commit, only if no test executes it. (2) No function pointers (an indirect call on every metadata read). One small internal header `src/server/read_proof_host.h` declares what the owner needs from the host, each with a one-line contract; those functions lose `static` in `raft_host.c`; tests define stubs with the same names. (3) `host_read_wait_ready` and `host_read_floor_ready` stay in `group_view.h`; the read-proof contract names them as its wake rule and completion rule and says why they live there (the wake needs them and they are pure over a view copy). (4) The applied wait (`host_wait_applied_mode`, `host_wait_applied_own`, `host_wait_applied`) does not move: five write-verdict waiters use it and write paths must not depend on the read-proof owner. The contract says "the follower read's applied wait is the host's"; the owner reaches it through `read_proof_host.h`. `read_proof_wait_applied` and `read_proof_stats` therefore leave the owner's operations, and `obs_wait_*` stay in the host. (5) The four transport functions stay. (6) No test-only budget setter; the owner test uses the real bounds, a timeout case waits the product's own bound (at most 400 ms) once, few such cases, no sleep. (7) Findings 1, 3, 5, 6, 9, 10 are documented, not changed. (8) The fifth reader trial is the coordinator's, after the merge.

**Additions.** A: `read_proof.h` contains the state table itself, short (one row per state: what moves it on, on a role or term change, on the deadline, on a changed read context, on a replica that is gone), 150 lines or fewer, contract and prototypes only; private layout and inline bodies in `read_proof_impl.h`, included at its end, as Task V6 does for the group view. B: in stage B, check whether the roughly ten sites that publish each hosted group's view and then call `host_waiters_wake` are the same statement sequence; replace identical ones by one function (`host_publish_all` or similar), leave and list the different ones; the contract names that function and lists the paths that do not publish by themselves (`server_raft_host_step_down`, `host_retire_check`, `host_mark_incomplete`).

**Stage A result: the direct mode is not dead in tests; nothing deleted.**

Production proof. `h->s` is assigned in one place, `raft_host.c:13703` (`h->s = s;` inside `server_raft_host_start`). The only caller of `server_raft_host_start` in `src/` is `efsd.c:1539`, which passes `&server`; `server_internal.h:440` declares it; `handler.c:1256` only mentions it in a comment. In `tests/` the name occurs only as text anchors (`test_query_page_barriers.py:9`, `test_shutdown_bounded.py:262`); no test calls it.

Whole-file compilers of `raft_host.c`: `tests/test_snap_import_host.c` and `tests/test_snap_import_pump.c` (Makefile L1437-L1441) and `tests/decoders/apply.c` (L1468). None names `host_read_*`, `host_wait_applied`, or `host_read_index`; they run snapshot import and apply with `s == NULL` and execute no direct-mode read code. The other five C tests that mention `raft_host` only define stubs named `server_raft_host_*`.

Python tests that cut functions out of `raft_host.c`, of the 54 naming a candidate function; those naming a direct-mode function or running with `s == NULL`:

| Test | Executes direct-mode code? | What it uses it for |
|---|---|---|
| `test_readindex_admission.py` | yes: runs `host_read_admit` (L8, L29-L48) | the admission refusals BUSY/NOT_PRIMARY and when a round starts |
| `test_readindex_pair.py` | yes: L110 sets `h.s=NULL`, then L111-L140 run the `!h->s` branches of `host_read_index_pair` and `host_append_authority`; the splice also contains `host_read_probe_needed` (L75) | pre-send fresh admissions under `h->mu`, pending rounds, routing, overflow, failures |
| `test_readindex_result.py` | yes: its `h` has `s == NULL`; L94 expects `host_read_admit` to be called once (`admissions==1`) | proof floor, follower path, term-change refusal in `host_read_index_admitted` |
| `test_raft_read_admission.py` | text only: asserts `!delegated && !floor.index` (L101) and the `else` branch of the delegated admission (L99); also calls `host_read_start_requested` | pins that the server-mode path takes no `h->mu` |
| `test_readindex_tickets.py` | only the guard: L89 sets `h.s=0` and expects `host_read_start_requested` to return 0 | the `if(!h->s)return 0;` guard in a function that stays; not a branch to delete |
| `test_readindex_operation.py`, `test_lookup_*`, `test_namespace_leader_route.py`, `test_query_*`, `test_apply_unsupported.py` | no: they set `h.s` to a server or do not read the branches | |

Per the rule, nothing was deleted. The decision is the coordinator's: deleting the direct mode means rewriting or dropping the first three tests and the text assertions in the fourth (about 105 lines of product code against tests that pin refusals). Without the deletion the owner needs, besides the five declarations below, `group_raft`, `host_publish_group` and a way to take `h->mu`, which is 9 declarations in `read_proof_host.h` (above the line of about 8) and the owner would take `h->mu`.

**`read_proof_host.h` as now planned** (each declaration is a function that loses `static` in `raft_host.c`; tests stub them by name):

| Declaration | One-line contract | Needed if direct mode is deleted |
|---|---|---|
| `int read_proof_host_gate(struct efs_raft_host *h, uint8_t group, int *hint)` | 1 and a leader hint when the group must not serve reads (incompatible or damaged); today `host_group_incompatible` plus `host_halted_hint` | yes |
| `int read_proof_host_retired(struct efs_raft_host *h, uint8_t group, int *hint)` | 1 and a live voter hint when this node's replica of the group is retired; today `host_group_retired` plus `host_fwd_leader_pick(h, group, -1)`; used by the applied wait only, so if the wait stays in the host this one is not needed | no (wait stays in the host) |
| `int read_proof_host_remote_read(struct efs_raft_host *h, uint8_t group, int leader, uint64_t *index, int *hint)` | one blocking leader RPC for a ReadIndex; today `host_remote_cmd` with the mkfs reply | yes |
| `void read_proof_host_kick(struct efs_raft_host *h)` | wake the pump; today `host_pump_kick` | yes |
| `int read_proof_host_wait_applied(struct efs_raft_host *h, uint8_t group, uint64_t idx, int *hint)` | the follower read's applied wait is the host's; today `host_wait_applied` | yes |
| `struct efs_raft *group_raft(...)`, `void host_publish_group(...)`, `void read_proof_host_lock(h)` / `unlock(h)` | direct mode only: take `h->mu`, find the core, publish | no (only if direct mode stays) |

If direct mode is deleted: 4 declarations (gate, remote read, kick, wait_applied); `retired` is not needed because decision 4 keeps the applied wait in the host. If it stays: 8 (those four plus `group_raft`, `host_publish_group`, lock, unlock). Outside `static` callees of the candidate: 17 today; the owner takes 3 by copying clock helpers, 2 through `read_prof.h` (not yet decided against the header rule: it would be a second internal header; alternative is to give the owner its own counter struct), 3 not needed, 3 stay with the transport functions, and 4 (or 8) through `read_proof_host.h`.

## Stage A decision and result: the direct mode is deleted

Decision (coordinator, Nov 11): delete the direct mode and port the tests that exercised it to the mode production runs. Profile helpers `read_prof_on` and `read_prof_add` go into `read_proof_host.h` as two more declarations, no second header and no new counter struct. The planned header then has **six declarations**: gate, remote read, kick, wait applied, `read_prof_on`, `read_prof_add`. The earlier table's `retired`, `group_raft`, `host_publish_group` and the lock pair are not needed. The direct mode is now gone from the product, so the 8-declaration variant above no longer exists.

Removed from `raft_host.c`: `host_read_admit`, `host_read_probe_needed`, the `else` branch of the admission and the locked begin in `host_read_index_admitted` (with the `delegated` variable: its three uses became the constants 1 and mode 3), and everything after the `if(h->s)` section of `host_read_index_pair`. 197 lines changed: 28 added, 169 removed (net 141 shorter). The two `h->s` guards kept here at first were removed in stage A2 (below). Remaining `g_read_prof.ri_lock` and `ri_begin` counters are now never incremented; they still render (value 0), unchanged to avoid a behaviour change in the profile line. The count of 12 before and 4 after used a pattern that missed several spellings; the complete list is in stage A2.

### Guarantee pinned today → where it lives after

| # | Test and assertion group (before) | Guarantee | After |
|---|---|---|---|
| 1 | `test_readindex_admission.py`: first `host_read_admit` starts one round, `want`/`round`/`term` from the core | an admission carries the commit floor, the next round and the term | ported: same file, `host_read_admit_view` over the view, `ri_begun`+1 |
| 2 | same: 100 arrivals during an unfinished round get round 12, their own commit, start nothing | arrivals during a round queue for the next and never borrow it | ported: 100 `host_read_admit_view` calls, one shared request, `host_read_start_requested` starts nothing while the previous round is unproven |
| 3 | same: idle after pending, `host_read_admit` begins round 12 | a fresh arrival after a finished round makes a round start | ported: the pump (`host_read_start_requested`) starts it, once |
| 4 | same: quorum 12 done, own apply parked: arrival starts round 13, not borrowing 12 | a completed quorum does not cover a later arrival | ported: admission after the begin asks round 13 and the pump starts 13 |
| 5 | same: begin fails with `EFS_ERR_IO` returns it, `started == 0` | a failed begin starts nothing | ported in server-mode form for the early refusals: the pump skips the group, publishes nothing and retries on its next cycle. The error no longer reaches the reader (removed with the direct mode). A failure after `efs_raft_read_begin` has advanced the round (`raft.c:3430-3438`: `solo`/`broadcast_read_probe`) is not retried, because the round then meets the request (`host_read_start_requested` skips it, L5138); the reader's outcome is BUSY at its 400 ms bound in both cases |
| 6 | same: round `UINT64_MAX` returns BUSY | saturation refuses | ported: `ri_begun >= UINT64_MAX-1` and `read_round == UINT64_MAX-1` give BUSY |
| 7 | same: follower role and NULL core return NOT_PRIMARY | non-leader refusal | ported: follower role, `has == 0`, unknown group give NOT_PRIMARY |
| 8 | same: "atomic fresh start": admission and begin in one critical section | the reader contends once on the pump mutex | removed with the direct mode (`test_readindex_admission.py` old L29-L31); server-mode readers never take `h->mu`, asserted by `test_raft_read_admission.py` |
| 9 | `test_readindex_pair.py`: server-mode block (L99-L109 before): both rounds requested, nothing begun, late commit floor, split leadership, remote proof, overflow, wait error | the paired server path | unchanged |
| 10 | same, direct block: `host_append_authority` with an absent group, hint 7, nothing begun | unknown group refuses with the peer hint | ported: same assertion with `h.s` set |
| 11 | same: `host_append_authority` both led, `begun==2 && waits==2` | both groups proven | ported: `!begun && waits==2` (the pump begins) |
| 12 | same: second RPC "needs newer rounds" (`begun==4`) | an RPC after a probe needs later rounds | ported: the test plays the pump (`host_read_begin_tracked`), then the next pair requests rounds 9 and 19 |
| 13 | same: pending rounds, pending with quorum: probes only when needed (`host_read_probe_needed`), fresh probes not old proof | which rounds the RPC begins | ported as "begins nothing; requests the next round" for pending and pending-with-quorum; the "probe needed" decision itself is removed with the direct mode (`host_read_probe_needed`, old L75 splice); the pump side is pinned by `test_readindex_tickets.py` |
| 14 | same: split leadership, direct, local begin before the remote proof (`begun==1`) | begin of the local proof before blocking | removed with the direct mode: the server path requests and the pump begins (server split-leadership block, row 9, remains) |
| 15 | same: same group twice, one scalar read | `first == second` | ported: server mode |
| 16 | same: pending local round with a follower peer: nothing begun, one scalar, helper capacity 0 | mixed leadership needs no helper | ported: server mode |
| 17 | same: both followers, overflow `round == UINT64_MAX`, begin fails with `EFS_ERR_IO`, wait error | direct refusals | the follower and wait-error cases exist in server mode (row 9); overflow is row 6; begin failure is row 5; removed with the direct mode as direct-pair assertions |
| 18 | same: negative "late-admission probe" (a completed earlier round must not suppress a required probe) | `host_read_probe_needed` | removed with the direct mode (it mutated `host_read_probe_needed`); the sibling negative "unpublished begin" (admission from `v->read_round`) stays |
| 19 | `test_readindex_result.py`: all `host_read_index_admitted` cases ran with `s == NULL` and a supplied admission | exact proof floor, BUSY/NOT_PRIMARY refusals, proof endpoint never forwards, wake-bracket recheck, delayed floor modes | ported: the host has `s` set, the sleeper is now mode 3 (`any==3`), the request is raised once per call |
| 20 | same: `admission == NULL` calls `host_read_admit` once (`admissions==1`) | a request without an admission takes a fresh one | ported: calls `host_read_admit_view` once and `host_read_request` once |
| 21 | `test_raft_read_admission.py` L96-L109: text of `if(delegated) {`, `!delegated && !floor.index`, the pair's `if(h->s) {` | server mode takes no `h->mu` to admit or begin | replaced by assertions that `delegated`, `host_read_admit(`, `host_read_probe_needed` do not occur and that the reader and pair bodies contain no `pthread_mutex_lock` or `h->s` |
| 22 | same, L1-L94 stress: concurrent lock-free admissions, probes follow arrival | unchanged | unchanged |

Totals: 22 rows. Ported: 14 (rows 1-7, 10, 11, 12, 15, 16, 19, 20); text assertions replaced: 1 (row 21); unchanged: 2 (rows 9, 22); ported in part with the direct-only part removed: 2 (rows 13, 17); removed with the direct mode: 3 (rows 8, 14, 18). No row ends otherwise.

### Stage A test results (host intel2, tree `fd2a410d8`, Nov 11)

Verdict: PASS. Exact tree: commits `506ee3aa2` (deletion and test ports) and `fd2a410d8` (map pages), built from `git archive HEAD` into `~/efs-taskV4/` (removed afterwards). Raw logs outside Git in `~/efs-artifacts/taskV4-20261111/` on intel2 with `SHA256SUMS`. Order and results (seconds): `make` rc 0 (28); fixtures `make test-bins tests/faults/efsd` rc 0 (15); the five tests `test_readindex_admission`, `test_readindex_pair`, `test_readindex_result`, `test_raft_read_admission`, `test_readindex_tickets` each rc 0 in under 1 s (no budget row touched); `python3 scripts/tests_for.py server/raft_host --run` rc 0 (581); `make test` rc 0 (154); `python3 tests/test_make_test_recipe.py` rc 0 (1); `tests/sanitizer_check.sh asan` rc 0 (605); `tests/sanitizer_check.sh tsan` rc 0, 53 run, 0 failed (721; `k96_status_slots_live` and `k37_segment_rebuild` both passed). Limitation: the ASan variant inside `test_readindex_pair.py` is skipped on the Mac (no ASan leak runtime) and ran on intel2 only.

## Stage A2: what was still left of the direct mode (commit `87e25c5f0`, on devel `88bc1189a`)

**Every `h->s` use in a condition in `raft_host.c`, before and after.** Before (devel plus stage A): L923 `peer_addr` (`!h->s` before it dereferences `h->s->lock`), L5133 `host_read_start_requested` (`if(!h->s)return 0;`), L8286 `host_pending_reads` (`if(!h->s)return;`), L8451 and L8491 (`host_stop_leading(h, h->s && server_stop_requested())`, pump), L13912 `host_read_control` (`fresh_probe=h->s && ...`), L13931 (`handled && h->s && BO_ON()`), L15965 (`EFS_FAULTS` hook: `d && *d && h->s && snprintf(.. h->s->id ..)`), L16991 `host_read_leader_hint` (`h->s ? host_namespace_leader_hint(h,group) : -1`). After: L923 and L15962 only.

| Use | What it was for | Decision |
|---|---|---|
| L923 `peer_addr` | protects the dereference of `h->s->lock` on the next lines | kept: a real guard |
| L15962 fault hook | protects `h->s->id`; compiled only with `EFS_FAULTS` | kept: a real guard |
| L5133, L8286 | mode switches: a host without a server requested rounds nobody starts, and skipped the early read control | removed; `tests/test_readindex_tickets.py` (the `h.s=0` line) and `tests/test_raft_pending_reads.py` (the `h.s=NULL` call, the `struct server` and the field) no longer build a host without a server |
| L8451, L8491 | `server_stop_requested()` is a global and touches no `h->s`; the `h->s &&` meant "a host without a server never relinquishes" | removed |
| L13912, L13931 | `fresh_probe` and the batch-obs counters were off without a server | removed |
| L16991 | routing hint off without a server; `host_namespace_leader_hint` itself reads no `h->s` | removed; `tests/test_namespace_leader_route.py` still sets `h.s`, harmless |

Proof that no product caller passes the other value: `h->s` is assigned only at `raft_host.c:13560` from `server_raft_host_start`, whose only caller is `efsd.c:1539` with `&server`; no code in `src/` resets it.

**Group-view owner.** (a) The `delegated` parameter of `host_read_wait_ready` and `host_read_floor_ready` (`group_view_impl.h`, prototypes in `group_view.h`) and its clause `!delegated && (!v->read_pending || v->read_done) && !host_view_wait_apply(...)` are removed; the function now ends `return 0`. Proof: the only product callers are `raft_host.c` L5239 and L5278, which pass `1`, and `host_waiters_wake` (`group_view.c`), which passed `w->any == 3`; the only other callers were tests. (b) Waiter mode 2 is removed from `host_waiters_wake` (`w->any >= 2` is now `w->any == 3`) and the contract says so ("there is no mode 2; the number is not reused"). Proof: every `host_waiter_sleep` call is `raft_host.c` L5282 (mode 3), L5423 (mode 0), L25229 (mode 1); no call passes 2. (c) `host_view_wait_apply` stays: the profile line in `host_read_index_admitted` calls it. `scripts/check_group_view_privacy.py`, its `--self-test` and `tests/test_group_view` pass (results below).

**Tests changed in A2.** `tests/test_group_view.c`: the case `test_floor_ready_modes_2_and_3` is now `test_floor_ready_mode_3` (the assertion that an idle round is ready in mode 2 is removed with the mode; the `src/server/README.md` row follows). `tests/test_read_waiters.py`: default waiter is mode 3, three assertions that only held for mode 2 are changed or removed with it (an idle or older finished round wakes a reader; the "scheduling wake is not completion" block). `tests/test_readindex_result.py`: predicate calls lose the last argument. `tests/test_readindex_pair.py`: stale words and the dead `h.s` and `published` removed; three cases that lost their server-mode twin in the port are back as server-mode cases (mixed leadership with a saturated led group: BUSY, nothing requested; group 0 follower with group 2 led: one scalar read and one wait; an absent group with a scalar read error: that error after one scalar read).

**Guarantee table, totals after A2.** The 22 rows of stage A stand (14 ported, 1 text replaced, 2 unchanged, 2 ported in part, 3 removed with the direct mode). Added rows: 23 (pair, saturation with mixed leadership): ported; 24 (pair, group 0 follower success): ported; 25 (pair, absent group with a scalar error): ported; 26 (`test_read_waiters.py`, an idle or older finished round wakes a waiter, and the first-signal-then-capture block): removed with mode 2; 27 (`test_group_view.c`, idle round ready in mode 2): removed with mode 2. Totals: 27 rows, 17 ported, 1 text replaced, 2 unchanged, 2 ported in part, 5 removed with the direct mode or mode 2.

### Stage A2 test results (intel2, PASS)

Tree `87e25c5f0` (code identical to the later commits except `tests/test_shutdown_bounded.py`). Method as stage A: `git archive HEAD` to `~/efs-taskV4`, `make -j8`, run in `screen`; raw logs with `SHA256SUMS` stay on intel2 in `~/efs-artifacts/taskV4-20261111-a2/` (first run) and `-a2b/` (rerun), not in Git; the tree is removed.

- Build 28 s; `tests/test_group_view` and the seven read-path Python tests (admission, pair, result, raft_read_admission, tickets, pending_reads, read_waiters) pass, each under 2 s.
- `tests_for server/raft_host --run` PASS (574 s on the rerun); `tests_for server/group_view --run` PASS (1 s).
- `make test` first run FAILED: `tests/test_shutdown_bounded.py` line 167 still searched for `host_stop_leading(h, h->s && server_stop_requested());`. Fixed in `666d280b1` (the text check now expects `server_stop_requested()`); rerun `make test` PASS (165 s), recipe test PASS. The same stale line made `tests_for raft_host` fail in the first run; it passes in the rerun.
- Sanitizers (first run, before the text-only fix): ASan gate PASS (77 run, 0 failed, 499 s of gate, 612 s wall); TSan gate PASS (53 run, 0 failed, 678 s; wall 719 s), including `k96_status_slots_live` and `k37_segment_rebuild` (K123 did not fail this time; it is an intermittent, not fixed here).
- Mac: `make docs-check`, `check_doc_links`, `tests/test_code_graph.py`, `tests_for`, `csrc`, and the group-view privacy check with its self-test pass. `test_host_view_spin` and `test_readindex_publish` fail to compile on the Mac with clang unused-function errors; the same on devel `88bc1189a`, they pass on Linux.
- Not run: no `tests/budgets.tsv` row was changed.
