# src/server/ — server — efsd

## Files

- `efsd.c` — daemon entry, startup/stop; `cluster.c` — cluster formation and membership wiring; `discovery.c/.h` — local-subnet discovery (never defines membership).
- `raft_host.c` — the production Raft host (hosted groups, apply pump, ReadIndex, publication, GC, sessions, transactions); owns every hosted group's state. **The file the [W171 plan](../../docs/status/2026-11/structure-plan-20261102.md) splits next** (the split waits for W123). ReadIndex code (`host_read_*`, `host_wait_*`, the view, the waiter slots, the follower RPC): [entry map](../../docs/status/2026-11/readindex-entry-map-20261108.md#entry-map), checked by `make docs-check`.
- `read_proof.c/.h` — the ReadIndex request side: admission, request, wait for the quorum proof, loss of leadership (Task V4); contract in `read_proof.h`, guide below.
- `group_view.c/.h` — the published group view and its sleepers (what handlers read without `h->mu`, and the wait bracket); extracted from `raft_host.c` in Task V3; contract in `group_view.h`, guide below.
- Concern map of `raft_host.c` (every function under one concern, with state, mutex, entry points, logs and tests; the lock order): [raft-host-map.md](../../docs/how-it-works/raft-host-map.md), checked by `make docs-check`. Start there for a defect in the file.
- `handler.c` — request dispatch per connection; `thread.c` — conn/writer thread stacks; `writer.c` — writer pool; `peer_pool.c` — persistent server→server connections.
- `server_internal.h` — shared `g_server`-style state, export lifetime (use-count guarded by `s->lock`); the anti-example in developing.md.
- `member_store.c/.h` — durable boot counter, member identity and committed membership under `<meta root>/mdraft`.
- `store.c`, `store_nvme.c` — NVMe fragment store and its `efs/store.h` backend; `storage_roots.c/.h` — exclusive root locks, root marker, ancestry barrier.
- `put_upload.c`, `put_fence.c`, `put_orphan.c` — the one data PUT path (ticket-admitted), stripe certificates, orphan fragment delete.
- `packer.c`, `segment_store.c`, `segment_rebuild.c` — W125 packing, packed segment store, rebuild on return (K37).
- `reclaim.c` — W124 node reclaim; `recovery.c` — verified recovery after an unclean stop (K36).
- `iostats.c` — always-on relaxed-atomic I/O counters.
- Header-only helpers: `apply_ring.h`, `apply_wait.h` (W159), `dest_fault.h` (test hook), `directory_sync_group.h`, `publication_session_fast.h`, `query_export_lease.h`, `query_refresh.h`, `read_index_pair_remote.h`, `write_prof.h` — state owned by the includer.

**Symptom to concern (`raft_host.c`).**

Section names of [raft-host-map.md](../../docs/how-it-works/raft-host-map.md); log keys are listed in [diagnostics](../../docs/how-it-works/diagnostics.md).

| Symptom | Concern |
| --- | --- |
| `raft-host: applied ...` / `raft-host: apply ...` line for a command | Apply: core, then the Apply section of that command |
| a group stuck below a committed entry (incompatible) or missing a row (`kv_incomplete`) | Apply: core |
| `txn-fault`, `raft-host: decide-lost fault` | Transaction coordination |
| `raft-host: txn-recover`, `raft-host: txn-finish`, `raft-host: txn-commit` | Transaction recovery and finisher; Transaction coordination |
| `append-abandon`, append reservation or catch-up refusal | Append |
| `namespace-prof`, `raft-host: opid-replay`, `raft-host: rmdir`, `raft-host: unlink-txn` | Namespace operations |
| `query-catchup`, `query-config`, `query-publish`, `query-recovery`, `query-refresh`, `query-restore`, `query-revisions` | Query service |
| `raft-snap`, snapshot import BUSY, apply BUSY during an import | Snapshots |
| `read-prof`, `raft-host: read-sleep` | Encoding helpers and diagnostic switches; ReadIndex round |
| read returns BUSY or NOT_PRIMARY, applied-wait timeout, stale follower read | ReadIndex round; Group view, waiters and leader state |
| `report-receipt`, `report-split`, `raft-host: pub-reject`, `raft-host: report pub` | Publication and REPORT |
| `storage-fence`, `raft-host: session cleanup batch`, session UNREGISTERED or FENCED, lock waiter never granted | Sessions, leases and lock waits |
| `raft-host: formed`, `raft-host: raft-mkfs`, `raft-host: membership notice`, HELLO refused | Formation and member join; Membership view and group configuration |
| `raft-host: gc reap`, `raft-host: gc watermark`, fragment delete or reclaim stalls | Reclaim and GC thread |
| `raft-host: rebuild`, `raft-host: rebuilt`, degraded mark not cleared | Degraded-fragment rebuild |
| `raft-host: put-orphan`, `raft-host: put-ticket drain` | Put-orphan and put-drain passes |
| `raft-host: recovery-mark`, recovery scan or resync refused | Recovery scan and resync |
| pump stall, inbox full, late heartbeat, `raft-host: shutdown durability failed` | Pump, inbox and applied checkpoint; Outbox and sender threads |
| `status` field wrong or missing | Observability and status |

## Contract

- Interface: [`src/server/server_internal.h`](server_internal.h) (private to `server/`); public seams are `efs/store.h`, `efs/transport.h`, `efs/protocol.h`.
- Invariants: I1–I4, I9, I10, I14, I16, I17, I20–I24 are enforced here per [architecture §4](../../docs/how-it-works/architecture.md#4-invariants) and §7.
- Lock ownership: export lifetime is stated in `server_internal.h`; the member-store identity (`s->mstore`) is written and read under the host's `h->mu` once the host runs, other threads read it through `server_member_identity` (K93, `member_store.h`); the host's lock order is not stated in one place.
- Apply visibility of `host_apply`'s scope (staged, committed, visible): [apply-visibility.md](../../docs/how-it-works/apply-visibility.md).
- Split rules and the `raft_host.c` prefix map: [W171 plan](../../docs/status/2026-11/structure-plan-20261102.md).

## Calls

- May call: `raft/`, `kv/`, `meta/`, `wire/`, `data/`, `common/` through `efs/*.h` ([developing.md](../../docs/how-it-works/developing.md)).
- Called by: `bench/` (observed private include `server/server_internal.h`) and tests; nothing else should include server internals.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py server/apply_wait`
- `python3 scripts/tests_for.py server/cluster`
- `python3 scripts/tests_for.py server/directory_sync_group`
- `python3 scripts/tests_for.py server/efsd`
- `python3 scripts/tests_for.py server/group_view`
- `python3 scripts/tests_for.py server/handler`
- `python3 scripts/tests_for.py server/member_store`
- `python3 scripts/tests_for.py server/packer`
- `python3 scripts/tests_for.py server/put_fence`
- `python3 scripts/tests_for.py server/put_orphan`
- `python3 scripts/tests_for.py server/put_upload`
- `python3 scripts/tests_for.py server/query_refresh`
- `python3 scripts/tests_for.py server/raft_host`
- `python3 scripts/tests_for.py server/raft_host.read_index`
- `python3 scripts/tests_for.py server/read_proof`
- `python3 scripts/tests_for.py server/reclaim`
- `python3 scripts/tests_for.py server/server_internal`
- `python3 scripts/tests_for.py server/storage_roots`
- `python3 scripts/tests_for.py server/store`
- `python3 scripts/tests_for.py server/writer`

**Read proof (`read_proof.c`, `read_proof.h`).** Owns the ReadIndex request side of every hosted group: the per-group `ri_begun` and `ri_request`, the follower coalescer, the profile counters, admission against the published view, the request, the sleep for the quorum proof and the follower ask. `raft_host.c` embeds one `struct read_proof`, keeps `host_read_index` and `host_read_index_pair` as thin wrappers, and answers the six functions in [`read_proof_host.h`](read_proof_host.h) (gate, remote call, pump kick, applied wait, profile switch and add); `scripts/check_group_view_privacy.py` fails if another file names the owner's private fields. Contract (state table S0 to S6, S8, S9; there is no S7; locks; publication) in [`read_proof.h`](read_proof.h); `read_proof_impl.h` is private. Interface test: [`tests/test_read_proof.c`](../../tests/test_read_proof.c) (links the real `read_proof.c` and `group_view.c` over a fake Raft core; `--list` prints the cases). Page: [read-proof-ownership-20261111.md](../../docs/status/2026-11/read-proof-ownership-20261111.md).

| State or invariant | Cases | Command |
|---|---|---|
| The gate refuses a damaged or incompatible group with NOT_PRIMARY and the host's hint (S0) | `test_gate_refuses_incompatible_group` | `make tests/test_read_proof && ./tests/test_read_proof test_gate_refuses_incompatible_group` |
| A gate that refuses on a later pass ends the request there (S0) | `test_gate_refuses_on_a_later_pass` | `make tests/test_read_proof && ./tests/test_read_proof test_gate_refuses_on_a_later_pass` |
| A group with no replica is refused (S0) | `test_no_replica_refuses` | `make tests/test_read_proof && ./tests/test_read_proof test_no_replica_refuses` |
| A follower asks the leader it sees (`-1` allowed), then the host's applied wait (S1, S9) | `test_follower_asks_leader_and_applies` | `make tests/test_read_proof && ./tests/test_read_proof test_follower_asks_leader_and_applies` |
| A proof-only request on a follower is refused and never forwards (S1) | `test_follower_proof_endpoint_never_forwards` | `make tests/test_read_proof && ./tests/test_read_proof test_follower_proof_endpoint_never_forwards` |
| A follower whose leader is itself is refused (S1) | `test_follower_leader_is_self_refuses` | `make tests/test_read_proof && ./tests/test_read_proof test_follower_leader_is_self_refuses` |
| An applied wait that ends BUSY becomes NOT_PRIMARY with the hint of another member, stays BUSY for itself, a non-member or a failed RPC (S9) | `test_follower_busy_apply_names_another_leader` | `make tests/test_read_proof && ./tests/test_read_proof test_follower_busy_apply_names_another_leader` |
| Admission takes the view's commit and the next round (S2) | `test_admission_takes_view_commit_and_next_round` | `make tests/test_read_proof && ./tests/test_read_proof test_admission_takes_view_commit_and_next_round` |
| A round counter at its limit is BUSY (S2) | `test_admission_overflow_is_busy` | `make tests/test_read_proof && ./tests/test_read_proof test_admission_overflow_is_busy` |
| A round begun but not yet in the view is waited for: the reader needs the round after `ri_begun` (S2) | `test_begun_round_ahead_of_the_view_is_waited_for` | `make tests/test_read_proof && ./tests/test_read_proof test_begun_round_ahead_of_the_view_is_waited_for` |
| A proof that does not reach the commit seen on arrival is not enough (S2, S5) | `test_proof_must_cover_the_arrival_commit` | `make tests/test_read_proof && ./tests/test_read_proof test_proof_must_cover_the_arrival_commit` |
| A new term seen on the admission pass is refused at once with the hint of the older copy (S2) | `test_new_term_on_the_admission_pass_refuses_with_the_old_hint` | `make tests/test_read_proof && ./tests/test_read_proof test_new_term_on_the_admission_pass_refuses_with_the_old_hint` |
| A role change in the same term is acted on at the next pass (S2, S1) | `test_role_change_in_the_same_term_is_acted_on_at_the_next_pass` | `make tests/test_read_proof && ./tests/test_read_proof test_role_change_in_the_same_term_is_acted_on_at_the_next_pass` |
| A request is raised once per round and kicks the pump once (S3) | `test_request_is_raised_once_and_kicks` | `make tests/test_read_proof && ./tests/test_read_proof test_request_is_raised_once_and_kicks` |
| `read_proof_start_requested` begins a round only after the previous quorum is done, then publishes (S4) | `test_start_requested_begins_when_previous_quorum_done` | `make tests/test_read_proof && ./tests/test_read_proof test_start_requested_begins_when_previous_quorum_done` |
| It skips groups that are not hosted, in flight or not led (S4) | `test_start_requested_skips_unled_and_unhosted` | `make tests/test_read_proof && ./tests/test_read_proof test_start_requested_skips_unled_and_unhosted` |
| A failed begin publishes nothing and is retried (S4) | `test_failed_begin_publishes_nothing_and_is_retried` | `make tests/test_read_proof && ./tests/test_read_proof test_failed_begin_publishes_nothing_and_is_retried` |
| A read returns OK once applied reaches the frozen floor (S5) | `test_read_returns_ok_once_applied_reaches_the_floor` | `make tests/test_read_proof && ./tests/test_read_proof test_read_returns_ok_once_applied_reaches_the_floor` |
| With no read context the covering-round test applies and waits for apply (S5) | `test_read_covered_by_round_waits_for_apply` | `make tests/test_read_proof && ./tests/test_read_proof test_read_covered_by_round_waits_for_apply` |
| A frozen floor survives a later round (S5) | `test_frozen_floor_survives_a_later_round` | `make tests/test_read_proof && ./tests/test_read_proof test_frozen_floor_survives_a_later_round` |
| A proof-only read returns the quorum index before apply (S5) | `test_proof_only_returns_quorum_index_before_apply` | `make tests/test_read_proof && ./tests/test_read_proof test_proof_only_returns_quorum_index_before_apply` |
| A changed read context or term is refused (S5) | `test_context_change_refuses`, `test_term_change_refuses` | `make tests/test_read_proof && ./tests/test_read_proof test_context_change_refuses` ; `make tests/test_read_proof && ./tests/test_read_proof test_term_change_refuses` |
| A reader sleeps in its slot, stays asleep through a publish that changes nothing it waits for, and wakes on one that does (S6) | `test_waits_in_slot_and_wakes_on_publish` | `make tests/test_read_proof && ./tests/test_read_proof test_waits_in_slot_and_wakes_on_publish` |
| A replica that disappears from the view ends the sleeper with NOT_PRIMARY (a view with `has` 0; the host never publishes that, see below) (S6) | `test_replica_gone_while_asleep` | `make tests/test_read_proof && ./tests/test_read_proof test_replica_gone_while_asleep` |
| A reader past its deadline gets BUSY (waits the product's 400 ms once) (S5, S8) | `test_deadline_returns_busy` | `make tests/test_read_proof && ./tests/test_read_proof test_deadline_returns_busy` |
| Leadership lost while the request is raised: a read asks the new leader, the proof endpoint is refused (S3) | `test_loses_leadership_after_admission` | `make tests/test_read_proof && ./tests/test_read_proof test_loses_leadership_after_admission` |
| Leadership lost before the pump starts the round: the pump skips the core, the reader is refused (S4) | `test_loses_leadership_after_request` | `make tests/test_read_proof && ./tests/test_read_proof test_loses_leadership_after_request` |
| Leadership lost while the finished round is published but apply is owed: a read asks the new leader (S5) | `test_loses_leadership_while_checking_with_floor` | `make tests/test_read_proof && ./tests/test_read_proof test_loses_leadership_while_checking_with_floor` |
| Leadership lost while asleep: a read asks the new leader (S6) | `test_loses_leadership_while_asleep` | `make tests/test_read_proof && ./tests/test_read_proof test_loses_leadership_while_asleep` |
| A replica that becomes the leader during the remote call is not looked at again; the RPC's result and the applied wait stand (S9) | `test_leadership_changes_while_asking_leader` | `make tests/test_read_proof && ./tests/test_read_proof test_leadership_changes_while_asking_leader` |
| `read_proof_read_pair`: a group without a replica falls back to the scalar path | `test_pair_without_a_replica_uses_the_scalar_path` | `make tests/test_read_proof && ./tests/test_read_proof test_pair_without_a_replica_uses_the_scalar_path` |
| `read_proof_read_pair`: two followers ask both leaders | `test_pair_of_followers_asks_both_leaders` | `make tests/test_read_proof && ./tests/test_read_proof test_pair_of_followers_asks_both_leaders` |
| `read_proof_read_pair`: the led group is requested before the other group's remote call | `test_pair_requests_the_led_group_before_asking_the_other` | `make tests/test_read_proof && ./tests/test_read_proof test_pair_requests_the_led_group_before_asking_the_other` |
| Host stop wakes no sleeper once the pump is joined; the host's own remote call and applied wait; lock order under the real pump; the follower coalescer under concurrency | none (needs the host) | `python3 scripts/tests_for.py server/read_proof` |

**Group view (`group_view.c`, `group_view.h`).** Owns each hosted group's published view (a sequence-locked copy of commit, applied, term, role, leader, read round and proof) and the table of 512 sleeper slots with their condition variables, the locks `view_mu` and `cv_mu` and the shared `applied_cv`. `raft_host.c` embeds one `struct group_views` and never names those fields (`scripts/check_group_view_privacy.py` fails the fast suite if it or any other file does). Contract in [`group_view.h`](group_view.h) (the file a caller reads; `group_view_impl.h` holds the representation and the inline bodies, is private, and is included only by `group_view.h`): lock order `h->mu`, then `cv_mu`, then `view_mu`; `host_view_get` is a coherent copy with no lock held on return, and after 64 odd sequence loads it waits on `view_mu` (K107); the three waits have three different predicates, so the owner exposes the lock bracket `group_view_wait_lock` / `host_view_get` / the caller's test / `host_waiter_sleep` / `group_view_wait_unlock` and no single wait operation. Interface test: [`tests/test_group_view.c`](../../tests/test_group_view.c) (links the real `group_view.c` over stub core getters; `--list` prints the cases). Page: [group-view-ownership-20261111.md](../../docs/status/2026-11/group-view-ownership-20261111.md).

| Operation or invariant | Cases | Command |
|---|---|---|
| `host_publish_view` with an unchanged core leaves the stamp (and the sequence) alone | `test_unchanged_publish_keeps_stamp` | `make tests/test_group_view && ./tests/test_group_view test_unchanged_publish_keeps_stamp` |
| A changed publish is seen whole with every field, and moves the stamp once | `test_changed_publish_moves_stamp_and_shows_fields` | `make tests/test_group_view && ./tests/test_group_view test_changed_publish_moves_stamp_and_shows_fields` |
| An absent replica (NULL core) clears `has` and the proof fields; the stamp moves only if `has` was set | `test_absent_replica_clears_has_and_proof` | `make tests/test_group_view && ./tests/test_group_view test_absent_replica_clears_has_and_proof` |
| An unknown group reads as absent with leader -1; a never-published group reads as absent (test `has` before `leader`) | `test_unknown_group_reads_as_absent` | `make tests/test_group_view && ./tests/test_group_view test_unknown_group_reads_as_absent` |
| `host_view_get` racing a publisher never returns a mixed round, term and completion (count-bounded) | `test_reader_never_sees_a_mixed_publish` | `make tests/test_group_view && ./tests/test_group_view test_reader_never_sees_a_mixed_publish` |
| The pure predicates `host_view_covers`, `host_view_wait_apply`, `host_view_quorum_index`, `host_read_wait_ready` | `test_predicates` | `make tests/test_group_view && ./tests/test_group_view test_predicates` |
| `host_read_floor_ready` for wait mode 3: the floor freezes the first proof; a changed context refuses; an idle round is not ready | `test_floor_ready_mode_3` | `make tests/test_group_view && ./tests/test_group_view test_floor_ready_mode_3` |
| The wait bracket with a true predicate returns at once and leaves `cv_mu` free | `test_bracket_true_predicate_returns_at_once` | `make tests/test_group_view && ./tests/test_group_view test_bracket_true_predicate_returns_at_once` |
| A publish before the bracket is seen by the re-read under the lock (no lost wakeup, single-threaded) | `test_publish_before_bracket_is_seen_no_sleep` | `make tests/test_group_view && ./tests/test_group_view test_publish_before_bracket_is_seen_no_sleep` |
| `host_waiter_sleep` with a passed deadline leaves no slot occupied and the floor as it was | `test_passed_deadline_sleep_leaves_no_slot` | `make tests/test_group_view && ./tests/test_group_view test_passed_deadline_sleep_leaves_no_slot` |
| `host_waiters_wake` with no sleeper changes nothing | `test_wake_without_sleeper_changes_nothing` | `make tests/test_group_view && ./tests/test_group_view test_wake_without_sleeper_changes_nothing` |
| A blocked mode-0 sleeper is counted by `group_view_waiting` (the reaper's yield test) and woken by a publish that satisfies it | `test_blocked_sleeper_is_counted_and_woken` | `make tests/test_group_view && ./tests/test_group_view test_blocked_sleeper_is_counted_and_woken` |
| The wake of a blocked sleeper (modes 0 and 1), the signal after `cv_mu` is released, the fall-back to `applied_cv` when all 512 slots are busy | none (needs another thread known to be blocked) | `python3 scripts/tests_for.py server/group_view` |

## Open findings

- [W171](../../docs/status/work-queue.md) — this component's split (`raft_host.c`); [plan](../../docs/status/2026-11/structure-plan-20261102.md).
- [W159](../../docs/status/work-queue.md) — expiry-fixture POSIX run timed out on metadata RPCs of 2–7 s; its row names the raft host.
