# ReadIndex entry map and investigation baseline before the `raft_host.c` split (W171 2b, Task DO)

**Update (Nov 11, Task V4):** the direct mode (`h->s == NULL`: `host_read_admit`, `host_read_probe_needed` and the locked branches of `host_read_index_admitted` and `host_read_index_pair`) was deleted; rows and line numbers below are the Nov 9 snapshot, and the two removed functions no longer have rows ([read-proof design](read-proof-ownership-20261111.md)).

**Verdict: PASS, map and baseline recorded; the split stays blocked** (Nov 9, 2026). Purpose: the two preconditions the [structure plan](structure-plan-20261102.md) (2b, and the Nov 5 decision in phase 3) sets for the `raft_host.c` pilot split besides Task DJ's merge: an entry map of the ReadIndex code, and a before-measurement of one unfamiliar ReadIndex investigation by the [reading-set method](reading-set-baseline-20261104.md). Docs and measurement only: no source or test changed, nothing built or run on a host. No K row; blocks alpha: no. The map does not unblock the split: 2b also waits on the `devel-w123-recovery` merge, which is the user's decision and still open. Merged into devel in wave 65 (Nov 9). Status Nov 9: remaining failures none; active run none; next action: the 2b split after the `devel-w123-recovery` merge.

- Tested tree: `devel` at `df7d11371` (wave 64 merged). `src/server/raft_host.c` is 26,262 lines. Line numbers on this page are a snapshot of that commit; names are checked by the self-check below.
- Environment: the macOS checkout only; `graphify` from the repository's pinned environment; Python 3. No cluster, no fstor007 run (nothing on this page needs a build).

## Findings the split must act on

1. **The prefix is not one concern.** The plan counts 30 `host_read_*`/`host_wait_*` functions for `read_index.c`. Today there are 28 definitions in `raft_host.c` (`grep` below; 29 lines, one of them the forward declaration of `host_wait_applied`) and 4 more in `read_index_pair_remote.h`, 32 in all. Of the 28 in `raft_host.c`, 17 are the ReadIndex round, the pair path and the apply wait (the 4 in the header are pair path too). The other 11 match the prefix but belong elsewhere: the outbox and inbox read-probe transport (2), KV inode-lane reads that call `host_read_index` (4), a namespace routing hint (1), and the proposal verdict waits (4). Carving by prefix would put the proposal waits and the lane reads into `read_index.c` and leave the view, the waiter slots and the follower RPC, which the round cannot run without, in other files. The "Concern" column below is the proposed cut.
2. **The round's support is not prefixed.** 25 functions the ReadIndex path calls or is woken by carry other names (`host_view_*`, `host_waiter_sleep`, `host_waiters_wake`, `host_remote_read_index`, `host_remote_cmd_impl`, ...); see the support table. They and `struct host_view`, `struct host_waiter`, the view fields of `struct host_group` and the waiter slots of `struct efs_raft_host` must go to `host_internal.h` or with the round.
3. **Two header-only helpers are included mid-file.** `read_index_pair_remote.h` is included at L5637 with `HOST_READ_PAIR_LIMIT` and `HOST_READ_PAIR_NOTE` defined just before it and undefined after; it defines four statics and calls `host_read_index`. `publication_session_fast.h` is included at L15308 and calls `host_read_index`. The file that includes each must see `host_read_index` declared.
4. **Text-order anchors in tests** (section "Merge notes"): `test_read_retry_budget.py` slices from `host_remote_cmd_impl` to the forward declaration of `host_wait_applied` at L5009, not to its definition.

## Entry map

Callers are functions in `src/server/raft_host.c` unless a path says otherwise. Every function is `static`. Locks: `h->mu` is the pump's raft-core mutex; `h->cv_mu` guards the waiter slots and `applied_cv`; the published view is a seqlock on `v_seq` read without a lock (`host_view_get`). Header helper means a header-only file in `src/server/` that the README lists.

| Function | Line | Concern | Callers in file | Outside the file | State touched | Header helpers |
|---|---|---|---|---|---|---|
| `host_read_probe` | 1387 | probe transport (outbox) | `host_send` (under `h->mu`) | none | `struct host_outbox` fields `preserve_read`, `read_probe_term`, `read_probe_boot`, `read_probe_context`, `pri`, `npri`; `HOST_OUTBOX_MAX`, `HOST_OUTBOX_BYTES` | none |
| `host_read_wait_ready` | 4484 | round: wake predicate | `host_read_floor_ready` | none | a `struct host_view` copy (`has`, `role`, `term`, `read_pending`, `read_done`) | none |
| `host_read_floor_ready` | 4505 | round: wake predicate | `host_read_index_admitted`, `host_waiters_wake` (under `h->cv_mu`) | none | `struct host_read_floor` (`index`, `context`), view copy | none |
| `host_read_note_begin` | 5353 | round: start | `host_read_begin_tracked` (under `h->mu`) | none | `host_group.ri_begun` (atomic store) | none |
| `host_read_begin_tracked` | 5363 | round: start | `host_read_index_admitted`, `host_read_index_pair`, `host_read_start_requested`, `host_publication_committed` | none | core `efs_raft_read_begin` | none |
| `host_read_admit_view` | 5374 | round: admission (server mode, no `h->mu`) | `host_read_index_admitted`, `host_read_index_pair` | none | `host_group.ri_begun` (atomic load), view copy | none |
| `host_read_request` | 5391 | round: request | `host_read_index_admitted`, `host_read_index_pair` | none | `host_group.ri_request` (CAS), `host_pump_kick` | none |
| `host_read_start_requested` | 5407 | round: start (pump side) | `host_pump`, `host_read_control` (under `h->mu`) | none | `h->g[]`, `host_group.ri_request`, `h->s`, `HOST_NGROUPS`; `host_publish_view` | none |
| `host_read_index_admitted` | 5427 | round: the reader | `host_read_index`, `host_read_index_pair`, `server_raft_host_submit` (proof endpoint, `proof_only=1`) | none | `h->mu` (direct-mode begin), `h->cv_mu` (sleep), `h->s`, `h->raft_id`, `h->n`, `h->telemetry`, `g_read_prof`, `HOST_READ_TRIES`, `HOST_TICK_US`; `g_ri_coal` through `host_remote_read_index` | none |
| `host_read_index` | 5616 (declared 2347) | round: entry | 56 functions: every server_raft_host_* metadata handler that reads rows, the expiry, GC, publication, session and txn paths, `host_read_index_pair`, `host_read_inode_lanes_reuse` | `src/server/read_index_pair_remote.h` (`host_read_pair_run`, `host_read_index_scalar_pair`, `host_read_index_remote_pair`), `src/server/publication_session_fast.h` (`host_session_establish_reuse`) | none | none |
| `host_read_pair_capacity` | 5626 | pair path | `HOST_READ_PAIR_LIMIT` macro, used by `host_read_pair_init` | `src/server/read_index_pair_remote.h` | none | `read_index_pair_remote.h` |
| `host_read_pair_note` | 5630 | pair path | `HOST_READ_PAIR_NOTE` macro, used by `host_read_index_remote_pair` | `src/server/read_index_pair_remote.h` | `g_read_prof` (`pair_parallel`, `pair_scalar`) | `read_index_pair_remote.h` |
| `host_read_index_pair` | 5646 | pair path | `server_raft_host_lookup`, `server_raft_host_mkdir`, `server_raft_host_query_page`, `server_raft_host_query_read`, `host_query_source_once`, `host_append_authority`, `host_read_inode_lanes_mask` | none | `h->mu`, `h->s`, view copies | `read_index_pair_remote.h` (`host_read_index_scalar_pair`, `host_read_index_remote_pair`) |
| `host_wait_applied_mode` | 5740 | apply wait | `host_wait_applied`, `host_wait_applied_own` | none | `h->cv_mu`, `h->apply_progress`, `h->obs_wait_timeouts`, `h->obs_wait_timeouts_own`, `h->obs_wait_extends`, `h->telemetry`, `HOST_READ_TRIES`, `HOST_TICK_US`, `HOST_APPLY_WAIT_CAP_US` | `apply_wait.h` (`efs_apply_wait_start`, `efs_apply_wait_expired`, `efs_apply_wait_remaining_us`) |
| `host_wait_applied` | 5808 (declared 5009) | apply wait: a ReadIndex served behind a follower's apply (one fixed budget) | `host_read_index_admitted` | none | none | none |
| `host_wait_applied_own` | 5814 | apply wait: own committed entry (progress-renewed) | `host_wait_verdict`, `host_wait_settled`, `host_wait_refs`, `host_bg_propose`, `host_pub_batch_wait` | none | none | none |
| `host_wait_verdict` | 6310 | proposal verdict wait | `server_raft_host_submit`, `server_raft_host_unlink`, `host_propose_wait_idx_impl`, `host_create_ex`, `host_truncate_lazy`, `host_bg_propose`, `host_append_execute_atomic`, `host_complete_single_append`, `host_gc_close_retire_batch` | named in the comment of `src/server/apply_wait.h` | apply-result ring (`host_arc_read`, `host_arc_verdict`, `struct efs_arc_view`), `wprof_verdict` | none |
| `host_wait_settled` | 6333 | proposal verdict wait | `server_raft_host_setattr`, `host_utimens`, `host_bg_propose` | none | `host_arc_read`, `h->obs_arc_term_miss` | none |
| `host_read_inode_lanes_reuse` | 7606 | lane reads after a ReadIndex | `host_read_inode_lanes_covered`, `host_read_inode_lanes_mask`, `server_raft_host_lookup_path`, `server_raft_host_readdir`, `host_truncate_lazy` | none | `h->kv`, `struct efs_meta_row`; `host_hosts`, `host_need_both`, `host_pick_peer` | none |
| `host_read_inode_lanes_covered` | 7679 | lane reads | `server_raft_host_lookup`, `server_raft_host_setattr`, `host_utimens` | none | none | none |
| `host_read_inode_lanes_mask` | 7686 | lane reads | `host_read_inode_lanes`, `server_raft_host_getattr` | none | `h->kv`; `host_hosts` | none |
| `host_read_inode_lanes` | 7713 | lane reads | `server_raft_host_link`, `server_raft_host_rename_at`, `server_raft_host_rmdir`, `host_unlink_txn` | named in a comment of `src/client/inode_rpc.c` | none | none |
| `host_read_control` | 14229 | probe transport (inbox read-ACK path) | `server_raft_host_inbox` | none | `h->mu` (trylock), `h->inbox_mu`, `h->inbox_n`, `h->s`; core decode `efs_wire_raft_decode`; `host_waiters_wake`, `host_publish_group` | none |
| `host_read_leader_hint` | 17075 | namespace routing hint | `server_raft_host_getattr`, `server_raft_host_lookup`, `server_raft_host_lookup_path` | none | `h->s`; `host_namespace_leader_hint` | none |
| `host_wait_refs` | 18638 | proposal verdict wait | `host_txn_commit`, `host_append_batch_decide`, `host_append_reserve_decide`, `host_mkdir_alloc_reserve`, `host_pub_unit_abort`, `host_resize_resolve`, `host_wait_refs_each`, `server_raft_host_mkdir`, `server_raft_host_rmdir` | none | `struct host_idx_ref`; `host_apply_rc` | none |
| `host_wait_refs_each` | 24335 | proposal verdict wait | `host_pub_unit_commit` | none | `struct host_idx_ref`; `host_apply_rc` | none |
| `host_read_pair_run` | `src/server/read_index_pair_remote.h:29` | pair path (worker thread) | started by `host_read_pair_init` | header | `host_read_pair_mu`, `struct host_read_pair_worker`, `efs_net_deadline_ms` | the header itself |
| `host_read_pair_init` | `src/server/read_index_pair_remote.h:48` | pair path | `pthread_once` from `host_read_index_remote_pair` | header | `host_read_pair_workers`, `host_read_pair_count`, `HOST_READ_PAIR_WORKERS`, `HOST_READ_PAIR_STACK` | the header itself |
| `host_read_index_scalar_pair` | `src/server/read_index_pair_remote.h:74` | pair path | `host_read_index_pair`, `host_read_index_remote_pair` | header | none | the header itself |
| `host_read_index_remote_pair` | `src/server/read_index_pair_remote.h:80` | pair path | `host_read_index_pair` | header | `host_read_pair_mu`, `host_read_pair_once`, `host_read_pair_workers` | the header itself |

Proposed cut for `read_index.c`: the round, the pair path and the apply wait (17 in `raft_host.c`, with `read_index_pair_remote.h` included there), plus the support functions marked "round" below. The proposal verdict waits go with the proposal code, the lane reads with the inode handlers, the probe transport with the outbox and inbox, the routing hint with the namespace code.

| Support function | Line | Role on the ReadIndex path | Goes with |
|---|---|---|---|
| `host_publish_group` | 4382 | publishes one group's view (callers hold `h->mu`) | pump, declared in host_internal.h |
| `host_view_get` | 4399 | seqlock read of the published view | round, declared in host_internal.h (37 call sites) |
| `host_halted_hint` | 4435 | leader hint for a group stuck below a committed entry | round |
| `host_view_covers` | 4443 | a finished round covers this admission and is applied | round |
| `host_view_quorum_index` | 4456 | proof index for the proof endpoint | round |
| `host_view_wait_apply` | 4472 | quorum done, own apply pending | round |
| `host_waiter_sleep` | 4529 | parks a reader or applier in a waiter slot under `h->cv_mu` | round, declared in host_internal.h |
| `host_waiters_wake` | 4582 | pump side: signals the slots whose predicate the new view satisfies | round, called by the pump |
| `host_group_retired` | 4649 | apply wait refuses on a retired group | membership |
| `host_fwd_leader_pick` | 4689 | leader hint for a retired group | forwarding |
| `host_local_leader` | 4916 | leader from the local view | forwarding |
| `host_remote_cmd_impl` | 4924 | follower RPC to the leader, retries on `EFS_ERR_NOT_PRIMARY` with hint | forwarding |
| `host_remote_cmd` | 4990 | read-only deadline scope around `host_remote_cmd_impl` | forwarding |
| `host_deadline_us` | 5035 | absolute deadline | common |
| `host_past_deadline` | 5046 | deadline check | common |
| `read_prof_on` | 5079 | `g_read_prof` switch | round |
| `read_prof_add` | 5090 | `g_read_prof` accumulate | round |
| `host_ri_complete` | 5194 | follower coalescer: freeze each waiter's first sufficient reply | round |
| `host_ri_remove` | 5210 | follower coalescer: unlink a waiter | round |
| `host_remote_read_index` | 5217 | follower ReadIndex: one leader RPC per round under `g_ri_coal[group >> 1].mu` | round |
| `host_follower_read_result` | 5288 | follower that cannot apply in time redirects to the leader | round |
| `host_group_incompatible` | 4299 | W127: never serve reads from a group stuck below a committed entry | apply |
| `host_pump_kick` | 8076 (declared 5011) | wakes the pump through `pump_efd` | pump |
| `server_raft_host_submit` | 15389 | leader side of a forwarded ReadIndex: `clen == 0` calls `host_read_index_admitted` with `proof_only` | RPC dispatch |
| `read_own_term_committed` | `src/raft/raft.c:572` | a new leader proves no read until it committed an entry in its own term | raft core (not moved) |

Shared state the cut must declare in host_internal.h (the header 2b plans; not in the tree yet) or keep private to `read_index.c`:

| Name | Where | Owner and lock |
|---|---|---|
| `struct host_view` | L4391 | value copy; written by `host_publish_view` under `h->mu`, read lock-free |
| `struct host_group` fields `ri_begun`, `ri_request` | L244 | atomics; `ri_begun` raised under `h->mu` before probes leave |
| `struct host_group` fields `v_seq`, `v_commit`, `v_applied`, `v_read_index`, `v_read_round`, `v_proven_index`, `v_proven_round`, `v_read_context`, `v_stamp` | L339 | published under `h->mu`, seqlock on `v_seq` |
| `struct host_waiter`, `HOST_WAITERS`, `struct host_read_floor` | L375 | under `h->cv_mu` |
| `struct efs_raft_host` fields `cv_mu`, `applied_cv`, `waiters`, `waiter_active`, `waiter_hint`, `cv_overflow` | L521 | `cv_mu` |
| `struct efs_raft_host` fields `mu`, `apply_progress`, `obs_wait_timeouts`, `obs_wait_extends` | L391 | `mu`; the counters relaxed atomics |
| `g_read_prof` | L5077 | anonymous-struct static, relaxed atomics; also used by the profile tick (`read_prof_tick`) |
| `g_ri_coal`, `struct host_ri_coal`, `struct host_ri_wait` | L5180 | per group pair `mu` and `cv`; waiters linked only under `mu` |
| `host_read_pair_mu`, `host_read_pair_once`, `host_read_pair_workers`, `host_read_pair_count` | `src/server/read_index_pair_remote.h` | statics of the header, `host_read_pair_mu` |
| `HOST_READ_TRIES`, `HOST_TICK_US`, `HOST_APPLY_WAIT_CAP_US` | L59–L102 | constants |

| Routed test | What it pins on the ReadIndex path |
|---|---|
| `tests/test_k96_status_slots_live.py` | live cluster; STATUS slots (routed here, names no ReadIndex function) |
| `tests/test_readindex_operation.py` | splices the round; `EFS_ERR_NOT_PRIMARY` on role change |
| `tests/test_readindex_publish.py` | `host_view_get` and the publish order |
| `tests/test_readindex_pair.py` | pair path; splices `host_read_note_begin` to `host_read_start_requested` and reads `read_index_pair_remote.h` |
| `tests/test_readindex_tickets.py` | splices `host_read_note_begin` to `host_read_index_admitted` |
| `tests/test_readindex_admission.py` | `host_read_admit_view` and `host_read_start_requested`: floor, shared next round, refusals (`EFS_ERR_BUSY`, `EFS_ERR_NOT_PRIMARY`) |
| `tests/test_readindex_result.py` | `host_read_index_admitted`: proof floor, follower path through `host_remote_read_index` and `host_wait_applied`, term-change refusal, proof endpoint never forwards |
| `tests/test_readindex_coalescer.py` | `struct host_ri_wait` to the `ReadIndex for one group` comment: the follower coalescer |
| `tests/test_read_waiters.py` | waiter slots and wake predicates |
| `tests/test_readindex_remote_pair.py` | compiles `read_index_pair_remote.h` |
| `tests/test_read_retry_budget.py` | `host_remote_cmd_impl` read budget; slice ends at the `host_wait_applied` forward declaration |
| `tests/test_host_read_deadline.py` | read deadlines |
| `tests/test_read_control_host.py` | `host_read_control` |
| `tests/test_queued_read_control.py` | queued read controls in the inbox |
| `tests/test_raft_pending_reads.py` | core pending-read rounds |
| `tests/test_raft_read_probe.py` | core read probe |
| `tests/test_raft_read_admission.py` | splices `host_read_note_begin` to `host_read_index_admitted` |
| `tests/test_follower_read_redirect.py` | `host_follower_read_result`; asserts the exact text of the remote-authority branch |

All 18 rows run through `make test` and splice the `raft_host` module text through `tests/csrc.py` except `tests/test_k96_status_slots_live.py` (live) and the two that compile `read_index_pair_remote.h`, which splice too. None compiles `raft_host.c` through an interface. `tests/test_follower_read_redirect.py` is the row the Nov 4 baseline (17 rows) did not have.

## Self-check

`python3 scripts/evidence/do/check_entry_map.py` reads the "Entry map" section of this page and fails when a function of the first table is not a `host_read_*`/`host_wait_*` definition in `raft_host.c` or `read_index_pair_remote.h` or one of those is missing from it, when a support function is not defined in `raft_host.c` or `src/raft/raft.c`, when any other backticked name is not a path that exists or an identifier in `raft_host.c`, `raft.c`, `src/server/*.h` or `include/efs/*.h`, and when the test table differs from `tests_for.py server/raft_host.read_index`. Line numbers are not checked. It runs in under 0.2 s and is part of `make docs-check`. Negative controls (a copy of the page): renaming `host_read_admit` to `host_read_admitx` in the first table fails with three stale names (the table misses `host_read_admit`, names a function that is not defined, and names an identifier the sources lack); deleting the `tests/test_follower_read_redirect.py` row fails on that test; changing `g_ri_coal` to `g_ri_coalx` fails on that name.

## Baseline: one unfamiliar investigation (before the split)

Question: **after a leader change, which path answers a metadata read on the old leader and on the new one, and what does a follower wait on before it reads rows?** Answered from `df7d11371` by the method of the [Nov 4 baseline](reading-set-baseline-20261104.md) without this page's map (the map is the "after" aid). This is the before-measurement the split's after-page compares with.

Answer. Every row-reading handler calls `host_read_index` → `host_read_index_admitted`, which reads the published view (`host_view_get`). On the old leader, now a follower (`v.role != EFS_RAFT_LEADER`), it calls `host_remote_read_index`: readers of one group pair share one leader RPC per round (`g_ri_coal`, `host_ri_complete` freezes each waiter's first sufficient reply), sent by `host_remote_cmd_impl`, which re-picks the leader from the local view and follows `EFS_ERR_NOT_PRIMARY` hints within `HOST_READ_TRIES × HOST_TICK_US`. At the new leader, `server_raft_host_submit` answers the empty command with `host_read_index_admitted(..., proof_only=1)`: a non-leader answers `EFS_ERR_NOT_PRIMARY` and never forwards; a leader admits the reader at its commit index for the next round (`host_read_admit_view` in server mode, `host_read_admit` under `h->mu` otherwise), and no round counts as proof until the core has committed an entry in the new term (`efs_raft_read_quorum` → `read_own_term_committed`); a term change after admission refuses with `EFS_ERR_NOT_PRIMARY`. The reply carries the exact proven index. The follower then waits in `host_wait_applied` (own=0) for its own applied index to reach that index: it parks in a waiter slot under `h->cv_mu` (`host_waiter_sleep`, any=0), the pump's `host_waiters_wake` signals it when `v_applied >= idx`, and the budget is one fixed grant (`efs_apply_wait_*`, no progress renewal for reads). On expiry `host_follower_read_result` turns `EFS_ERR_BUSY` into `EFS_ERR_NOT_PRIMARY` with the leader's hint, so the client asks the leader instead.

Entry: `src/server/README.md` says only that ReadIndex is in `raft_host.c`; no symptom map below file level, no ReadIndex test tag (`server/raft_host.read_index` exists in the catalogue but not in the README), "the host's lock order is not stated in one place". The function index came from the plan's 2b prefix and one grep; the support functions (`host_view_*`, `host_waiter_*`, `host_remote_*`, `host_ri_*`) came from reading `host_read_index_admitted`'s callees, the raft-core rule from `graphify explain efs_raft_read_begin`.

| File | Lines | Category | Opened | Needed |
|---|---|---|---|---|
| `src/server/README.md` | 1–54 | guide | 54 | 54 |
| `docs/status/2026-11/structure-plan-20261102.md` | 119–131 (2b) | prefix map | 13 | 13 |
| grep `host_read_*`/`host_wait_*` definitions | 29 hits | navigation | 29 | 29 |
| `graphify explain "server/raft_host.read_index"` | 19 edges | navigation: the same 18 tests as `tests_for.py` | 22 | 0 |
| `graphify query` (broad, below) | 644 nodes, 61 shown | navigation | 30 | 0: dead end |
| `graphify explain efs_raft_read_begin` | 22 edges | navigation to `src/raft/raft.c` | 12 | 12 |
| `src/server/raft_host.c` | 244–262 | state: `ri_begun`, `ri_request` | 19 | 7 |
| `src/server/raft_host.c` | 336–390 | state: view fields, `struct host_waiter` | 55 | 32 |
| `src/server/raft_host.c` | 510–540 | locks: `cv_mu`, `applied_cv`, waiter slots | 31 | 20 |
| `src/server/raft_host.c` | 4391–4432 | state: `struct host_view`, `host_view_get` | 42 | 42 |
| `src/server/raft_host.c` | 4433–4440 | `host_halted_hint` | 8 | 0 |
| `src/server/raft_host.c` | 4441–4519 | predicates: covers, quorum index, wait apply, wait ready, floor ready | 79 | 79 |
| `src/server/raft_host.c` | 4520–4631 | waiting: `host_waiter_sleep`, `host_waiters_wake` | 111 | 111 |
| `src/server/raft_host.c` | 4916–5007 | follower RPC: `host_local_leader`, `host_remote_cmd_impl`, `host_remote_cmd` | 92 | 92 |
| `src/server/raft_host.c` | 5165–5297 | coalescer, `host_remote_read_index`, design comment, `host_follower_read_result` | 133 | 133 |
| `src/server/raft_host.c` | 5299–5324 | admission: `host_read_admit` | 26 | 26 |
| `src/server/raft_host.c` | 5325–5345 | `host_read_probe_needed` (pair path) | 21 | 0: dead end |
| `src/server/raft_host.c` | 5346–5426 | round start: note/begin/admit_view/request/start_requested | 81 | 81 |
| `src/server/raft_host.c` | 5427–5619 | `host_read_index_admitted`, `host_read_index` | 193 | 193 |
| `src/server/raft_host.c` | 5733–5818 | `host_wait_applied_mode` and its two wrappers | 86 | 86 |
| `src/server/raft_host.c` | 15389–15440 | `server_raft_host_submit` entry (fault hooks not needed) | 52 | 30 |
| `src/server/raft_host.c` | 16015–16075 | the `clen == 0` proof branch (7 lines) among the session and proposal branches | 61 | 7 |
| `src/raft/raft.c` | 568–590 | `read_own_term_committed` | 23 | 18 |
| `src/raft/raft.c` | 3280–3345 | `efs_raft_read_begin` | 66 | 48 |
| `src/raft/raft.c` | 3503–3566 | `efs_raft_read_quorum`, `efs_raft_read_covers` (`efs_raft_read_proven` not needed) | 64 | 17 |
| `src/server/apply_wait.h` | 1–70 | the follower's fixed budget (own=0) | 70 | 70 |

Understand: **needed 1,200, opened 1,473.** Categories (needed): guide 67, navigation 41, shared state and locks 212, implementation 727, raft core 83, header helper 70. Source files: 3 (`raft_host.c`, `raft.c`, `apply_wait.h`) plus the README and the plan. Whole-file upper bound: 26,262 + 3,566 + 70 + 54 = 29,952 lines.

| File | Lines | Category | Opened | Needed |
|---|---|---|---|---|
| `python3 scripts/tests_for.py server/raft_host.read_index` | 18 rows | routing | 18 | 18 |
| grep of the 18 tests for the path's names | 18 lines | navigation: 5 tests name a function on this path | 18 | 18 |
| `tests/test_follower_read_redirect.py` | 1–26 | the redirect and the remote-authority branch | 26 | 26 |
| `tests/test_readindex_result.py` | 1–112 | follower path, term refusal, proof endpoint refusal | 112 | 112 |
| `tests/test_readindex_coalescer.py` | 1–12 | the coalescer slice (assertions not read) | 12 | 0 |
| `tests/test_read_retry_budget.py` | grep, 12 lines | `host_remote_cmd` read budget | 12 | 12 |
| `tests/csrc.py` | 1–60 | module contract (1–24) | 60 | 24 |

Validate: **210 needed, 258 opened**; 4 of 18 routed tests opened. All splice by text; none compiles `raft_host.c`. Nothing routed covers an actual leader change on a cluster; `test_readindex_result.py` simulates it with a stubbed view.

Dead ends: the broad graph query, `host_read_probe_needed`, the session and proposal branches of `server_raft_host_submit`, `efs_raft_read_proven`.

Against the target (one guide, one interface, one implementation area, the routed tests): guide no (file level only), interface no, implementation area spread over 14 ranges of one 26,262-line file plus the raft core, routed tests yes but by text.

| Command | Nodes | Useful |
|---|---|---|
| `graphify query "ReadIndex read after a leader change follower wait host_read host_wait"` | 644 (61 shown) | no: start nodes `change()`, `leader()`, `wait`, `read()` in tests and `src/raft/raft.c` |
| `graphify explain "server/raft_host.read_index"` | 1, 19 edges | the routed tests only; no function edges |
| `graphify explain efs_raft_read_begin` | 1, 22 edges | yes, once the name is known |

What the split's after-page must show: the same question entered from `src/server/README.md` and `src/server/raft_host/read_index.c` with `host_internal.h`, the follower RPC and the waiter slots reachable without a grep, and lines needed compared with 1,200 here; the 2b target is under 3,000 lines read in all against 26,262 today.

## Merge notes for the split

- **What must not change.** Each commit moves code and changes `static` to internal linkage, nothing else (2b): no change to which lock a function takes or holds (`h->mu`, `h->cv_mu`, `g_ri_coal[].mu`, `host_read_pair_mu`), no reordering inside a function (the seqlock in `host_view_get`; `ri_begun` raised before `efs_raft_read_begin`; registration under `cv_mu` after the re-read of the view, which is what rules out a lost wakeup in `host_waiter_sleep`), no change to the `HOST_READ_PAIR_LIMIT`/`HOST_READ_PAIR_NOTE` overrides around `#include "read_index_pair_remote.h"`. Task DL's deviations (a few `static inline` helpers, dropped redundant prototypes) were each listed on its page; do the same here.
- **Tests that splice `raft_host.c` text.** `grep -l raft_host tests/*.py tests/csrc.py` lists 138 files; 108 read `csrc.module('raft_host')`. `tests/csrc.py` maps a module name to its files in a fixed order and its docstring states the contract: `str.index` takes the first match, so after the split each function's first occurrence must stay unique and a definition must come before any text searched for after it. `MODULES['raft_host']` becomes the ordered list of `src/server/raft_host/` files; choose the order against these users. At least 33 tests slice from one anchor to another function's anchor. On the ReadIndex path: `test_read_retry_budget.py` (`host_remote_cmd_impl` to the forward declaration `\nstatic int host_wait_applied(` at L5009: dropping or moving that declaration silently changes the slice), `test_raft_read_admission.py` and `test_readindex_tickets.py` (`host_read_note_begin` to `host_read_index_admitted`), `test_readindex_pair.py` (`host_read_probe_needed` to `host_read_start_requested`), `test_readindex_coalescer.py` (`struct host_ri_wait {` to the comment `/* ReadIndex for one group`), `test_lookup_read_group.py` (`host_read_inode_lanes_reuse` to `/* Process a bounded FIFO prefix`), and `test_follower_read_redirect.py` asserts the exact whitespace of two lines of `host_read_index_admitted`. Nine tests besides `tests/csrc.py` name the path `src/server/raft_host.c` directly; `test_host_batch_handoff.py` also reads it by `git show <ref>:src/server/raft_host.c` and breaks for a ref after the split.
- **TSan.** Run the F2 lane, `make tsan-check`, on the split result: making functions visible across files changes inlining, so the instruction stream is no longer the proof it was for the `meta_apply.c` split with inlining off. Its list already has `readindex_pair` and the daemon entries (`d4_progress`, `k96_status_slots_live`, `k36_recovery`) that run `efsd` under load; Task DL's run is the shape (22 of 22, 0 TSan reports in the routed binaries).
- **Still blocked.** The map and this baseline meet the Task DO precondition only. 2b also waits on the `devel-w123-recovery` merge, which is the user's decision and still open, and runs between waves while no branch touches `raft_host.c`.

## Reproduce

Any checkout at `df7d11371`, no cluster, no build:

```
python3 scripts/evidence/do/check_entry_map.py
python3 scripts/tests_for.py server/raft_host.read_index
grep -nE "^[a-z].*\b(host_read_[a-z0-9_]*|host_wait_[a-z0-9_]*)\(" src/server/raft_host.c | grep -v ";$"
grep -nE "^static .*host_read_[a-z_]+\(|^static .*host_read_index_[a-z_]+\(" src/server/read_index_pair_remote.h
graphify explain "server/raft_host.read_index"
graphify explain efs_raft_read_begin
graphify query "ReadIndex read after a leader change follower wait host_read host_wait"
grep -l raft_host tests/*.py tests/csrc.py | wc -l
grep -l "module('raft_host')" tests/*.py | wc -l
```

The baseline ranges are the table rows above; read them with `sed -n A,Bp` on the same commit.

## Raw evidence

None: every number on this page comes from the tree at `df7d11371` and the commands above. No host was used; no capture exists.

## Not done

- No build, no `tests_for.py --run`, no host run (fstor007 not used): nothing on the page needed one.
- The split itself, its `host_internal.h`, the after-measurement, and the `make tsan-check` lane on the result (2b).
- The `devel-w123-recovery` merge decision (the user's).
- The Concern column is a proposal from reading; the split's author decides the cut.
