# src/meta/meta_apply/ — the metadata apply (split from `meta_apply.c`, Task DL)

The replicated metadata state machine: every metadata command a Raft group
applies, and the reads its handlers serve, over the KV it is handed. Public
callers include [`efs/meta_apply.h`](../../../include/efs/meta_apply.h) and
nothing in this directory.

Find the area your symptom names in the table, then read its header first:
each `<area>.h` opens with that area's contract (what it owns, which lock
protects what, the lifetime of its objects, what callers get on refusal and
failure), then what it exports inside the module. `apply_internal.h` holds
only what every file shares: the include block, the KV value sizes and five
byte helpers. A `.c` file includes only the headers of the areas it calls, so
the includes at its top are its dependency list. An area without a header
exports nothing inside the module. The concatenation order used by
source-splicing tests is `MODULES["meta_apply"]` in
[`tests/csrc.py`](../../../tests/csrc.py); it is a transition mechanism, not
a contract.

## Files

Symptom → file → tests (`python3 scripts/tests_for.py meta/meta_apply.<file>`):

| Symptom or change | File | Header | Holds | Tests |
|---|---|---|---|---|
| A write refused BUSY by a prepared EXCL image or a staged publication; a malformed inode, lane, dentry, fence-history, lane-authority or chunk value | `rows.c` | `rows.h` | `meta_write_*` (the checked KV writes), `struct lane_rec`, the row codecs | bare tag |
| Inode or dentry reads, LOOKUP, RESOLVE, path walks, the writer view of an inode | `lookup.c` | `lookup.h` | — | bare tag |
| Inode number allocation, the ALLOC key, opid replay records, mkfs/init, salt, membership | `init.c` | `init.h` | — | bare tag |
| CREATE, MKDIR (local, log, lease, pending and expiring paths), create refusal | `create.c` | — | — | bare tag |
| Extended attributes | `xattr.c` | `xattr.h` | — | bare tag |
| UNLINK, expiry policy and renew, LINK, RENAME, SETATTR | `namespace.c` | `namespace.h` | — | bare tag |
| READDIR (bounded, transactional, hashed and splitting directories), RMDIR | `readdir.c` | — | the `EFS_FAULTS` READDIR layout pause | `.readdir` |
| Inode reclaim and orphan reclaim | `reclaim.c` | — | — | bare tag |
| GC and reap records, the pending-GC watermark, degraded marks (W154) and their count, recovery marks (K36), chunk reference scans | `gc.c` | `gc.h` | `gc_wm_*` under `gc_wm_mu`, `dg_wm_*` under `dg_wm_mu` | bare tag |
| Chunk publication: receipts (admit, result, retire), the publish CAS and its STALE reasons, ticketed and rejected publishes | `publish.c` | `publish.h` | `pub_stale_why` (thread-local), `struct publish_mode` | bare tag |
| Staged publication (W77, K14): stage and pin records, visibility, transaction resolve and retire | `pub_stage.c` | `pub_stage.h` | — | `.pub_stage` |
| Lane activation, content fences, resize, lane bootstrap, lane and chunk writer views | `lanes.c` | `lanes.h` | — | `.lanes` |
| The exact-image mtime capture and execute | `mtime.c` | — | — | `.mtime` |
| Lane sweep, reap selection and batches, reap done, GC ACK | `sweep.c` | `sweep.h` | `struct packed_retirement` | bare tag |
| GETATTR, the content-epoch fence, UTIMENS | `attr.c` | `attr.h` | — | bare tag |
| APPEND reserve, capture and execute, completion, catch-up, open state, the per-session drain, lazy truncate | `append.c` | `append.h` | `struct append_cur` | `.append` |
| Chunk rows, deltas and holds, chunk views, inode authority, lane epoch, bootstrap lane, chunk sweep | `chunk.c` | — | — | bare tag |
| The whole-store consistency check | `check.c` | — | — | bare tag |
| Small-file packing: source, admit, cancel, switch, retire, degraded, candidate index and backfill cursor | `pack.c` | — | — | bare tag |
| Truncate sweep, swept lanes, sweep done, retire | `tsweep.c` | `tsweep.h` | — | bare tag |

"Bare tag" means the area's tests are rows that span files and keep
`meta/meta_apply`: `tests/test_meta_apply.c` (the applied-state machine over
a memory KV, one or more cases per area), `test_session_owned_capture`
(append and mtime capture), `test_namespace_session` (create, unlink, rmdir)
and `test_meta_cmd_namespace.py` (command tags).

## Contract

- Interface: [`efs/meta_apply.h`](../../../include/efs/meta_apply.h) for
  callers; inside the module, one header per area next to its owner with the
  contract at the top. Ownership in one line each: [`rows.h`](rows.h) owns
  the write checks and the row codecs; [`gc.h`](gc.h) owns the only mutable
  process state (the two per-anchor counts, each under a leaf mutex);
  [`publish.h`](publish.h) owns the thread-local STALE reason. Every other
  area owns no state.
- Serialization: every mutation runs inside the participant's serialized
  Raft apply, or as a coordinator-checked transaction step; the KV is the
  only shared state. Every mutation of an inode, lane, chunk, fence, append
  or dentry row goes through `meta_write_*`, so a key a prepared EXCL image
  or a staged publication holds is refused BUSY; `pub_stage.c` writes its own
  stage and pin records directly.
- Failure behaviour: a refusal (`EFS_ERR_STALE`, `EFS_ERR_BUSY`,
  `EFS_ERR_INVAL`, `EFS_ERR_PROTO`) returns before the batch; one batch is one
  atomic `efs_kv_batch`, so a refused or failed mutation changed no inode,
  lane, chunk, fence or dentry row. It is not free of durable side effects:
  a publication with `durable_result` records its STALE or INVAL verdict as
  a receipt before answering, so a retry gets the same answer, and a ticketed
  publish records the ticket's rejection (`publish.c`, `efs_meta_apply_publish`
  and `publish_ticketed`; `test_publication_receipt_stale_retire`).
  `EFS_ERR_NOT_FOUND` is only an absent row or name (I9).
- Ownership smell: the write check in `rows.c` calls `pub_stage_owner`
  (`pub_stage.h`), the bottom layer reaching up, because a staged publication
  owns its chunk key until resolved.
- Apply visibility (staged, committed, visible; the derived counters'
  count-once rule): [apply-visibility.md](../../../docs/how-it-works/apply-visibility.md).
- Invariants: I5–I9, I16–I19, I23, I24
  ([architecture §4](../../../docs/how-it-works/architecture.md#4-invariants)).

## Calls

- May call: `kv/` and `raft/` headers, `meta/` public headers (`txn.h`,
  `session.h`, `put_ticket.h`, `pack_mapping.h`, `dir_layout.h`,
  `dir_spread.h`, `expiry.h`), `common/` helpers, through `apply_internal.h`.
- Called by: `server/` (the Raft host's apply, handlers, PUT upload,
  recovery), `meta/` (`append_plan.c`, `txn.c`, `session.c`, `lock.c`,
  `dir_layout.c`), `kv/kv_snap.c`, `client/write.c`, `sim/` and tests, all
  through `efs/meta_apply.h`. The fault
  daemon (`tests/faults/efsd`) links this directory compiled with
  `EFS_FAULTS=1` ahead of `libefs.a`.

## Tests

- `python3 scripts/tests_for.py meta/meta_apply` lists every row of the
  module; the dotted forms in the table select one file's rows, and a file
  with a bare tag (`meta/meta_apply.publish`) gets the module's rows with a
  note on stderr (add `--run` after `make all`).
- `tests/test_meta_apply.c` compiles and links the real module through
  `efs/meta_apply.h`, as do the other C tests above; only
  `test_readdir_migration_race.py` and `test_retained_create_authority.py`
  splice source text (`csrc.module('meta_apply')`).
- Contract → case map (Task EU): the operations
  [`efs/meta_apply.h`](../../../include/efs/meta_apply.h) declares and this
  directory defines (152; the 22 it declares for `../append_plan.c` and
  `common/publication.c` belong to those owners), grouped by area, and the
  invariants above. A case is listed when it calls the operation (directly
  or through a test helper); where many cases do, the ones named after the
  operation are listed, at most three per row. Build once with
  `make tests/test_meta_apply`; the binary runs every case whose name
  contains an argument (Task EG), so `test_publish` also runs the other
  `test_publish*` cases and `--list` prints them all. "none" means no case
  of `tests/test_meta_apply.c` proves it: the fallback command lists the
  module's other tests. `scripts/check_guide_counts.py` checks that every
  case named here is a `RUN(...)` case of the file and that the command
  selects it.

  | Operation or invariant | Cases | Command |
  |---|---|---|
  | One batch is one atomic `efs_kv_batch`: a failed mutation changes no row | `test_batch_fail` | `./tests/test_meta_apply test_batch_fail` |
  | I9: `EFS_ERR_NOT_FOUND` only for an absent row or name (`efs_meta_apply_resolve`) | `test_i9`, `test_w36_unlink_before_rename_prepare` | `./tests/test_meta_apply test_i9 test_w36_unlink_before_rename_prepare` |
  | I16: replaying a mutation with the same operation ID applies it once and answers as the original did (`efs_meta_apply_create_file_op`, `efs_meta_apply_create_file_log_op`, `efs_meta_apply_mkdir_log_op`, `efs_meta_apply_unlink_op`, `efs_meta_apply_rmdir_op`, `efs_meta_apply_opid_probe`) | `test_i16_durable`, `test_create_negative_replay` | `./tests/test_meta_apply test_i16_durable test_create_negative_replay` |
  | I7: `nlink` equals the committed visible links | `test_link_nlink` | `./tests/test_meta_apply test_link_nlink` |
  | I8: a committed removal is never reverted (a hashed-directory unlink stays gone) | `test_i8_spread` | `./tests/test_meta_apply test_i8_spread` |
  | Write check: a key a prepared image or a txn intent holds is refused BUSY | `test_log_delete_busy_under_intent`, `test_alloc_vs_txn_intent` | `./tests/test_meta_apply test_log_delete_busy_under_intent test_alloc_vs_txn_intent` |
  | I5, I6, I17, I18, I19, I23, I24: no case of `tests/test_meta_apply.c` names them | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |
  | `rows.c`: `efs_meta_get_fence_history`, `efs_meta_pack_fence_history`, `efs_meta_unpack_fence_history` | `test_content_fence_history` | `./tests/test_meta_apply test_content_fence_history` |
  | `rows.c`: `efs_meta_stamp_dir_lane` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |
  | `lookup.c`: `efs_meta_apply_get_inode`, `efs_meta_apply_lookup` | `test_create_lookup_unlink`, `test_mkdir` | `./tests/test_meta_apply test_create_lookup_unlink test_mkdir` |
  | `lookup.c`: `efs_meta_apply_lookup_path` | `test_lookup_path` | `./tests/test_meta_apply test_lookup_path` |
  | `lookup.c`: `efs_meta_apply_get_inode_tx`, `efs_meta_apply_lookup_tx`, `efs_meta_apply_lookup_row_tx`, `efs_meta_apply_resolve_tx` | `test_stat_committed_unresolved_row`, `test_lookup_committed_unresolved_dentry`, `test_readdir_spread` | `./tests/test_meta_apply test_stat_committed_unresolved_row test_lookup_committed_unresolved_dentry test_readdir_spread` |
  | `lookup.c`: `efs_meta_capture_dentry_drop` | `test_w36_split_source_snapshots`, `test_w36_unlink_before_rename_prepare` | `./tests/test_meta_apply test_w36_split_source_snapshots test_w36_unlink_before_rename_prepare` |
  | `lookup.c`: `efs_meta_get_writer_view_tx` | `test_writer_authority_view`, `test_writer_authority_race` | `./tests/test_meta_apply test_writer_authority_view test_writer_authority_race` |
  | `init.c`: `efs_meta_apply_init`, `efs_meta_apply_peek_alloc` (allocation skips live and retired numbers) | `test_alloc_skips_live`, `test_alloc_skips_retired_version` | `./tests/test_meta_apply test_alloc_skips_live test_alloc_skips_retired_version` |
  | `init.c`: `efs_meta_apply_mkfs`, `efs_meta_apply_export_salt`, `efs_meta_apply_salt_record` | `test_export_salt`, `test_export_salt_anchor` | `./tests/test_meta_apply test_export_salt` |
  | `init.c`: `efs_meta_pack_inode`, `efs_meta_pack_dentry` (fixture rows the apply reads back), `efs_meta_unpack_inode` | `test_lookup_path`, `test_dir_rename`, `test_stored_decode_owner` | `./tests/test_meta_apply test_lookup_path test_dir_rename test_stored_decode_owner` |
  | `init.c`: `efs_meta_apply_alloc_prepare`, `efs_meta_apply_membership_record`, `efs_meta_membership_get`, `efs_meta_unpack_dentry` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |
  | `create.c`: `efs_meta_apply_create_file`, `efs_meta_apply_create_file_log` | `test_create_lookup_unlink`, `test_create_remote_parent` | `./tests/test_meta_apply test_create_lookup_unlink test_create_remote_parent` |
  | `create.c`: `efs_meta_apply_create_file_log_at`, `efs_meta_apply_mkdir_at` | `test_create_log_at_matches`, `test_mkdir_log_at_matches`, `test_create_log_at_dup_hint` | `./tests/test_meta_apply test_create_log_at_matches test_mkdir_log_at_matches test_create_log_at_dup_hint` |
  | `create.c`: `efs_meta_apply_create_file_log_lease`, `efs_meta_apply_create_file_log_pending` | `test_create_initial_lease_atomic`, `test_create_negative_replay` | `./tests/test_meta_apply test_create_initial_lease_atomic test_create_negative_replay` |
  | `create.c`: `efs_meta_apply_mkdir` | `test_mkdir`, `test_mkdir_group_affinity` | `./tests/test_meta_apply test_mkdir` |
  | `create.c`: `efs_meta_apply_create_expiring`, `efs_meta_apply_create_refusal` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |
  | `xattr.c`: `efs_meta_apply_xattr`, `efs_meta_xattr_get`, `efs_meta_xattr_list` | `test_xattr` | `./tests/test_meta_apply test_xattr` |
  | `namespace.c`: `efs_meta_apply_unlink` | `test_create_lookup_unlink`, `test_orphan_unlink`, `test_unlink_hashed` | `./tests/test_meta_apply test_create_lookup_unlink test_orphan_unlink test_unlink_hashed` |
  | `namespace.c`: `efs_meta_apply_link` | `test_link_nlink`, `test_link_hashed`, `test_link_splitting` | `./tests/test_meta_apply test_link_` |
  | `namespace.c`: `efs_meta_apply_rename` | `test_dir_rename`, `test_rename_cross_dir`, `test_rename_hashed` | `./tests/test_meta_apply test_dir_rename test_rename_` |
  | `namespace.c`: `efs_meta_apply_setattr` | `test_setattr_mode_owner`, `test_content_fence_reductions_commute` | `./tests/test_meta_apply test_setattr_mode_owner test_content_fence_reductions_commute` |
  | `namespace.c`: `efs_meta_apply_expire`, `efs_meta_apply_expiry_policy`, `efs_meta_apply_expiry_renew` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |
  | `readdir.c`: `efs_meta_apply_readdir`, `efs_meta_apply_readdir_bounded` | `test_readdir_local`, `test_readdir_tombstone_budget`, `test_readdir_concurrent_split` | `./tests/test_meta_apply test_readdir_local test_readdir_tombstone_budget test_readdir_concurrent_split` |
  | `readdir.c`: `efs_meta_apply_readdir_tx_bounded` | `test_readdir_intent_frontier`, `test_readdir_migration_participant_cuts` | `./tests/test_meta_apply test_readdir_intent_frontier test_readdir_migration_participant_cuts` |
  | `readdir.c`: `efs_meta_apply_rmdir` | `test_rmdir_hashed`, `test_rmdir_rename` | `./tests/test_meta_apply test_rmdir_` |
  | `reclaim.c`: `efs_meta_apply_reclaim`, `efs_meta_apply_orphan_reclaim` | `test_gc_last_close_before_resolve` | `./tests/test_meta_apply test_gc_last_close_before_resolve` |
  | `gc.c`: the pending-GC watermark (`efs_meta_gc_pending_apply_start`, `efs_meta_gc_pending_begin`, `efs_meta_gc_pending_derived`, `efs_meta_gc_pending_invalidate`, `efs_meta_gc_pending_note`, `efs_meta_gc_pending_peek`, `efs_meta_gc_pending_zero_if`, seven operations) | `test_gc_watermark`, `test_gc_pending_follower_derive` | `./tests/test_meta_apply test_gc_watermark test_gc_pending_follower_derive` |
  | `gc.c`: degraded marks and their count (`efs_meta_apply_degraded_mark`, `efs_meta_apply_degraded_clear`, `efs_meta_degraded_count_begin`, `efs_meta_degraded_count_derived`, `efs_meta_degraded_count_invalidate`, `efs_meta_degraded_count_peek`, `efs_meta_degraded_live`, `efs_meta_pack_degraded`, `efs_meta_unpack_degraded`) | `test_degraded_marks`, `test_degraded_mark_after_publish` | `./tests/test_meta_apply test_degraded_mark` |
  | `gc.c`: recovery marks and the chunk reference scan (`efs_meta_apply_recovery_mark`, `efs_meta_chunk_refs_scan`) | `test_recovery_marks` | `./tests/test_meta_apply test_recovery_marks` |
  | `gc.c`: GC and reap records (`efs_meta_unpack_gc`, `efs_meta_pack_reap`, `efs_meta_unpack_reap`) | `test_gc_reap`, `test_reap_bounded_exact_keys` | `./tests/test_meta_apply test_gc_reap test_reap_bounded_exact_keys` |
  | `gc.c`: `efs_meta_degraded_count_zero_if`, `efs_meta_pack_gc` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |
  | `publish.c`: `efs_meta_apply_publish` (CAS, I20, I21, STALE then retry) | `test_publish`, `test_cas_i20_i21`, `test_publish_stale_then_retry` | `./tests/test_meta_apply test_publish test_cas_i20_i21` |
  | `publish.c`: receipts and the STALE reason (`efs_meta_apply_publication_retire`, `efs_meta_publication_result`, `efs_meta_apply_publish_stale_why`) | `test_publication_receipt_stale_retire`, `test_chunk_deltas` | `./tests/test_meta_apply test_publication_receipt_stale_retire test_chunk_deltas` |
  | `publish.c`: `efs_meta_apply_publication_admit`, `efs_meta_apply_publish_reject`, `efs_meta_publication_receipt` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |
  | `pub_stage.c`: `efs_meta_pub_stage_encode`, `efs_meta_pub_stage_decode`, `efs_meta_pub_stage_peek`, `efs_meta_pub_pinned` | `test_pub_stage_record` | `./tests/test_meta_apply test_pub_stage_record` |
  | `pub_stage.c`: `efs_meta_apply_publication_stage`, `efs_meta_apply_txn_resolve`, `efs_meta_apply_txn_retire`, `efs_meta_pub_pin_encode`, `efs_meta_pub_stage_visible` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply.pub_stage` |
  | `lanes.c`: `efs_meta_apply_activate_lane`, `efs_meta_apply_activate_lanes`, `efs_meta_apply_activate_lanes_at` | `test_cross_group_lane`, `test_lane_epoch_never_lags`, `test_writer_lane_authority` | `./tests/test_meta_apply test_cross_group_lane test_lane_epoch_never_lags test_writer_lane_authority` |
  | `lanes.c`: content fences (`efs_meta_apply_content_fence`, `efs_meta_prepare_content_fence`, `efs_meta_encode_fence_prepare`, `efs_meta_get_writer_chunk_view_tx`) | `test_content_fence_prepared_writers`, `test_content_fence_sweep_cas`, `test_writer_lane_authority` | `./tests/test_meta_apply test_content_fence_prepared_writers test_content_fence_sweep_cas test_writer_lane_authority` |
  | `lanes.c`: lane bootstrap and writer views (`efs_meta_capture_lane_bootstrap`, `efs_meta_encode_lane_bootstrap`, `efs_meta_get_lane_writer_view`) | `test_lane_local_writer_views` | `./tests/test_meta_apply test_lane_local_writer_views` |
  | `lanes.c`: `efs_meta_apply_fence_prepare`, `efs_meta_apply_lane_bootstrap` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply.lanes` |
  | `mtime.c`: `efs_meta_capture_mtime`, `efs_meta_execute_mtime` | `test_mtime_capture_execute` | `./tests/test_meta_apply test_mtime_capture_execute` |
  | `sweep.c`: `efs_meta_apply_lane_sweep`, `efs_meta_apply_gc_ack` | `test_gc_reap`, `test_gc_tail_alias`, `test_gc_watermark` | `./tests/test_meta_apply test_gc_reap test_gc_tail_alias test_gc_watermark` |
  | `sweep.c`: `efs_meta_apply_lane_sweep_batch`, `efs_meta_lane_sweep_select`, `efs_meta_apply_lane_sweep_reaped` | `test_gc_delta_batch_boundary`, `test_reaped_sweep_no_gc` | `./tests/test_meta_apply test_gc_delta_batch_boundary test_reaped_sweep_no_gc` |
  | `sweep.c`: `efs_meta_apply_reap_batch`, `efs_meta_reap_select`, `efs_meta_apply_reap_done` | `test_reap_bounded_exact_keys`, `test_reap_cleanup_deferred` | `./tests/test_meta_apply test_reap_bounded_exact_keys test_reap_cleanup_deferred` |
  | `attr.c`: `efs_meta_apply_getattr`, `efs_meta_apply_epoch_fence` (I22) | `test_stat_collect`, `test_stat_fence_and_gen`, `test_epoch_i22` | `./tests/test_meta_apply test_stat_collect test_stat_fence_and_gen test_epoch_i22` |
  | `attr.c`: `efs_meta_apply_utimens` | `test_utimens_fence` | `./tests/test_meta_apply test_utimens_fence` |
  | `append.c`: `efs_meta_apply_append_reserve`, `efs_meta_apply_append_resolve`, `efs_meta_apply_append_state`, `efs_meta_apply_append_drop_session` | `test_append_reserve`, `test_truncate_append_cursor` | `./tests/test_meta_apply test_append_reserve test_truncate_append_cursor` |
  | `append.c`: `efs_meta_apply_truncate_lazy` | `test_truncate_lazy`, `test_truncate_range_del`, `test_sparse_grow_tail_epoch` | `./tests/test_meta_apply test_truncate_lazy test_truncate_range_del test_sparse_grow_tail_epoch` |
  | `append.c`: `efs_meta_append_result`, `efs_meta_append_session_next`, `efs_meta_apply_append_catchup`, `efs_meta_apply_append_drain_file`, `efs_meta_apply_append_open`, `efs_meta_apply_append_reserve_durable`, `efs_meta_capture_append`, `efs_meta_collect_append_barrier_retirement`, `efs_meta_collect_append_completion`, `efs_meta_execute_append` | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply.append` |
  | `chunk.c`: chunk rows, deltas and holds (`efs_meta_apply_get_chunk`, `efs_meta_apply_get_chunk_deltas`, `efs_meta_apply_chunk_holds`, `efs_meta_apply_get_chunk_row`) | `test_chunk_deltas`, `test_stored_decode_owner`, `test_evidence` | `./tests/test_meta_apply test_chunk_deltas test_stored_decode_owner test_evidence` |
  | `chunk.c`: chunk views and inode authority (`efs_meta_get_chunk_view`, `efs_meta_get_chunk_view_auth`, `efs_meta_get_chunk_view_tx`, `efs_meta_get_inode_authority_tx`, `efs_meta_get_lane_epoch`) | `test_content_fence_view_race`, `test_lane_adoption`, `test_content_fence_prepared_writers` | `./tests/test_meta_apply test_content_fence_view_race test_lane_adoption test_content_fence_prepared_writers` |
  | `chunk.c`: `efs_meta_apply_bootstrap_lane`, `efs_meta_apply_sweep_chunk` | `test_lane_adoption`, `test_content_fence_sweep_cas` | `./tests/test_meta_apply test_lane_adoption test_content_fence_sweep_cas` |
  | `tsweep.c`: `efs_meta_apply_trunc_sweep`, `efs_meta_get_lane_swept`, `efs_meta_apply_sweep_done`, `efs_meta_apply_retire` | `test_trunc_sweep`, `test_truncate_range_del` | `./tests/test_meta_apply test_trunc_sweep test_truncate_range_del` |
  | `check.c`: `efs_meta_apply_check` (38 cases call it as their end-of-case oracle; none hands it an inconsistent store) | none; whole-module fallback | `python3 scripts/tests_for.py meta/meta_apply` |

- Split evidence:
  [meta-apply-split-20261105](../../../docs/status/2026-11/meta-apply-split-20261105.md).

## Open findings

- None owned here; see [`../README.md`](../README.md) for the `meta/` rows.
