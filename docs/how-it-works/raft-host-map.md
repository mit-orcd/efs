# Raft host map: `src/server/raft_host.c`

Concern map of `src/server/raft_host.c` (26,277 lines at the time of writing; 655 definition lines matched by the checker, 641 distinct names, because 14 names also have a forward declaration line). Each function of the file is listed under exactly one concern; `scripts/check_concern_map.py` (run by `make docs-check`) fails when a function is missing, unknown or in two concerns. There are no line numbers here: find a function with `grep -n "^static.* <name>(" src/server/raft_host.c`. For a defect, read the Logs line first (log keys come from [diagnostics](diagnostics.md)), then open the concern. Functions are grouped by what they do, not by name prefix: the ReadIndex round, the pair path and the apply wait are one concern; the other `host_read_*`/`host_wait_*` functions sit with the concern they serve (see the [ReadIndex entry map](../status/2026-11/readindex-entry-map-20261108.md)). Apply visibility (staged, committed, visible) is in [apply-visibility.md](apply-visibility.md). Header-only helpers are not checked here: `apply_ring.h`, `apply_wait.h` and `write_prof.h` are included at the top, `read_index_pair_remote.h` (ReadIndex round), `query_refresh.h` and `query_export_lease.h` (Query service) and `publication_session_fast.h` (Sessions, leases and lock waits; it defines `host_session_establish_reuse`) mid-file, and the concern of the includer owns their state. Entry points listed first are the non-static functions (called from other files); "In file" names the static functions other concerns call.

Structs and the host mutexes are declared at the top of the file (`struct efs_raft_host`, `struct host_group`, `struct host_outbox`). Mutexes: `h->mu` (the raft cores, apply, the KV writer scope; held by the pump), `view_mu` and `cv_mu` (inside `h->gv`, owned by `group_view.c`: the leaf for the atomic stores of `host_publish_view`; the waiter slots and `applied_cv`), `h->snap_mu` (snapshot files and import state), `h->outbox_mu` (all `tx[]` queues), `h->inbox_mu` (inbox FIFO), `h->mb_mu` (membership thread state), `h->wait_mu` (lock wait queue), `h->rec_mu` (stranded-transaction marks), `h->fin_mu` (finisher ring), `h->query_lock` (rwlock, published query view), `h->s->lock` (server node table), `mview_mu` (global membership view), `query_page_mu`, `host_report_session_parallel`, `host_append_locks[]`, `g_ri_coal[].mu`, and the worker mutexes `w->mu` of the KV and checkpoint workers. The order is in the last section.

## Encoding helpers and diagnostic switches

Does: Big-endian read and write helpers for packed commands, the wall and monotonic clocks, the salt generator, and the environment switches that gate diagnostics (`EFS_RAFT_DBG`, `EFS_RAFT_OBS`, write and read profiling) so success-path logging costs nothing on the pump thread. `dirop_fail_on` decides which directory-operation errors are worth a log line.

State: `g_read_prof` (`struct read_prof_acc` counters, updated with relaxed atomics in `read_prof_add`, no mutex); `g_wprof[]` and `g_wprof_ticks` (histograms updated through `efs_wprof_add` in `write_prof.h`; their atomicity is not determined here). The environment switches hold no state of their own that this page determined.

Entry points: none public.

Functions: `wr32be` `rd32be` `wr64be` `rd64be` `env_on` `raft_dbg_on` `write_prof_on` `now_us_` `dirop_fail_on` `raft_obs_on` `read_prof_on` `read_prof_add` `read_prof_fmt` `read_prof_tick` `make_salt` `now_ns`

Logs: `read-prof` (read_prof_tick)

Tests: `tests/test_lookup_replacement.py` `tests/test_readindex_result.py` `tests/test_apply_unsupported.py`

## Membership view and group configuration

Does: Holds the process-wide published membership (`mview`, a copy of the newest committed revision) and turns it into per-group configuration: which voters each group has at the committed count, the boot voters of a replica, attach of a replica or group, the persisted operator-desired configuration, the retire check that stops a no-longer-voting replica from campaigning, and the learning and reconciling of a later committed revision from the node's own KV.

State: `mview`, `mview_formed`, `mview_state`, `mview_cluster`, `mview_prop` under `mview_mu`; `h->members` (newest committed revision, under `h->mu`); `host_group.retired` (under `h->mu`, per its field comment); `host_group.voters` (stored and loaded with acquire/release atomics, read by `host_route_voters`); `host_group.hosted` (set at attach; lock not determined); `g_host` (stored with release at start in `server_raft_host_start`, loaded with acquire by `server_member_identity`, `membership_copy` and the query/publication handlers).

Entry points: `server_member_identity` `server_membership_get` `server_member_view`

Functions: `group_voters` `host_boot_voters` `mview_publish` `mview_publish_store` `membership_copy` `server_member_identity` `hosts_group` `host_group_retired` `desired_path` `load_desired` `save_desired` `attach_replica` `attach_group` `host_cfg_fan_note` `host_cfg` `host_retire_check` `host_learn_membership` `host_membership_reconcile` `server_membership_get` `server_member_view`

Logs: `raft-host` lines from `host_retire_check`, `host_learn_membership`, `host_membership_reconcile`

Tests: `tests/test_membership.c` `tests/test_member_view.c` `tests/test_host_retire.py` `tests/test_host_cover_route.py` `tests/test_k33_bookkeeping.py`

## Formation and member join

Does: Brings a new cluster to its first committed membership and admits later members. The seed collects the joining stores' promises, the group-0 leader forms (MKFS on group 0, the FORMED notice to every member, SALT on group 2); a membership thread (`host_member_thread`) reconciles voters, retries notices and enqueues joiners; HELLO from a sender with a node id and a joining store's HELLO are answered here.

State: `h->mb_*` fields (acks, term, forced flag, join queue, wake) under `h->mb_mu`; the membership record reached through `mview_mu`; `h->members` under `h->mu`; the node table under `h->s->lock`.

Entry points: `server_raft_host_mkfs` `server_member_hello` `server_member_hello_join` `server_member_filter`

Functions: `host_test_exit` `host_mkfs_committed` `host_formation_notify` `host_form` `host_table_copy` `cand_cmp` `host_seed_propose` `host_formation_ready` `server_raft_host_mkfs` `host_member_add_forward` `host_member_add_decode` `host_member_add` `host_member_kick` `host_join_enqueue` `host_member_learn_kv` `host_auto_form` `host_notice_pump` `host_voter_reconcile` `host_member_thread` `server_member_hello` `hello_ack_record` `hello_ack_members` `server_member_hello_join` `server_member_filter`

Logs: `raft-host` lines from `host_mkfs_committed`, `host_formation_notify`, `host_form`, `host_seed_propose`, `server_raft_host_mkfs`, `host_auto_form` and 1 more

Tests: `tests/test_w153_formation.py` `tests/test_k33_setup.py` `tests/test_k33_join_retry.py` `tests/test_k33_seed_loss.py` `tests/test_k33_interrupt.py`

## Outbox and sender threads

Does: Per-peer outbound queues with two lanes (heartbeats, AE replies and votes; entry-carrying AppendEntries and snapshot chunks in order). `host_send` encodes and queues under `h->mu`, coalescing a stale heartbeat or reply and evicting when full; a lazily started sender thread per peer batches the blocking send and ACK on a private connection. Fresh-read probes are queued beside, never instead of, the original frame.

State: `h->tx[]` (`struct host_outbox`: `pri[]`, `ent[]`, counters, `read_probe_*`, the per-peer cv) under `h->outbox_mu`; the sender's private `conn` is owned by its thread only; `tx_running` is the stop gate.

Entry points: none public.

Functions: `host_sender_depth` `host_sender_take` `host_sender_pick` `host_sender` `outbox_nentries` `outbox_entry_lane` `outbox_is_heartbeat` `outbox_is_ae_rep` `outbox_pure_read_control` `outbox_pri_replace` `host_fill_ae_hdr` `host_wire_taken` `host_send_ready` `host_read_probe` `host_send` `host_stop_senders`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_host_pending_entries.py` `tests/test_host_outbox.py` `tests/test_raft_read_probe.py` `tests/test_raft_ack_window.py`

## Pump, inbox and applied checkpoint

Does: The pump thread (`host_pump`) is the single owner of the raft cores: it drains the inbox in bounded FIFO slices, runs ticks and heartbeats, starts requested ReadIndex rounds, applies, publishes the group views, wakes waiters and persists the replay checkpoint (`applied.<g>`) after a successful KV barrier. Handlers only append to the inbox and kick the pump through an eventfd.

State: `h->inbox` and counters under `h->inbox_mu`; `pump_efd` (eventfd, no lock); cores, `applied_saved`, `cursor_durable` and the stored-reader notification under `h->mu` (the pump owns step-down); `applied_fd` is owned by the checkpoint owner.

Entry points: `server_raft_host_inbox` In file: `host_pump_kick` (any thread, never blocks), `host_stored_decode_error` (KV scan callbacks).

Functions: `host_inbox_take` `host_pump_poll_timeout` `host_note_queued_reads` `host_receive_persist` `host_inbox_batch_handoff` `drain_inbox` `applied_path` `host_applied_interval` `host_capture_cursor` `host_publish_cursor` `persist_applied` `host_save_applied` `host_persist_durable_tail` `load_applied` `host_pump_kick` `host_pending_reads` `host_stored_decode_error` `host_stop_leading` `host_heartbeat_due` `host_pump` `host_read_control` `server_raft_host_inbox`

Logs: `raft-host` lines from `host_stop_leading`, `host_pump`, `server_raft_host_inbox`

Tests: `tests/test_host_batch_handoff.py` `tests/test_applied_interval.py` `tests/test_cursor_checkpoint.py` `tests/test_host_checkpoint.py` `tests/test_shutdown_bounded.py`

## KV and checkpoint workers

Does: Off-pump workers that finish what the pump started under its WAL hold: the KV worker runs the durable barrier for a pump cycle and signals readers waiting on the accepted data cursor; the checkpoint worker writes the restart marker after a proven release. Each has a dispatch, a stop and a start.

State: `struct host_kv_worker` and `struct host_checkpoint_worker` (queue, flags, condition) under their own `w->mu`; `host_kv_signal_data` is called from `host_kv_dispatch` with `w->mu` held and takes no host lock. The hold buffers are owned by the dispatching pump cycle.

Entry points: none from other files. In file: `host_kv_dispatch`, `host_checkpoint_dispatch` (pump, called off `h->mu` with the cycle's WAL hold still owned).

Functions: `host_checkpoint_retire` `host_checkpoint_work` `host_checkpoint_start` `host_checkpoint_dispatch` `host_checkpoint_stop` `host_kv_work` `host_kv_start` `host_kv_signal_data` `host_kv_dispatch` `host_kv_stop`

Logs: `raft-host` lines from `host_kv_work`

Tests: `tests/test_kv_prefix_worker.py` `tests/test_checkpoint_worker.py` `tests/test_cursor_checkpoint.py`

## Group view, waiters and leader state

Does: Publishes one group's replica state (role, term, commit, applied, read-round and proof fields) as a seqlock that handlers read without `h->mu`, and parks readers and appliers in per-waiter slots that the pump wakes only when the new view satisfies their predicate. Also the lock-free leader and "is leader" reads used for routing and status.

The view and the waiters moved to their own owner, `src/server/group_view.c` with the contract and the inline predicates in `src/server/group_view.h` (Task V3; page `docs/status/2026-11/group-view-ownership-20261111.md`). `raft_host.c` holds one `struct group_views gv` in the host and reaches it through `host_publish_view`, `host_view_get`, the wait bracket `group_view_wait_lock`/`group_view_wait_unlock` with `host_waiter_sleep`, `host_waiters_wake`, `group_view_init`/`group_view_destroy`/`group_view_attach`/`group_view_waiting`. `host_publish_group`, `host_halted_hint`, `host_local_leader`, `host_local_view`, `host_is_leader`, `host_leader_hint` and the four `server_raft_host_*` entry points stay here: they read no private field of the owner.

State: private to the owner: the per-group slots (`v_*`, `v_seq`, written by `host_publish_view` under `h->mu` plus `view_mu`, read by `__atomic_load_n`); the sleeper table `wait_slot[]`, `waiter_active`, `cv_overflow`, `applied_cv` under `cv_mu`. `scripts/check_group_view_privacy.py` fails if another file names them.

Entry points: `server_raft_host_active` `server_raft_host_formed` `server_raft_host_unresponsive_voters` `server_raft_host_group_commit` In file: `host_view_get`, `host_waiter_sleep`, `host_waiters_wake` (declared for the ReadIndex round and the apply wait).

Functions: `group_raft` `group_slot` `host_publish_view` `host_publish_group` `host_view_get` `host_halted_hint` `host_view_covers` `host_view_quorum_index` `host_view_wait_apply` `group_view_init` `group_view_destroy` `group_view_attach` `group_view_waiting` `group_view_slot` `group_view_wait_lock` `group_view_wait_unlock` `host_waiter_sleep` `host_waiters_wake` `host_local_leader` `host_local_view` `server_raft_host_active` `server_raft_host_formed` `server_raft_host_unresponsive_voters` `server_raft_host_group_commit` `host_is_leader` `host_leader_hint`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_read_waiters.py` `tests/test_readindex_publish.py` `tests/test_readindex_result.py` `tests/test_host_batch_handoff.py`

## ReadIndex round

Does: A reader admits itself against the published view (no `h->mu` in server mode), asks for a round by raising `ri_request`, and sleeps in a waiter slot until the quorum proof and its own apply cover the request. The pump starts the requested round; a follower coalesces one leader RPC per round (`g_ri_coal`) and redirects to the leader when it cannot apply in time. The pair path runs two independent rounds on bounded helper threads; the apply wait gives an own entry a progress-renewed budget and a served-behind ReadIndex one fixed budget.

State: `host_group.ri_begun` (raised under `h->mu` before the core sends), `ri_request` (CAS, no lock); `g_ri_coal[]` under each entry's `mu`; `g_read_prof` (relaxed atomics); `h->apply_progress` and the wait counters (atomics) with the sleep under `h->cv_mu`.

Entry points: none public. In file: `host_read_index` (called by every metadata handler that reads rows), `host_wait_applied_own` (called by the verdict waits).

Functions: `host_read_wait_ready` `host_read_floor_ready` `host_wait_applied` `host_ri_complete` `host_ri_remove` `host_remote_read_index` `host_follower_read_result` `host_read_note_begin` `host_read_begin_tracked` `host_read_admit_view` `host_read_request` `host_read_start_requested` `host_read_index_admitted` `host_read_index` `host_read_pair_capacity` `host_read_pair_note` `host_read_index_pair` `host_wait_applied_mode` `host_wait_applied_own`

Logs: `raft-host` lines from `host_read_index_admitted`

Tests: `tests/test_readindex_result.py` `tests/test_raft_read_admission.py` `tests/test_readindex_pair.py` `tests/test_readindex_tickets.py` `tests/test_follower_read_redirect.py`

## Leader routing and forwards

Does: Picks where an operation runs and forwards it: the hosted groups of a node, a peer that hosts every needed group, the leader of a single group, with the read-only deadline scope for proofs. `host_rpc_submit` and `host_remote_cmd` carry a member-forwarded submit; each `host_fwd_*` re-encodes one handler's request (keeping the client's op-id suffix) for the peer that finally runs it.

State: `g_fwd_leader[]` (leader hint, relaxed atomic store in `host_fwd_leader_note`); `host_serving_forward` (thread-local); routing reads the published view and `host_group.voters` (acquire load) without `h->mu`; peer addresses are read in `peer_addr` under `h->s->lock`, and its callers (sender thread, `host_rpc_submit`, `host_inode_rpc_peer`, `host_recovery_scan_remote`) hold no other host lock that this page determined.

Entry points: none public. In file: `host_remote_cmd`, `host_inode_forward`, `host_pick_peer`, `host_leader_cover_pick` (every handler's forward path).

Functions: `peer_addr` `host_hosts` `host_hosts_all` `bounce_add` `host_fwd_leader_pick` `host_fwd_leader_note` `host_pick_peer` `host_route_voters` `host_leader_cover_pick` `host_need_both` `host_report_primary_pick` `host_submit_read_only` `host_rpc_submit` `host_remote_cmd_impl` `host_remote_cmd` `host_deadline_us` `host_past_deadline` `host_inode_rpc_peer` `host_inode_forward` `host_fwd_opid` `host_fwd_create` `host_fwd_lookup` `host_fwd_getattr` `host_fwd_hold` `host_fwd_flock` `host_fwd_append` `host_fwd_unlink` `host_fwd_link` `host_fwd_rename` `host_fwd_lookup_path` `host_fwd_readdir` `host_fwd_setattr` `host_fwd_truncate_opid` `host_fwd_getchunks` `host_fwd_report` `host_serving_forward_restore`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_read_retry_budget.py` `tests/test_remote_cmd_halted_peer.py` `tests/test_host_cover_route.py` `tests/test_host_read_deadline.py` `tests/test_forward_once.py`

## Proposal, admission and verdict wait

Does: Takes a command from a handler into the raft log and waits for what the apply decided. `host_admit` refuses before the hard bounds (BUSY at the watermark), `host_propose_impl` appends under `h->mu` and publishes the view, and the verdict is read from the apply-result ring by (index, term) so a truncated tail is never mistaken for success. Durable-sync coverage and the REPORT/cohort hold keep an append from being advertised before the shared log covers it.

State: `host_group.arc[]` (apply-result ring, seqlock slots written by `host_apply` under `h->mu`, read lock-free; `host_arc_read` falls back to `h->mu`); `propose_q` (file-scope counter, atomic add/sub in `propose_q_add`/`propose_q_sub`); `h->admit_busy` and `obs_arc_*` counters (lock not determined); durable-synced offsets: `host_durable_synced_mode` and `host_mark_durable_covered` are called with `h->mu` held, per their comments.

Entry points: none public. In file: `host_propose`, `host_propose_wait`, `host_wait_verdict`, `host_wait_settled`, `host_wait_refs` (called by the writing handlers), `host_bg_propose` (GC and background passes).

Functions: `propose_q_add` `propose_q_sub` `propose_q_busy` `host_durable_synced_mode` `host_durable_synced` `host_note_durable_synced` `host_mark_durable_covered` `host_tag_refused` `host_kv_pressure` `host_admit` `host_submit_serves_forward` `host_forward_refused` `host_propose_impl` `host_arc_read` `host_arc_verdict` `host_apply_rc` `host_arc_apply_us` `host_wait_verdict` `host_wait_settled` `host_propose_mode` `host_propose` `host_propose_wait_idx_impl` `host_propose_wait_idx` `host_propose_wait` `host_bg_propose` `host_wait_refs` `host_wait_refs_each`

Logs: `raft-host` lines from `host_tag_refused`, `host_admit`

Tests: `tests/test_propose_hold_ownership.py` `tests/test_apply_ring.py` `tests/test_gc_verdict.py` `tests/test_k32_entry_count.py` `tests/test_prepare_wait_progress.py`

## Apply: core

Does: `host_apply` is the raft core's apply callback: it decodes the tag, runs the matching `apply_*_cmd` handler inside the KV writer scope, records the verdict in the apply-result ring, and halts the group only for a command it cannot apply (`host_mark_incompatible`, W127) or a missing row (`host_mark_incomplete`). The per-group KV cursor is written atomically with the entry's effects.

State: apply-result ring and `kv_incomplete`, `incompatible`, `incompat_*` fields of `host_group` under `h->mu`; the KV cursor key in the node-local shard; `h->rec_mu` for stranded-transaction marks set from apply.

Entry points: none public. In file: `host_apply`, `host_apply_dispatch` (registered with each raft core).

Functions: `host_apply_coord` `host_apply_dispatch` `host_mark_incomplete` `host_mark_incompatible` `host_clear_incomplete` `host_kv_cursor` `host_apply` `host_group_incompatible`

Logs: `txn-fault` (host_apply); `raft-host` lines from `host_mark_incomplete`, `host_mark_incompatible`, `host_clear_incomplete`, `host_apply`

Tests: `tests/test_apply_unsupported.py` `tests/test_txn_apply_exact.py` `tests/test_session_combined_group.py` `tests/test_txn_apply_retry.py`

## Apply: namespace and append commands

Does: Apply handlers for the metadata mutations: MKFS, SALT, MEMBERS, CREATE, UNLINK, RMDIR, SETATTR, XATTR, UTIMENS, lazy TRUNCATE, lane activation and bootstrap, DIR, and the three APPEND forms. Each validates the exact length and owning group before touching the KV, and a logical refusal is a verdict, not a halt.

State: the KV rows it writes, under the writer scope `host_apply` holds (`h->mu`); no file-scope state.

Entry points: none public.

Functions: `apply_mkfs_cmd` `apply_salt_cmd` `apply_members_cmd` `apply_create_cmd` `apply_unlink_cmd` `apply_rmdir_cmd` `apply_setattr_cmd` `apply_xattr_cmd` `apply_utimens_cmd` `apply_truncate_lazy_cmd` `apply_activate_lane_cmd` `apply_bootstrap_lane_cmd` `apply_append_atomic_cmd` `apply_append_rsv_cmd` `apply_append_catchup_cmd` `host_dir_migrate_local` `apply_dir_cmd`

Logs: `raft-host` lines from `apply_mkfs_cmd`, `apply_salt_cmd`, `apply_members_cmd`, `apply_create_cmd`, `apply_unlink_cmd`, `apply_rmdir_cmd` and 8 more

Tests: `tests/test_apply_unsupported.py` `tests/test_pending_create_command.py` `tests/test_atomic_append_host.py` `tests/test_append_catchup_host.py` `tests/test_dir_migrate_route.py`

## Apply: sessions, locks, publication and transactions

Does: Apply handlers for the session table (REGISTER, ESTABLISH, LEASE and lock commands, cleanup), file publication and REPORT records (each ticketed and checked against the lane floor), the transaction commands (PREPARE, DECIDE, RESOLVE, RETIRE, TXN_BATCH) and the PUT ticket batches.

State: the KV rows it writes under the writer scope (`h->mu`); lock-wait entries are dequeued under `h->wait_mu` by the session apply (`apply_session_cmd`).

Entry points: none public.

Functions: `apply_session_cmd` `apply_lock_cmd` `apply_one_publish` `apply_report_publish_cmd` `apply_txn_cmd` `apply_txn_batch_cmd` `apply_put_ticket_batch_cmd`

Logs: `raft-host` lines from `apply_session_cmd`, `apply_one_publish`

Tests: `tests/test_txn_batch_apply.py` `tests/test_publication_host.py` `tests/test_session_combined_group.py` `tests/test_txn_decision_sweep.py` `tests/test_namespace_session_apply.py`

## Apply: storage, sweep and recovery commands

Does: Apply handlers for host-generated storage maintenance: truncate sweeps, lane sweeps, REAP_MULTI, the W125 packer commands, GC_ACK, PUT orphan and ticket-drain acks, degraded marks and clears, and the K36 recovery mark. Per-member refusals are recorded as ledger rows; only storage failures retry the entry.

State: the KV rows it writes under the writer scope (`h->mu`); no file-scope state.

Entry points: none public. In file: `host_pack_sweep_next` (an unhosted reaper asks the lane leader for one immutable batch; never logged).

Functions: `apply_trunc_sweep_cmd` `apply_sweep_done_cmd` `apply_retire_cmd` `host_pack_sweep_next` `apply_lane_sweep_multi_cmd` `apply_reap_multi_cmd` `apply_pack_cmd` `apply_orphan_reap_cmd` `apply_put_orphan_ack_cmd` `apply_put_ticket_drain_cmd` `apply_degraded_clear_cmd` `apply_recovery_mark_cmd` `apply_degraded_mark_cmd` `apply_gc_ack_cmd`

Logs: `raft-host` lines from `apply_trunc_sweep_cmd`, `apply_lane_sweep_multi_cmd`, `apply_reap_multi_cmd`, `apply_pack_cmd`, `apply_put_orphan_ack_cmd`, `apply_put_ticket_drain_cmd` and 4 more

Tests: `tests/test_gc_verdict.py` `tests/test_put_drain.py` `tests/test_k36_recovery.py` `tests/test_pack_ledger_recovery.c`

## Command pack and unpack

Does: Encodes the proposed commands (`pack_*`, big-endian, matching the simulator's layout, with the op-id trailer and the mount-session bytes) and decodes the batched ones (`unpack_*`, `host_unpack_*`): publications, REPORT records, PUT ticket batches, lease edges, session cleanup. `host_publication_command_valid` and the TXN_BATCH frame check are shared by the apply and the proposal gate.

State: none (pure functions over the caller's buffers).

Entry points: none public.

Functions: `pack_lease_cmd` `pack_opid_trailer` `unpack_opid_trailer` `unlink_cmd_opid` `host_unpack_lease_edge` `host_unpack_session_cleanup` `host_unpack_settle` `host_unpack_ticket_batch` `host_publication_command_valid` `unpack_publication` `unpack_report_ticket` `txn_batch_shard` `txn_batch_frame_check` `pack_create_cmd` `pack_unlink_cmd` `pack_rmdir_cmd` `pack_setattr_cmd` `pack_utimens_cmd` `pack_lock_cmd` `fill_flock_req` `pack_publish_cmd` `pack_report_rec` `pack_activate_at_cmd` `pack_prep` `pack_prep_raw` `pack_decide` `pack_retire` `pack_resolve` `pack_drop` `fill_txid`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_txn_batch_apply.py` `tests/test_k32_entry_count.py` `tests/test_pending_create_command.py` `tests/test_submit_unknown_tag.py`

## Sessions, leases and lock waits

Does: Session admission on the metadata path (REGISTER, ESTABLISH, cleanup, per-namespace-shard admission and the parallel REPORT admission), lease edges and close effects, file locks and open leases, and the leader-memory queue of blocked lock requests (strict FIFO per inode). Also the storage-side session fence.

State: `h->wait_head`, `wait_tail` and each `host_lock_wait` under `h->wait_mu` (never held while sleeping; the waiter sleeps on its own cv); `host_report_session_parallel` serialises the parallel REPORT admission workers; no other file-scope state.

Entry points: `server_raft_host_storage_session_fence` `server_raft_host_storage_session_learn` `server_raft_host_hold` `server_raft_host_flock`

Functions: `host_lock_wait_drop_session` `host_lease_close_effects` `host_session_register_active_proof` `host_session_register_apply` `host_session_combined_group` `host_sess_gate` `host_session_get_covered` `host_session_get` `host_session_establish_admit_covered` `host_session_establish_admit` `host_session_cleanup_admit` `host_pack_session_cleanup` `host_lease_edge_result` `host_lease_submission_result` `host_storage_fence_members` `server_raft_host_storage_session_fence` `server_raft_host_storage_session_learn` `server_raft_host_hold` `lock_wait_queued_locked` `lock_wait_is_head_locked` `lock_wait_signal_locked` `lock_wait_unlink_locked` `host_lock_wait` `server_raft_host_flock` `host_namespace_session_command` `host_namespace_session_prepare_owner` `host_namespace_session_admit_covered` `host_namespace_session_admit_after_group` `host_namespace_session_admit_with_owner` `host_namespace_session_admit` `host_report_session_worker` `host_report_session_admit` `host_session_cleanup_probe` `host_fence_arm` `host_fence_claim` `host_fence_class` `host_fence_disarm` `host_fence_drain` `host_fence_record` `host_fence_release` `host_fence_resume` `host_fence_rpc` `host_fence_unref` `host_fence_worker` `server_raft_host_fence_wake`

Logs: `storage-fence` (host_storage_fence_members)

Tests: `tests/test_session_admission.py` `tests/test_namespace_admission_rounds.py` `tests/test_report_session_admission.py` `tests/test_namespace_authority.py` `tests/test_storage_fence_members.py`

## Transaction coordination

Does: Builds and drives cross-shard transactions: PREPARE, DECIDE and RESOLVE proposals (`host_prep*`), the participant set and ancestry guards, allocation and resize decisions, the batched PREPARE/RESOLVE path (`host_txn_batch_*`), the command cohort that proposes independent commands together, and the fault barriers tests use to stop a transaction before or after its decision.

State: `host_command_capture` (thread-local pointer to the active `struct host_command_cohort`; `host_command_cohort_flush` takes `c->host->mu` to propose the cohort); `struct host_txn_batch` and `struct host_resize_ctx` are per call. Other proposals go through the proposal concern (`h->mu`).

Entry points: none public. In file: `host_txn_commit`, `host_txn_batch_submit`, `host_prep` (every multi-shard handler).

Functions: `host_txn_coord` `host_txn_read_coord` `host_parts_add` `host_pver_guard_chain` `host_command_cohort_flush` `host_command_cohort_add` `host_command_cohort_begin` `host_command_cohort_end` `host_txn_fault_barrier` `host_decide_lost_fault` `host_txn_batch_init` `host_txn_batch_drop` `host_txn_batch_submit` `host_txn_batch_close` `host_txn_batch_add` `host_txn_batch_flush` `host_txn_commit` `host_prep` `host_prep_async` `host_prep_raw_pack` `host_prep_raw` `host_prep_value` `host_alloc_capture` `host_prep_alloc_value` `host_mkdir_alloc_reserve` `host_prep_ino_delta` `host_prep_dseq_bump` `host_has_opid` `host_prep_opid` `host_prep_lane_stamp` `host_drop_parts` `host_resize_decide` `host_resize_resolve` `host_mtime_prepare`

Logs: `raft-host` lines from `host_decide_lost_fault`, `host_txn_commit`, `host_drop_parts`, `host_resize_decide`

Tests: `tests/test_append_prepare_batch.py` `tests/test_prepare_cohort.py` `tests/test_append_cohort.py` `tests/test_dir_migrate_visibility.py` `tests/test_k32_entry_count.py`

## Transaction recovery and finisher

Does: Completes transactions a crash or a lost reply left half-done. The finisher thread (`host_fin_thread`) finishes a coordinator whose DECIDE is in the log; the GC thread's recovery pass marks, scans and resolves stranded participants (`host_txn_recover_*`) and sweeps live decisions for retirement.

State: the finisher ring under `h->fin_mu` with `fin_cv`; `h->rec_mark[]` under `h->rec_mu` (set by the apply path under `h->mu`, read by the GC thread); `rec_hot_next`/`rec_cold_next` are GC-thread cursors; decision-sweep continuations are GC-thread-only.

Entry points: none public.

Functions: `host_fin_add` `host_txn_retire` `host_fin_one` `host_fin_thread` `host_rec_mark` `host_rec_mark_all` `txid_ns` `host_leads` `host_txn_recover_one` `host_txn_decision_sweep` `host_txn_recover_pass` `host_recover_thread`

Logs: `raft-host` lines from `host_fin_one`, `host_txn_recover_one`

Tests: `tests/test_txn_recovery_priority.py` `tests/test_txn_decision_sweep.py` `tests/test_fin_abort_handoff.py` `tests/test_recovery_pin_owner.py` `tests/test_stale_recovery.py`

## Namespace operations

Does: The server side of the directory and inode mutations: create (also hashed and retained create), mkdir, rmdir, unlink, link, rename, setattr, xattr, utimens, lazy truncate and expire. Each probes the op-id window, reads under ReadIndex, builds a single-group command or a transaction, and replies with the recorded verdict; `host_create_ex` is the shared create path.

State: no file-scope state of its own; per-call structs only. Proposals go through `h->mu` in the proposal concern.

Entry points: `server_raft_host_create` `server_raft_host_create_retained` `server_raft_host_mkdir` `server_raft_host_rmdir` `server_raft_host_unlink` `server_raft_host_setattr` `server_raft_host_xattr` `server_raft_host_link` `server_raft_host_rename_at` In file: `host_create`, `host_unlink_txn`, `host_truncate_lazy`, `host_utimens` (called across handlers).

Functions: `host_read_leader_hint` `host_hashed_create_txn` `host_opid_replay` `host_opid_reply_ino` `host_test_dir_serve_read` `host_create_ex` `host_create` `server_raft_host_create` `host_retained_create_forward` `host_retained_create_proof` `server_raft_host_create_retained` `host_namespace_capacity` `host_regular_mtime` `host_dent_drop_prep` `host_namespace_leader_hint` `server_raft_host_mkdir` `host_test_rmdir_busy` `server_raft_host_rmdir` `host_unlink_txn` `server_raft_host_unlink` `host_utimens` `host_truncate_lazy` `server_raft_host_setattr` `server_raft_host_xattr` `server_raft_host_link` `server_raft_host_rename_at`

Logs: `namespace-prof` (server_raft_host_mkdir); `raft-host` lines from `host_opid_reply_ino`, `host_test_dir_serve_read`, `host_create_ex`, `host_namespace_capacity`, `server_raft_host_mkdir`, `host_test_rmdir_busy` and 4 more

Tests: `tests/test_retained_create_authority.py` `tests/test_namespace_receipt_pending.py` `tests/test_namespace_leader_route.py` `tests/test_truncate_read_reuse.py` `tests/test_k48_readdir.py`

## Directory migration and spread

Does: Moves a LOCAL directory to HASHED placement and spreads it across groups: the decoder for a planning request (`host_dir_migrate_request_decode`, called from `server_raft_host_submit`), a transaction for leftovers whose shard is on the other group, and the spread pass that decides when. The same-group migrate stays inside the apply (`apply_dir_cmd`, Apply: namespace; `host_dir_migrate_local`).

State: no file-scope state; the planner reads rows under ReadIndex.

Entry points: none public. In file: `host_dir_migrate` (the submit path), `host_dir_spread_pass` (the GC thread).

Functions: `host_dir_migrate_request_decode` `host_dir_migrate` `host_dir_migrate_txn` `host_dir_spread_pass`

Logs: `raft-host` lines from `host_dir_spread_pass`

Tests: `tests/test_dir_migrate_route.py` `tests/test_dir_migrate_visibility.py`

## Append

Does: The O_APPEND write path: reserve and prepare, decide, resolve, the atomic single-shard form, the batched form for a session's close-step, the transaction form and the abandon. `server_raft_host_append` serialises appenders per inode with a lock stripe and chooses between the forms.

State: `host_append_locks[]` (a stripe of mutexes, initialised once under `host_append_once`; `server_raft_host_append` takes one per inode with `pthread_mutex_trylock` and answers BUSY when taken, then runs `host_append_owner` with it held); `host_append_ops` and `host_append_reserve_ops` are const tables; the rest is per call.

Entry points: `server_raft_host_append`

Functions: `host_append_close_batched` `host_close_append_step` `host_session_append_step` `host_append_session_admit_covered` `host_append_prepare_ref` `host_append_prepare_pack` `host_append_prepare` `host_append_reserve_prepare` `host_append_reserve_decide` `host_append_reserve_resolve` `host_append_authority` `host_append_execute_atomic` `host_append_atomic_eligible` `host_append_batch_prepare` `host_append_batch_decide` `host_append_batch_resolve` `host_append_execute_batched` `host_append_prune_batched` `host_append_close_prepare` `host_append_transaction` `host_append_abandon` `host_append_primary` `host_append_init` `host_append_owner` `server_raft_host_append` `host_complete_single_append` `host_fault_append_hold`

Logs: `append-abandon` (host_append_abandon); `raft-host` lines from `host_session_append_step`, `host_fault_append_hold`

Tests: `tests/test_append_prepare_batch.py` `tests/test_append_cohort.py` `tests/test_atomic_append_host.py` `tests/test_append_reservation_reply.py` `tests/test_append_host_authority.py`

## Publication and REPORT

Does: Turns a client's REPORT into published file content: per-unit transactions (`host_pub_unit_*`), packed batches proposed and waited together (`host_pub_batch_*`), the atomic publication units of W77 (units valid, set length, stale note), the K32 transaction sets and their republish, and the REPORT receipt table that answers a retry from the recorded outcome.

State: `g_report_receipts` (a `struct efs_receipt_table` from `efs/request_receipt.h`, set up once under `g_report_receipts_once`, flag `g_report_receipts_ok`, trace switch `g_report_receipt_trace`; whether the table locks internally is not determined here); `struct host_pub_batch` and `host_pub_unit` are per call; `host_pub_unit_active` (thread-local, set around `host_pub_unit_commit` and read by `host_pub_fault`).

Entry points: `server_raft_host_report` `server_raft_host_report_receipted` `server_raft_host_report_receipt` `server_raft_host_publication`

Functions: `host_bootstrap_lane` `host_pub_fault` `host_resolve_caught_up` `host_holds_chunks` `host_pub_candidate_gen` `host_pub_stale_note` `host_pub_pack_inner` `host_report_ticket` `host_pub_pack` `host_pub_batch_reset` `host_pub_batch_propose` `host_pub_batch_push` `host_report_release` `host_pub_batch_finish` `host_pub_batch_wait` `host_pub_set_len` `host_pub_units_valid` `host_pub_unit_txn` `host_pub_unit_abort` `host_pub_unit_commit` `host_pub_units_publish` `host_report_stale_verdict` `server_raft_host_report` `host_report_receipts_init` `host_report_receipt_key` `host_report_status_rc` `host_report_exec` `host_receipt_how_name` `server_raft_host_report_receipted` `server_raft_host_report_receipt` `host_publication_committed` `server_raft_host_publication`

Logs: `report-split` (server_raft_host_report); `report-receipt` (host_report_receipts_init, server_raft_host_report_receipted, server_raft_host_report_receipt); `raft-host` lines from `host_pub_fault`, `host_pub_stale_note`, `host_pub_pack_inner`, `host_pub_pack`, `host_pub_unit_txn`, `host_pub_unit_commit` and 2 more

Tests: `tests/test_w155_report_resolve.py` `tests/test_k32_entry_count.py` `tests/test_publication_host.py` `tests/test_report_log_coverage.py` `tests/test_report_stepdown.py`

## Read handlers

Does: The read-side RPCs: getattr, lookup, lookup by path, readdir and getchunks. Each admits through ReadIndex, reads the local replica after authority is proven, and returns the stat or rows with the content epoch, or forwards when the lanes are not hosted here.

State: none of its own; per-call reads of `h->kv` after `host_read_index`.

Entry points: `server_raft_host_getattr` `server_raft_host_lookup` `server_raft_host_getchunks` `server_raft_host_readdir` `server_raft_host_lookup_path`

Functions: `server_raft_host_getattr` `host_lookup_plan_group` `server_raft_host_lookup` `host_getchunks_collect` `server_raft_host_getchunks` `server_raft_host_readdir` `server_raft_host_lookup_path`

Logs: `raft-host` lines from `server_raft_host_getchunks`

Tests: `tests/test_lookup_replacement.py` `tests/test_lookup_group_plan.py` `tests/test_getchunks_collect_retry.py` `tests/test_lookup_path_authority.py` `tests/test_k48_readdir.py`

## Writer view and lane bootstrap

Does: The endpoints a writing client uses before it publishes: the writer's view of an inode's lanes (`server_raft_host_writer_view` and its reuse and lane variants) and the lane bootstrap, which stamps a lane on the inode group (`server_raft_host_lane_bootstrap`, called per item by `server_raft_host_lane_bootstrap_rpc` as a bounded cohort). The publication path calls `host_bootstrap_lane` (listed with Publication and REPORT).

State: none of its own; `server_raft_host_lane_bootstrap_rpc` reads the node table under `h->s->lock`.

Entry points: `server_raft_host_lane_bootstrap` `server_raft_host_lane_bootstrap_rpc` `server_raft_host_lane_writer_view` `server_raft_host_writer_view_reuse` `server_raft_host_writer_view`

Functions: `server_raft_host_lane_bootstrap` `server_raft_host_lane_bootstrap_rpc` `server_raft_host_lane_writer_view` `server_raft_host_writer_view_reuse` `server_raft_host_writer_view`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_lane_bootstrap_host.py` `tests/test_lane_bootstrap_export.py` `tests/test_lane_writer_host.py`

## Inode replies and lane reads

Does: Shapes a reply from a row: stat conversion, the content epoch, the return-code to inode-status mapping, the routing hint, the check that every active lane of a row is hosted here, and the lane reads that follow a ReadIndex (reuse of an earlier fence, covered mask, one read per missing group).

State: none (per call); `host_export_salt` reads the immutable salt record under `h->mu`.

Entry points: none public. In file: `host_read_inode_lanes`, `host_read_inode_lanes_mask`, `host_stat_from_row`, `set_inode_rc`.

Functions: `stat_to_inode` `reply_note_epoch` `host_stat_from_row` `host_export_salt` `rc_to_inode_status` `host_reply_hint` `set_inode_rc` `host_row_lanes_hosted` `host_read_inode_lanes_reuse` `host_read_inode_lanes_covered` `host_read_inode_lanes_mask` `host_read_inode_lanes`

Logs: `raft-host` lines from `set_inode_rc`

Tests: `tests/test_truncate_read_reuse.py` `tests/test_readindex_operation.py` `tests/test_lookup_path_authority.py` `tests/test_namespace_leader_route.py`

## Expiry

Does: D43 expiry: the cluster policy switch read through a ReadIndex authority, births collected at create, the due-row scan, the expiry transaction (update, renew, enable, policy) and its delete, the clock read, and the accelerated pass groups run by the GC thread.

State: `h->expiry_enabled`, `expiry_folders` (atomics); `expiry_peer_skip` and `expiry_clock` under `h->mu`.

Entry points: `server_raft_host_expire`

Functions: `host_expiry_on` `host_expiry_births` `host_expiry_switch_authority` `host_expiry_refresh` `host_expiry_due_covered` `host_gc_expiry_idle` `host_expiry_clock_read` `host_expiry_delete` `host_gc_expiry_pass` `host_gc_expiry_groups` `host_expiry_collect` `host_expiry_update_txn` `host_expiry_renew_txn` `host_expiry_enable_txn` `host_expiry_policy_txn` `server_raft_host_expire`

Logs: `raft-host` lines from `host_expiry_delete`

Tests: `tests/test_gc_backlog_idle.py` `tests/test_expiry_policy_live.py` `tests/test_gc_status_expiry.py` `tests/test_namespace_bounds.py`

## Reclaim and GC thread

Does: The background GC thread (`host_gc_thread`) and the passes it runs per group: dead-chunk fragment deletes with batched GC_ACK, whole-inode reclaim with the batched acceptance RPCs to every member and bounded REAP_MULTI completion, packer retire, close-step retire and orphan collection. The thread yields to waiting metadata and stops on shutdown.

State: scan cursors `g_gc_cur[]`, `g_gc_cur_len[]`, the reap arrays `host_reap_*` and `host_reap_sweep/done[]` (GC thread only); `h->gc_tid`; the node table under `h->s->lock` for batch RPC targets.

Entry points: none public (`server_raft_host_gc_running` is in Lifecycle). In file: `host_gc_thread`.

Functions: `gc_gi` `gc_scan_cb` `gc_count_cb` `reap_scan_cb` `host_gc_export` `host_gc_inode_nodes_batch` `host_gc_propose` `host_gc_segment_nodes_batch` `pack_retire_cb` `host_pack_retire_pass` `host_gc_scan_work` `orphan_scan_cb` `close_step_scan_cb` `host_gc_close_retire_batch` `host_gc_close_step_pass` `host_gc_close_pass` `host_gc_orphan_pass` `host_gc_records` `host_gc_metadata_waiting` `reap_multi_init` `reap_multi_flush` `reap_multi_add` `host_gc_lane_swept` `host_gc_reap_pass` `host_gc_ack_flush` `host_gc_frag_pass` `host_gc_backlog_idle` `host_gc_thread`

Logs: `raft-host` lines from `host_gc_inode_nodes_batch`, `host_pack_retire_pass`, `host_gc_records`, `reap_multi_flush`, `host_gc_reap_pass`, `host_gc_ack_flush` and 2 more

Tests: `tests/test_gc_reap_shutdown.py` `tests/test_gc_fragment_backlog.py` `tests/test_reap_fairness.py` `tests/test_close_retire_fastpath.py` `tests/test_gc_backlog_idle.py`

## Truncate sweep

Does: D25 truncate GC: finds inodes with a pending truncate target old enough to sweep, bounds the pass by the work room, and proposes the sweep, sweep-done and lane commands.

State: `trunc_age[]` (first-seen table, GC thread only).

Entry points: none public. In file: `host_gc_trunc_pass` (the GC thread).

Functions: `trunc_sweep_due` `trunc_pass_room` `trunc_scan_cb` `host_gc_trunc_pass`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_trunc_gc_fairness.py`

## Put-orphan and put-drain passes

Does: D31 abandoned-upload collection: scans PUT ticket records, checks the revoked epoch, deletes orphan fragments and proposes the ack; the drain pass raises the lane floor with a PUT_TICKET_DRAIN. The storage side answers the orphan-status RPC after a ReadIndex.

State: `h->put_orphan_cursor*`, `put_orphan_cycle` and the drain cursors (GC thread owns them); per-pass `struct put_orphan_host`/`put_drain_host`.

Entry points: `server_raft_host_put_orphan_status`

Functions: `put_ticket_group` `server_raft_host_put_orphan_status` `host_put_orphan_status` `put_orphan_scan_cb` `put_orphan_revoked` `put_orphan_delete` `put_orphan_ack` `host_put_orphan_pass` `put_drain_scan_cb` `put_drain_revoked` `put_drain_cmd` `host_put_drain_pass`

Logs: `raft-host` lines from `host_put_orphan_pass`, `host_put_drain_pass`

Tests: `tests/test_put_drain.py` `tests/test_k54_member_down_fence.py` `tests/test_put_orphan_server.c`

## Degraded-fragment rebuild

Does: W154/K35 rebuild: scans degraded marks the GC thread leader owns, fetches or reconstructs a fragment from the other members (`dg_get_fragment`), puts it on the target node (`dg_put_fragment`), probes a fenced node, and proposes the mark clear. A node that failed is skipped for twice the failed call's cost.

State: `g_dg_cur[]`, `g_dg_node_backoff_us[]`, `g_dg_pass`, `g_dg_node_defer[]` (GC thread only); `h->s->lock` for the node table.

Entry points: `server_raft_host_degraded_check` `server_raft_host_degraded_now`

Functions: `degraded_clear_decode` `degraded_clear_encode` `degraded_mark_group` `server_raft_host_degraded_check` `host_degraded_check` `degraded_mark_cmd_group` `dg_skip` `dg_scan_cb` `dg_count_cb` `dg_conn_get` `dg_conn_release` `dg_node` `dg_get_fragment` `dg_put_fragment` `dg_commit_probe` `dg_ready` `dg_propose_clear` `dg_one` `host_rebuild_pass` `server_raft_host_degraded_now`

Logs: `raft-host` lines from `dg_one`, `host_rebuild_pass`

Tests: `tests/test_k35_fsync_degraded.py` `tests/test_k35_placement.py` `tests/test_k37_segment_rebuild.py` `tests/test_put_upload_server.c`

## Snapshots

Does: Raft snapshots of the KV: the pump pins a view and the GC thread writes the `.kvx` file (`host_snap_export_pass`); the sender reads it in chunks; a follower writes a `.part` file, queues the import, and the GC thread diffs it (`host_snap_import_pass`, which drops an image the member has already applied past or leads) before the pump batches the diff in the ack. While an import is queued, running or being applied, a chunk at offset 0 of any snapshot is answered BUSY (`snap_import_active`; the pump also holds elections while any import state is pending, `snap_import_pending`), and the pump steps the finished import once per cycle and installs it in the Raft core (`host_snap_import_pump`, `efs_raft_snap_imported`) even when the leader no longer retries that image. `host_maybe_snapshot` decides when.

State: `host_group.snap_*`, `part_*`, `import_*` under `h->snap_mu`; `import_block` makes `host_apply` of that group return BUSY while a diff is in flight.

Entry points: none public. In file: `host_snap_open`, `host_snap_read`, `host_snap_close` (raft core callbacks), `snap_import_poll`.

Functions: `host_snap_open` `host_snap_read` `host_snap_close` `snap_import_free_diff` `snap_import_cancel` `snap_import_active` `snap_import_pending` `snap_import_poll` `host_snap_chunk` `host_snap_import_pump` `host_snap_window_bytes` `host_maybe_snapshot` `host_snap_unlink_other` `host_snap_drop_parts` `host_snap_export_pass` `host_snap_import_pass`

Logs: `raft-snap` (host_snap_open, host_maybe_snapshot, host_snap_export_pass, host_snap_import_pass, host_snap_import_pump); `raft-host` lines from `snap_import_poll`

Tests: `tests/test_k22_snapshot_import.py` `tests/test_query_snapshot_failover.py` `tests/test_kv_snapshot_tail.c`

## Query service

Does: The catalog query RPCs: a published two-generation view of the namespace refreshed by `host_query_thread` from a pinned source after an authority and fence check, paged streams (`query_page_*`) held under `query_page_mu`, and the request path that enforces fresh permissions and ancestry before returning bytes.

State: `query_page_mu`/`query_page_cv` with the `query_page_session` table and `query_page_serial`; `h->query_lock` (rwlock: readers share the view, publish drains them); `query_reply_bytes`, `query_requests` and `host_query_interval_ms` (relaxed atomics in `server_raft_host_query_snapshot` and `host_query_thread`); `query_export_once` (`pthread_once`).

Entry points: `server_raft_host_query_read` `server_raft_host_query_reply_free` `server_raft_host_query_features_checked` `server_raft_host_query_snapshot` `server_raft_host_query_page`

Functions: `query_mono_ms` `query_export_configure` `query_export_expired` `query_page_clear` `query_page_sweep` `query_page_acquire` `query_page_release` `host_query_cancel` `host_query_capable` `host_query_source_once` `host_query_source` `host_query_disk_cancel` `host_query_policy_authority` `query_reply_status` `host_query_policy_operation` `host_query_thread` `server_raft_host_query_read` `server_raft_host_query_reply_free` `query_access` `query_directory_binding` `query_auth_dir` `query_ancestry` `query_append` `query_command_begin` `query_emit` `query_expired` `query_consume` `server_raft_host_query_features_checked` `server_raft_host_query_snapshot` `query_page_capture` `server_raft_host_query_page`

Logs: `query-config` (query_export_configure, host_query_thread); `query-catchup` (host_query_thread); `query-publish` (host_query_thread); `query-recovery` (host_query_thread); `query-refresh` (host_query_thread); `query-restore` (host_query_thread); `query-revisions` (host_query_thread)

Tests: `tests/test_query_page_barriers.py` `tests/test_query_page_sessions.py` `tests/test_query_root_refusals.py` `tests/test_query_capture_retry.py` `tests/test_query_target_route.py`

## Recovery scan and resync

Does: K36 recovery inventory: pages of a group's segments, chunks and tickets that place a fragment on a returning node (local and remote scan, never forwarded), the recovery proof and mark, and the W145 resync that marks a group to reload from its leader's snapshot. Also the check that an inode is really reaped.

State: `struct recovery_page` per call; the resync request itself lives in the raft core (`efs_raft_request_resync`, under `h->mu`).

Entry points: `server_raft_host_resync` `server_raft_host_recovery_group` `server_raft_host_recovery_prove` `server_raft_host_recovery_scan` `server_raft_host_recovery_scan_serve` `server_raft_host_recovery_mark` `server_raft_host_inode_reaped`

Functions: `server_raft_host_resync` `server_raft_host_recovery_group` `server_raft_host_recovery_prove` `recovery_rec_len` `recovery_segment_cb` `recovery_chunk_cb` `recovery_ticket_cb` `host_recovery_scan_local` `host_recovery_scan_remote` `server_raft_host_recovery_scan` `server_raft_host_recovery_scan_serve` `server_raft_host_recovery_mark` `server_raft_host_inode_reaped`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_k36_recovery.py` `tests/test_k36_segment_recovery.py` `tests/test_resync_admission.py` `tests/test_mgmt_resync_busy.py` `tests/test_stale_recovery.py`

## Packer support

Does: Read-side support for the W125 packer and reclaim: the packed-object state under a ReadIndex, the degraded mask beside it, leader and proposal entry points, and the remote variants for a node that does not lead.

State: none.

Entry points: `server_raft_host_pack_object_serve` `server_raft_host_pack_state` `server_raft_host_pack_object` `server_raft_host_pack_leader` `server_raft_host_pack_propose`

Functions: `host_pack_degraded_mask` `host_pack_object_remote` `server_raft_host_pack_object_serve` `server_raft_host_pack_state` `server_raft_host_pack_object` `server_raft_host_pack_leader` `server_raft_host_pack_propose`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_pack_object_remote.py` `tests/test_pack_base_reads.py` `tests/test_reclaim.c`

## Observability and status

Does: `server_raft_host_status` and `server_raft_host_expiry_status` build the status page from the cores and counters; the observation helpers render pump phase maxima, role events and KV table activity off the pump from a bounded numeric snapshot (`host_periodic_capture`); `wprof_*` record write-phase profile histograms.

State: `host_periodic`, `host_tx_obs` and `host_role_ev` mailbox structs (the profile histograms `g_wprof[]` are in the first concern); `h->mu` for the status snapshot, `h->outbox_mu` for the outbox counters copied by the capture.

Entry points: `server_raft_host_expiry_status` `server_raft_host_status`

Functions: `wprof_now` `wprof_add` `wprof_tick` `wprof_verdict` `host_obs_roles` `host_obs_roles_report` `host_obs_render` `host_periodic_capture` `host_kv_obs_dump` `host_periodic_render` `server_raft_host_expiry_status` `server_raft_host_status`

Logs: no keyed lines in [diagnostics](diagnostics.md).

Tests: `tests/test_k96_status_slots_live.py` `tests/test_b_geometry_status.py` `tests/test_status_version.py` `tests/test_gc_status_expiry.py` `tests/test_report_log_coverage.py`

## Lifecycle and submit dispatch

Does: Starts and stops the host (threads, replicas, senders, workers, GC, query), steps a leader down or transfers leadership, and is the single `submit` entry: `server_raft_host_submit` decodes the message tag, forwards or serves it, and calls the handler. The PUT ticket batch submit and the KV accessor live here.

State: `g_host` (the singleton: release-stored at start, set to NULL on a failed start and at stop); the thread ids `tid`, `query_tid`, `gc_tid`, `fin_tid`, `mb_tid`; the stop path takes `h->fin_mu`, `h->inbox_mu`, `h->mb_mu`, `h->mu`, `h->query_lock` and `h->wait_mu` in turn (not nested).

Entry points: `server_raft_host_start` `server_raft_host_stop` `server_raft_host_submit` `server_raft_host_step_down` `server_raft_host_gc_running` `server_raft_host_kv`

Functions: `server_raft_host_start` `server_raft_host_stop` `host_put_ticket_batch_submit` `server_raft_host_submit` `server_raft_host_step_down` `server_raft_host_gc_running` `server_raft_host_kv`

Logs: `raft-host` lines from `server_raft_host_start`, `server_raft_host_submit`, `server_raft_host_step_down`

Tests: `tests/test_shutdown_bounded.py` `tests/test_forward_once.py` `tests/test_submit_unknown_tag.py` `tests/test_report_stepdown.py`

## Lock order

Orders below were read from the code: an order is listed only when one function takes both locks, or when the inner lock's function is called with the outer held (named in the line). Everything else is "not determined"; this page does not prove that no other nesting exists, and the file states its order in no one place.

- `h->mu` then `h->snap_mu`: `host_query_source_once` and `host_query_thread` (the field comment says the pump may hold `h->mu` then `snap_mu`, and that the GC thread never takes `h->mu` under `snap_mu`).
- `h->mu` then `h->inbox_mu`: `host_read_control` (a trylock on `h->mu` first, then the inbox).
- `h->mu` then `view_mu` (group view): `host_publish_view` (`h->mu` held by the caller; `view_mu` guards only the atomic stores).
- `h->mu` then `cv_mu` then `view_mu` (group view): `host_pump` calls `host_waiters_wake` with `h->mu` held; the wake holds `cv_mu` while it scans and `host_view_get` may take `view_mu` after 64 odd loads (K107). The three wait brackets (`host_read_index_admitted`, `host_wait_applied_mode`, `host_publication_committed`) take `cv_mu` through `group_view_wait_lock` without `h->mu`, never nested with it; `host_view_get` inside the bracket can take `view_mu`.
- `h->mu` then `h->outbox_mu`: `host_send` (queues under the pump's `h->mu`, per the field comment); the sender threads take `h->outbox_mu` alone.
- `h->mu` then `h->rec_mu`: `host_rec_mark` is set from the apply path under `h->mu` (field comment); `host_txn_recover_pass` takes `h->rec_mu` alone on the GC thread.
- `h->mu` then `h->wait_mu`: `apply_session_cmd` and `host_lease_close_effects` (apply handlers run under `h->mu`); `host_lock_wait` and `host_lock_wait_drop_session` take `h->wait_mu` alone.
- `h->mu` then `mview_mu`: `host_learn_membership` and `host_seed_propose` call `mview_publish`/`mview_publish_store` with `h->mu` held. The reverse is not taken: `membership_copy` releases `mview_mu` before it takes `h->mu`.
- `host_append_locks[i]` then `h->mu`: `server_raft_host_append` holds a stripe (trylock) across `host_append_owner`, which calls `host_append_abandon` or `host_append_transaction` (the prepare, decide and resolve proposals); `h->mu` is taken inside those proposals (`host_propose_impl` locks it). `host_append_primary` runs before the stripe is taken.
- `c->mu` (`g_ri_coal`) is dropped before `host_remote_cmd` is called in `host_remote_read_index`; `h->mb_mu` is dropped before the calls in `host_member_thread`; `host_gc_metadata_waiting` asks `group_view_waiting` (which takes and drops `cv_mu`) and then takes `h->s->lock`, one after the other.
- Not determined: the locks held by the callers of `host_fin_add` (it takes `h->fin_mu`), of `peer_addr` (`h->s->lock`), and of `host_report_session_parallel` (a trylock in `host_report_session_admit`).
