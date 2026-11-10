# Write path map: `src/client/write.c`

Concern map of `src/client/write.c` (9,812 lines at the time of writing). Each function of the file is listed under exactly one concern; `scripts/check_concern_map.py` (run by `make docs-check`) fails when a function is missing, unknown or in two concerns. There are no line numbers here: find a function with `grep -n "^static.* <name>(" src/client/write.c`. For a defect, read the Logs line first, then open the concern.

Mutexes in this file: `g_client.lock` (table), `g_client.idx_mu`, `g_client.dirty_mu`, `g_client.meta_flush_mu` (all declared in `client_internal.h`), `g_dcache.shard[]` (`dcache_mu`), `g_dcache.shard_io[]` (`dcache_io_mu`), `g_dcache_pin_mu`, `put_win_mu`, `g_putid_mu`, `g_dgobj_mu`, `g_unit_mu`, `g_wb_error_mu`, `g_reclaim.mu`, `g_put_sh[].mu`, `g_put_init`. The order is in the last section.

## PUT pool and write entry

Does: `efs_client_write` splits one FUSE write into chunk jobs, runs them through `client_write_run` and the persistent PUT worker pool (`put_pool_run` enqueues, `put_pool_thread` drains, `chunk_put_worker` stores one chunk into the dcache or PUTs it), then returns the first error. `efs_client_write_no_replicate` is the same path with replication off.

State: `g_put_sh[]` (per-worker FIFO of `chunk_put_job`, each with its own `mu`/`cv`); `g_put_pool` (thread ids, `ready`, `shutdown`, `rr`) started once under `g_put_init`.

Entry points: `efs_client_write`, `efs_client_write_no_replicate`. Also called from other files: `efs_client_write` `efs_client_write_no_replicate` `put_pool_run`

Functions: `put_pool_thread` `put_pool_ensure` `put_pool_run` `chunk_put_worker` `write_chunks_no_replicate` `client_write_run` `efs_client_write` `efs_client_write_no_replicate`

Logs: none of its own; failures surface as the return code of the write.

Tests: `tests/test_put_pool_budget.py` `tests/test_k88_chunk_hints.c` `tests/test_create_credentials.py` `tests/test_client_memory.py` `tests/test_k88_chunk_hints_live.py`

## Dcache core

Does: A fixed table of 65536 slots in 64 shards holds the assembled chunk bodies (`struct dcache_ent`: data, dirty ranges, base generation, snapshot and object sequence, pin flag). This concern owns slot lookup, the per-shard dirty lists and slot index bitmaps, the snapshot sequence counter, the trace switch and the per-inode pin table that keeps a staged row from being evicted while a dirty body exists.

State: `g_dcache` (`shard[]` via `dcache_mu`, `shard_io[]` via `dcache_io_mu`, `e[]`, `dirty_head[]`, `pressure_slots` and `chain_slots` under the slot's shard lock, `dirty_bytes` atomic); `g_dcache_seq` atomic; `g_dcache_pin_keys`/`counts`/`mask`/`count` under `g_dcache_pin_mu`; `g_dcache_once`.

Entry points: `efs_dcache_init`, `efs_dcache_ino_pinned`, `efs_dcache_trace_on`. Also called from other files: `efs_dcache_init` `efs_dcache_ino_pinned` `efs_dcache_trace_on` `now_ns`

Functions: `now_ns` `dcache_seq_next` `dcache_seq_now` `dcache_trace_on` `efs_dcache_trace_on` `dcache_trace_ranges` `dcache_pin_ensure` `dcache_pin_add` `dcache_pin_release` `efs_dcache_ino_pinned` `dcache_note_dirty_bytes` `dcache_init` `dcache_ensure` `efs_dcache_init` `dcache_slot` `dcache_mu` `dcache_io_mu` `dcache_index_set` `dcache_dirty_link` `dcache_dirty_unlink` `dcache_set_dirty` `dcache_find` `dcache_metadata_idle` `dcache_chain_reuse` `dcache_find_meta` `dcache_keep_on_drop` `dcache_typed_admission_idle` `dcache_drop_locked` `dcache_apply_init`

Logs: `EFS_DCACHE_TRACE=1` prints one `dcache ino= ci=` line per slot transition (`dcache_trace_ranges`); `client: sparse metadata small-page hint unavailable`.

Tests: `tests/test_client_memory.py` `tests/test_writer_fence_merge.py` `tests/test_fold_observation.py` `tests/test_load_merge_race.py` `tests/test_k38_stale_span_replay.py`

## Dcache store and read

Does: Writes land here: a write is merged into the slot body (`dcache_add_range`, `dcache_patch*`, `dcache_store*`, sparse variants for slots without a base, full-chunk store), the slot gives up buffer budget when asked (`efs_dcache_yield_extra`), and reads copy or overlay the dirty body (`efs_dcache_copy*`, `efs_dcache_overlay*`). Fetched images are installed and folded by `dcache_install_image` and `dcache_fetch_merge_base`; `chunk_obs_*` decide whether a fetched chunk row has live spans or is masked by a truncate.

State: slot fields of `g_dcache.e[]` under the slot's `dcache_mu`; the local chunk map is read under `g_client.idx_mu`, taken outside the slot lock (never under it).

Entry points: `efs_dcache_try_patch`, `efs_dcache_try_patch_sparse`, `efs_dcache_store_full_owned`, `efs_dcache_admit_lane_write`, `efs_dcache_write_lane`, `efs_dcache_copy`, `efs_dcache_overlay`, `efs_dcache_drop`, `efs_dcache_drop_if_clean`, `efs_dcache_has`, `efs_dcache_get`. Also called from other files: `dcache_image_current` `dcache_note_size` `efs_dcache_admit_lane_write` `efs_dcache_bind_writer` `efs_dcache_copy` `efs_dcache_copy_kept` `efs_dcache_copy_unpub` `efs_dcache_drop` `efs_dcache_drop_if_clean` `efs_dcache_get` `efs_dcache_has` `efs_dcache_overlay` `efs_dcache_overlay_lane` `efs_dcache_retire_clipped` `efs_dcache_store_full_owned` `efs_dcache_trim_metadata` `efs_dcache_try_patch` `efs_dcache_try_patch_sparse` `efs_dcache_write_lane` `efs_dcache_yield_extra`

Functions: `chunk_obs_has_live_spans` `chunk_obs_masked` `efs_dcache_trim_metadata` `dcache_full_overwrite` `efs_dcache_has` `efs_dcache_get` `dcache_range_covered` `dcache_unpublished` `dcache_image_current` `efs_dcache_copy` `efs_dcache_copy_kept` `efs_dcache_copy_unpub` `efs_dcache_overlay` `efs_dcache_overlay_lane` `efs_dcache_retire_clipped` `efs_dcache_bind_writer` `efs_dcache_write_lane` `efs_dcache_drop` `efs_dcache_drop_if_clean` `dcache_store_owned_locked` `dcache_merge_owned` `dcache_add_range` `dcache_patch_full` `dcache_patch` `dcache_load_and_patch` `dcache_note_size` `dcache_store_full_chunk` `dcache_store_full_owned_run` `efs_dcache_store_full_owned` `dcache_try_patch_run` `efs_dcache_try_patch` `dcache_patch_sparse` `dcache_load_sparse` `dcache_try_patch_sparse_run` `efs_dcache_try_patch_sparse` `dcache_fill` `dcache_take` `dcache_account_extra` `efs_dcache_yield_extra` `dcache_store` `efs_dcache_admit_lane_write` `dcache_install_image` `dcache_body_to_rdcache` `dcache_img_to_rdcache` `dcache_fetch_merge_base` `span_of`

Logs: none.

Tests: `tests/test_client_memory.py` `tests/test_write_acceptance_gate.py` `tests/test_load_merge_race.py` `tests/test_k38_stale_span_replay.py` `tests/test_writer_fence_merge.py`

## Flush pipeline

Does: Turns dirty bodies into PUTs. `dcache_steal_dirty*` copies a slot out under the slot locks (taking `shard_io` first), `dcache_put_now*` assembles and sends the chunk and records the object, `dcache_flush_slot*`/`dcache_flush_ino_pass` walk an inode's dirty chunks, and `flush_pipe_*` batch several PUTs in flight. `efs_dcache_flush_ino` is what fsync, close and the REPORT driver call; it waits for the inode's PUT windows first.

State: `flush_pipe` locals only; slot state under `dcache_io_mu` then `dcache_mu`. `g_dcache.dirty_bytes` atomic.

Entry points: `efs_dcache_flush_ino`, `efs_dcache_flush_pressure_ino`, `efs_dcache_flush_all`, `efs_dcache_drain_pending`, `efs_dcache_discard_dirty`, `efs_dcache_pending_records`, `efs_dcache_pressure_ino`. Also called from other files: `dcache_flush_ino_pass` `dcache_put_now` `dcache_steal_dirty` `efs_dcache_discard_dirty` `efs_dcache_drain_pending` `efs_dcache_flush_all` `efs_dcache_flush_ino` `efs_dcache_flush_pressure_ino` `efs_dcache_pending_records` `efs_dcache_pressure_ino`

Functions: `dcache_put_now` `dcache_put_now_budgeted` `efs_dcache_pressure_ino` `dcache_steal_dirty_budgeted` `dcache_steal_dirty` `dcache_flush_keep` `dcache_retire_gone` `flush_pipe_init` `flush_pipe_drain` `flush_pipe_add` `dcache_log_discard` `efs_dcache_pending_records` `dcache_chain_has` `dcache_flush_slot_inner_budgeted` `dcache_flush_slot_inner` `dcache_flush_slot` `dcache_flush_all_slots` `cmp_u32` `dcache_dirty_cis_of` `dcache_flush_ino_pass` `efs_dcache_flush_pressure_ino` `efs_dcache_flush_ino` `cmp_ino` `efs_dcache_drain_pending` `efs_dcache_discard_dirty` `efs_dcache_flush_all`

Logs: `dcache-flush-fail ino= stage=put|put-window`, `dcache-inode-gone` (record a write-back error and drop the bytes), `force-discard`.

Tests: `tests/test_pressure_cohort.py` `tests/test_client_memory.py` `tests/test_writer_fence_merge.py` `tests/test_fold_observation.py` `tests/test_stop_dirty_drain.py`

## PUT window

Does: Counts, per inode, the PUTs that have already marked a slot clean but not yet recorded the chunk in a dirty set. During that gap a REPORT snapshot sees nothing dirty. The flusher waits for the inode's windows to close (`put_win_wait`, bounded by the RPC deadline) and readers wait via `efs_dcache_put_win_wait` so a stolen body is never read as a hole.

State: `put_win[]`, `put_win_overflow` under `put_win_mu`/`put_win_cv`; `put_win_total` atomic (lock-free fast path for readers).

Entry points: `efs_dcache_put_win_wait`.

Functions: `put_win_open` `put_win_close` `put_win_busy_locked` `put_win_wait` `efs_dcache_put_win_wait`

Logs: none.

Tests: `tests/test_put_window_deadline.py` `tests/test_writer_fence_merge.py` `tests/test_read_failure_admission.py` `tests/test_read_refs_window.py` `tests/test_client_memory.py`

## Dcache publication hand-off

Does: The writer-state (D25) side of publication: snapshot a lane of the body, begin and finish a publication, report the result and consume it. These calls move a slot between 'accepted', 'sent' and 'published' for epoch-owned writers.

State: slot and `efs_writer_state` fields under the slot's `dcache_mu`; no file-scope state.

Entry points: `efs_dcache_snapshot_lane`, `efs_dcache_snapshot_publication`, `efs_dcache_begin_publication`, `efs_dcache_finish_publication`, `efs_dcache_publication_report`, `efs_dcache_complete_publication`, `efs_dcache_publication_request`, `efs_dcache_consume_publication`, `efs_dcache_publication_result`. Also called from other files: `efs_dcache_begin_publication` `efs_dcache_complete_publication` `efs_dcache_consume_publication` `efs_dcache_finish_publication` `efs_dcache_publication_report` `efs_dcache_publication_request` `efs_dcache_publication_request_ticketed` `efs_dcache_publication_result` `efs_dcache_snapshot_lane` `efs_dcache_snapshot_publication`

Functions: `dcache_snapshot_lane` `efs_dcache_snapshot_lane` `efs_dcache_snapshot_publication` `efs_dcache_begin_publication` `efs_dcache_finish_publication` `efs_dcache_publication_report` `efs_dcache_complete_publication` `efs_dcache_publication_request_ticketed` `efs_dcache_publication_request` `efs_dcache_consume_publication` `efs_dcache_publication_result`

Logs: none.

Tests: `tests/test_client_memory.py` `tests/test_publication_writeback.py`

## Reclaim

Does: Background drain once dirty bytes cross the limit: writers only kick (`efs_dcache_maybe_reclaim`); up to 16 workers (`dcache_reclaim_main`) sweep slots round-robin until the dirty total is under the low-water mark, and park after an empty sweep.

State: `g_reclaim` (`mu`, `cv`, `cursor`, `lim`, `kicks`, `empty_kicks`, `complete`, `waiters`, `active`, `shutdown`) under `g_reclaim.mu`; `g_reclaim_once`.

Entry points: `efs_dcache_maybe_reclaim`, `efs_dcache_reclaim_stop`.

Functions: `dcache_reclaim_limit` `reclaim_now_ns` `dcache_reclaim_main` `reclaim_kick` `dcache_reclaim_start` `dcache_kick_complete` `efs_dcache_maybe_reclaim` `efs_dcache_reclaim_stop`

Logs: none.

Tests: `tests/test_client_memory.py`

## Fragment PUT send

Does: Encodes a chunk into fragments, hashes them and sends them to the stripe members in parallel (`put_fragments_parallel_once` sends and polls, `put_fragments_parallel_run` retries, re-places on full or unreachable members and decides the verdict). `efs_client_put_fragments_parallel` is the public wrapper.

State: no file-scope state; per-call buffers.

Entry points: `efs_client_put_fragments_parallel`. Also called from other files: `data_chunk_size` `data_frag_size` `efs_client_put_fragments_parallel` `frag_ptrs`

Functions: `data_chunk_size` `data_frag_size` `frag_ptrs` `put_fragments_parallel_once` `put_fragments_parallel_run` `efs_client_put_fragments_parallel` `assemble_write_chunk` `hash_write_fragments`

Logs: `put-attempt`, `put_fragments ... only N/M peers live`, `... EC needs 3 distinct nodes`, `... member(s) ... unreachable/refused; placing again`, `put quota backpressure`, `put ino= ci= seq= rc=`.

Tests: `tests/test_put_retry_budget.py` `tests/test_get_integrity.py` `tests/test_wb_runtime.py` `tests/test_fold_observation.py` `tests/test_fence_read.py`

## PUT replies and degraded acknowledgements

Does: Reads each member's PUT reply, maps the replies to one verdict (`put_drain_verdict`: STALE, QUOTA, ack count, BUSY past the deadline, NET, NO_QUORUM) and keeps the table of objects that two of three members acknowledged, so REPORT can mark the stripe degraded (`EFS_CHUNK_REC_F_MISSING`).

State: `g_dgobj`, `g_dgobj_mask`/`live`/`used` under `g_dgobj_mu`; a fault-build collision injector with its own static mutex.

Entry points: `efs_client_degraded_flags`, `efs_client_degraded_drop`, `efs_client_degraded_pending`.

Functions: `put_reply_fault` `put_recv_reply` `dgobj_hash` `dgobj_find` `dgobj_rehash` `dgobj_note` `efs_client_degraded_flags` `efs_client_degraded_drop` `efs_client_degraded_pending` `degraded_ack_log` `put_drain_verdict` `put_reply_pollfds` `degraded_why` `degraded_record`

Logs: `put-reply fault` (fault builds), `put_fragments ... degraded acknowledgement`, `put_fragments: N more degraded acknowledgements` (capped at 100/s).

Tests: `tests/test_put_retry_budget.py` `tests/test_w155_report_resolve.py` `tests/test_report_frame_boundary.py` `tests/test_put_reply_deadline.py` `tests/test_publication_rpc.py`

## Node notes and rerouting

Does: Per-member marks used when placing a stripe: full (`PUT_FULL_MARK_MS` after a QUOTA refusal), away (`PUT_AWAY_MARK_MS` after a failed write; an acknowledged fragment clears it) and the spare re-probe interval. `reroute_untried_fragments` re-places fragments on a spare when a member did not answer.

State: `g_node_full_until_ms[]`, `g_node_away_until_ms[]`, `g_node_spare_probe_ms[]`, all accessed with relaxed atomics, no mutex.

Entry points: none outside the file. Also called from other files: `spare_reprobe`

Functions: `node_note_full` `node_is_full` `node_note_away` `node_note_back` `node_is_away` `spare_reprobe` `reroute_untried_fragments` `put_quota_has_spares`

Logs: none.

Tests: `tests/test_put_retry_budget.py` `tests/test_k35_placement.py`

## PUT ids and ticket ledger

Does: Keeps one record per (ino, chunk) of the last PUT that is not yet published: object generation, placement, checksums, span, ticket (`put_seq`). REPORT is built from it, and it is dropped when a REPORT carrying it succeeds. `g_cbase` remembers the committed base per chunk.

State: `g_putid`, `g_putid_mask`/`live`/`used` and `g_cbase[]`, all under `g_putid_mu`.

Entry points: `efs_client_putid_owns`, `efs_client_put_referenced`.

Functions: `putid_hash` `putid_find` `putid_rehash` `putid_insert_slot` `putid_note` `rec_ticket` `putid_fill` `efs_client_putid_owns` `putid_drop` `putid_drop_snapshot` `putid_miss_note` `report_fill_owned` `cbase_slot` `cbase_note` `cbase_apply` `cbase_reconcile` `efs_client_put_referenced`

Logs: `efs: putid table NOMEM`, `efs: report rec ... rejected unowned staging observation`.

Tests: `tests/test_k38_stale_span_replay.py` `tests/test_report_span_base.py` `tests/test_stale_recovery.py` `tests/test_report_orphan.py` `tests/test_k38_swept_hint_live.py`

## Dirty sets and mtime pins

Does: One `ino_dirty` record per inode holds the chunk marks and the size/mtime mark since the last REPORT. `efs_client_mark_chunk_dirty`/`mark_ino_dirty` add marks, `efs_client_ino_is_dirty` and `efs_client_ino_has_unpublished_data` read them, and the mtime pin table holds back a client-set mtime until the next write.

State: `g_idirty[]`, `g_idirty_n`, `mtime_pin[]`, `mtime_pin_n` under `g_client.dirty_mu`.

Entry points: `efs_client_mark_chunk_dirty`, `efs_client_mark_ino_dirty`, `efs_client_ino_is_dirty`, `efs_client_ino_has_unpublished_data`, `efs_client_mtime_pin`, `efs_client_mtime_unpin`, `efs_client_mtime_is_pinned`, `efs_client_dirty_sets_free`. Also called from other files: `efs_client_dirty_sets_free` `efs_client_ino_has_unpublished_data` `efs_client_ino_is_dirty` `efs_client_ino_is_dirty_locked` `efs_client_mark_chunk_dirty` `efs_client_mark_ino_dirty` `efs_client_mtime_is_pinned` `efs_client_mtime_pin` `efs_client_mtime_unpin`

Functions: `pack_dirty_chunk` `dirty_set_ensure` `dirty_set_put` `dirty_set_has` `idirty_find` `idirty_get` `idirty_free` `dirty_sets_clear` `efs_client_dirty_sets_free` `efs_client_ino_is_dirty` `efs_client_ino_is_dirty_locked` `efs_client_ino_has_unpublished_data` `efs_client_mtime_pin` `efs_client_mtime_unpin` `efs_client_mtime_is_pinned` `efs_client_mark_ino_dirty` `efs_client_mark_chunk_dirty`

Logs: `mtime-pin: table full`.

Tests: `tests/test_report_pressure.py` `tests/test_inode_pending_eof.py` `tests/test_pub_units.py` `tests/test_fold_observation.py` `tests/test_client_metadata_authority.py`

## Dirty snapshots

Does: A REPORT detaches an inode's marks into a `dirty_snap` and merges them back if the REPORT fails, so no mark is lost.

State: `struct dirty_snap` is local to the REPORT run; the records it takes from are under `g_client.dirty_mu`.

Entry points: none outside the file.

Functions: `dirty_snap_take_record` `dirty_snap_save_locked` `dirty_snap_free` `dirty_snap_remark_locked`

Logs: none.

Tests: `tests/test_report_pressure.py`

## REPORT admission and publication slots

Does: Bounds the REPORT workload: a request reserves records/bytes (`efs_client_report_admit`, default 8192 records, 16 MiB, 8000 ms wait) and an inode takes one of 64 publication slots (`pub_take_locked`) before its marks are detached; `write_accept_begin/end` stop a write from installing a body while its inode is being published.

State: `pub_slots[]`, `pub_cv`, `report_reserved`, `report_waits`, `report_timeouts`, `report_limit_*` under `g_client.dirty_mu`; `report_request_records` is thread-local.

Entry points: `efs_client_report_admit`, `efs_client_report_unreserve`, `efs_client_report_pressure_stats`. Also called from other files: `efs_client_report_admit` `efs_client_report_pressure_stats` `efs_client_report_unreserve` `report_landed_note`

Functions: `pub_has_locked` `pub_full_locked` `pub_take_locked` `pub_release` `write_accept_begin` `write_accept_end` `report_clock_ms` `report_pressure_locked` `efs_client_report_pressure_stats` `efs_client_report_admit` `efs_client_report_unreserve` `report_landed_note`

Logs: `report-pressure`, `report-admission timeout`, `report-landed`.

Tests: `tests/test_report_pressure.py` `tests/test_write_acceptance_gate.py` `tests/test_opid_admission.py` `tests/test_committed_directory_stage.py` `tests/test_create_errors.py`

## REPORT driver

Does: Sends the detached marks to the server as REPORT_CHUNKS: `report_dirty_ino_run` flushes the inode, builds chunk and inode records, sends them, handles STALE/BUSY rounds and the stuck bound (`report_streak_*`), and `report_dirty_ino_complete` retires the publication slot and the PUT ids.

State: none at file scope (`report_streak` is per call); the table lock, directory lock and `g_client.idx_mu` are taken through `report_snapshot_lock`, then `g_client.dirty_mu`.

Entry points: `efs_client_report_dirty`, `efs_client_report_dirty_ino`, `efs_client_report_pressure_ino`.

Functions: `efs_client_report_dirty` `report_fault_withhold` `report_send_records` `report_retry_pause` `report_pubid_mint` `report_snapshot_lock` `report_snapshot_unlock` `report_streak_cmp` `report_streak_find` `report_streak_same` `report_streak_note` `report_rec_stuck` `report_dirty_ino_run` `report_dirty_ino_complete` `efs_client_report_dirty_ino` `efs_client_report_pressure_ino`

Logs: `report ino= ci=`, `report-irec`, `report-loop`, `report-stuck`, `report-slot-timeout`, `report-sync-fail ino= stage=`, `efs: report ino= server size below dirty`.

Tests: `tests/test_k38_report_bound.py` `tests/test_report_resolved_retry.py` `tests/test_pressure_cohort.py` `tests/test_report_snapshot_locks.py` `tests/test_report_pressure.py`

## Stale replay

Does: When the server answers STALE, the chunk is re-pulled and this client's ranges are replayed onto the committed base (`dcache_replay_stale_ex*`, in parallel by `replay_fan_*`); `stale_*` decide which records to resend and `replay_span_without_body` handles a span whose body is gone.

State: slot fields under `shard_io` then `dcache_mu`; `replay_fan` has a per-run mutex.

Entry points: `report_replay_stale`, `dcache_replay_stale_ex`.

Functions: `stale_pair_cmp` `report_mono_ms` `replay_fan_thread` `replay_fan_run` `stale_sent_cmp` `span_refused` `stale_retry_owned` `stale_repull_replay` `report_replay_snapshot` `report_replay_stale` `replay_span_without_body` `dcache_replay_stale_ex_budgeted` `dcache_replay_stale_ex`

Logs: `dcache ... stale-class`, `report-stale`, `report-replay`, `report-span-lost`, `dcache ... replay-span`, `dcache ... replay committed=`.

Tests: `tests/test_k38_stale_span_replay.py` `tests/test_stale_recovery.py` `tests/test_report_resolved_retry.py` `tests/test_replay_fan_budget.py` `tests/test_k38_report_bound.py`

## Publication units

Does: A unit is the closed interval of chunk indices one multi-chunk write touched, merged with older unpublished units it overlaps, so the whole write becomes visible in one REPORT. This concern also holds the per-chunk publication-cycle verdict (`dcache_cycle_verdict`: contention, stalled), identity abandon and committed-object bookkeeping.

State: `g_units[]`, `g_unit_version` under `g_unit_mu` and `g_unit_cv`; `unit_sort_recs` is thread-local.

Entry points: `efs_client_unit_begin`, `efs_client_unit_end`.

Functions: `dcache_cycle_verdict` `dcache_object_of` `dcache_note_committed` `dcache_identity_abandon` `report_units_shape` `report_retry_without_identity` `dcache_pending_of` `dcache_base_gen_of` `dcache_object_seq_of` `unit_find` `unit_range` `efs_client_unit_begin` `efs_client_unit_end` `unit_wait_idle` `unit_chunk_state` `unit_ord_cmp` `unit_rec_find` `report_units_published` `report_units_remark` `dcache_cycle_sent`

Logs: `publish-contention`, `publish-stalled` (records a write-back error).

Tests: `tests/test_pub_units.py` `tests/test_k38_stale_span_replay.py` `tests/test_wb_runtime.py` `tests/test_stale_recovery.py` `tests/test_report_span_base.py`

## Export chunks

Does: Helpers over the local chunk map (`g_client.export`): does a chunk exist, copy its row, reserve rows, forget a row the server never saw.

State: `g_client.export` under `g_client.idx_mu`.

Entry points: none outside the file.

Functions: `export_chunk_exists` `export_chunk_copy` `export_reserve_chunks_locked` `export_chunk_gen_of` `export_forget_unreported`

Logs: none.

Tests: `tests/test_fold_observation.py` `tests/test_writer_fence_merge.py` `tests/test_k38_stale_span_replay.py` `tests/test_stale_recovery.py` `tests/test_span_selection.py`

## Meta flush batching

Does: The flush pool: `efs_client_note_meta_change` counts dirty operations and, past the threshold (or when an inode crossed the landed-PUT count), `meta_flush_enqueue` queues the inode; `meta_flush_main` threads pop it and run the blocking REPORT off the FUSE worker threads. Duplicate kicks for one inode coalesce.

State: the queue and stop flag in `g_client` (`meta_flush_qn`, `meta_flush_stop`, `meta_flush_ready`) under `g_client.meta_flush_mu`/`meta_flush_cv`; `meta_dirty` and `meta_dirty_ops` under `g_client.dirty_mu`.

Entry points: `efs_client_enable_meta_batch`, `efs_client_stop_meta_flush`, `efs_client_note_meta_change`.

Functions: `meta_flush_enqueue` `meta_flush_main` `efs_client_enable_meta_batch` `efs_client_stop_meta_flush` `efs_client_note_meta_change`

Logs: none.

Tests: `tests/test_meta_flush_batch.py` `tests/test_flock_description_release.py` `tests/test_report_pressure.py` `tests/test_put_drain.py` `tests/test_write_acceptance_gate.py`

## Write-back error state

Does: Records a write-back error for an inode when its bytes could not be published (`wb_error_record_add`, from `report-stuck`, `dcache-inode-gone`, `publish-stalled`, `report-span-lost`); the error is reported once per open file description (`efs_wb_description_sync`) and cleared when a later publish succeeds (`wb_error_record_resolve`). This is where a `close()` or `fsync()` gets its EIO.

State: `g_wb_errors[]` and every `wb_inode_error` under `g_wb_error_mu`.

Entry points: `efs_wb_description_open`, `efs_wb_description_close`, `efs_wb_description_sync`, `efs_wb_inode_stalled`.

Functions: `wb_error_get` `wb_error_retire` `efs_wb_description_open` `efs_wb_description_close` `efs_wb_description_sync` `efs_wb_inode_stalled` `wb_error_record_add` `wb_error_record_resolve`

Logs: none of its own.

Tests: `tests/test_k88_chunk_hints.c` `tests/test_wb_runtime.py` `tests/test_flock_description_release.py` `tests/test_k38_report_bound.py` `tests/test_client_memory.py`

## Lock order

Outer lock first. Only pairs held together in this file are listed; each is shown by one function.

1. `g_client.lock`, then the directory lock, then `g_client.idx_mu`, then `g_client.dirty_mu`: `report_dirty_ino_run` (`report_snapshot_lock`, then `dirty_mu`). The wait for a free publication slot drops both before sleeping.
2. `g_client.dirty_mu`, then `g_dcache_pin_mu`: `efs_client_ino_has_unpublished_data` (via `efs_dcache_ino_pinned`).
3. `dcache_io_mu` (`shard_io`), then `dcache_mu` (`shard`): `dcache_flush_slot_inner_budgeted`. Both are dropped before a fragment GET or PUT; `dcache_replay_stale_ex_budgeted` takes them in the same order.
4. `dcache_mu`, then `g_dcache_pin_mu`: `dcache_pin_add` and `dcache_pin_release` run with the slot lock held; nothing takes them in reverse.
5. `g_client.idx_mu` is never taken under `dcache_mu`: `export_chunk_gen_of` is called before the slot lock for that reason.
6. `g_putid_mu` is released before `dcache_mu` is taken: `efs_client_put_referenced`.
7. `g_unit_mu` should come before the putid and dcache locks (comment above `struct unit_span`); no function of this file holds it while taking them, so the order is stated, not exercised: not determined beyond the comment.
8. `put_win_mu`, `g_dgobj_mu`, `g_wb_error_mu`, `g_reclaim.mu` and the `g_put_sh[]` queue mutexes are taken alone: none of their functions takes a second mutex while holding one.

