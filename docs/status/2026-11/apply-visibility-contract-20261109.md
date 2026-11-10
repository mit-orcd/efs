# Apply visibility contract between owners (Task ET)

**Verdict: PASS, docs and comments only, no code or behavior change, no K
row, blocks alpha: no** (Nov 9, branch `claude/eager-sammet-10c74c` on devel
`9bb50fe79`; merged into devel in wave 68, Nov 10).

- Purpose: an outside reviewer asked for one short authoritative
  description of when a metadata batch is staged, committed and visible,
  beside the interfaces and linked from both owners. The Task EI blind
  reader had to rebuild that order from several files
  ([blind reader page](blind-reader-20261109.md), question 2), and the
  fixes of Task DY ([k37 page](k37-degraded-count-overshoot-20261108.md)),
  Task EI (`6e7f838aa`) and Task EK
  ([degraded-count begin race](degraded-count-begin-race-20261109.md))
  all turn on it.
- Source: devel `9bb50fe79` (wave 67). Build identity: none; no binary
  changed (comments only).
- Environment: Mac (Darwin 25.6.0), no cluster, no host.

## What changed

- New page [`docs/how-it-works/apply-visibility.md`](../../how-it-works/apply-visibility.md)
  (146 lines): sections "The three states of a metadata row", "The sequence
  of one apply", "How a derived counter is derived", "The rule: each row
  counted once across a leader change", the two worked examples and "For a
  new derived counter".
- Two- to four-line comments naming staged, committed, visible and the page:
  top of `include/efs/meta_apply.h`; `host_apply` in
  `src/server/raft_host.c`, at `efs_kv_lsm_apply_begin`.
- One Contract bullet each in `src/meta/meta_apply/README.md` and
  `src/server/README.md`.

## Where each definition comes from

| Claim | File and function |
|---|---|
| staged: in the thread-local scope, read only by the apply thread | `src/kv/kv_lsm.c`: `apply_scope` (`__thread`), `lsm_batch` (staging branch), `lookup`, `merge_scan`, `apply_staging` |
| committed: one atomic WAL record with the cursor, then writers unlocked | `kv_lsm.c` `efs_kv_lsm_apply_end`; `include/efs/kv_lsm.h` comment |
| other threads' scans see the old committed table | `kv_lsm.c` `lsm_scan_from`; `kv_lsm_internal.h` `lsm_rlock` (read hold on `l->view_rw`, not `l->writers`) |
| committed is readable, fsync at the last sync release | `include/efs/kv_lsm.h` comments on `efs_kv_lsm_sync_hold` and `efs_kv_lsm_apply_end` |
| every efsd note is inside `host_apply`'s scope | callers grepped across `src/`: `gc_queue` (`publish_inner_as`, `sweep_cb`, `efs_meta_apply_trunc_sweep`, `efs_meta_apply_pack_switch`, `efs_meta_apply_sweep_chunk`), `gc_ack_apply`, the degraded mark, recovery mark and clear, `efs_meta_apply_publication_stage` and `efs_meta_apply_txn_resolve` (via `efs_txn_apply_prepare`, `apply_txn_cmd`) all reach efsd only from `apply_*_cmd` functions called by `host_apply_dispatch`; `efs_meta_apply_lane_sweep` only from `src/sim/` |
| applies serialized by the writers lock | `efs_kv_lsm_apply_begin` (`pthread_rwlock_wrlock(&l->writers)`) |
| apply order: begin, apply-start hook, dispatch, end | `raft_host.c` `host_apply`; callback set in `attach_replica`; pump `host_pump` |
| notes before commit | `gc.c` `gc_queue`, `efs_meta_apply_degraded_mark`, `efs_meta_apply_recovery_mark`, `efs_meta_apply_degraded_clear`; `sweep.c` `gc_ack_apply`; `publish.c` `publish_inner_as` |
| unknown: positive deltas kept in `*_pos`/`*_cur`, negatives dropped | `gc.c` `efs_meta_gc_pending_note`, `degraded_note` |
| derive on the leader's GC thread only | `raft_host.c` `host_gc_thread` (role check), `host_gc_frag_pass`, `host_rebuild_pass` |
| begin keeps the latest apply's notes | `gc.c` `efs_meta_gc_pending_apply_start`, `degraded_apply_start`, `efs_meta_gc_pending_begin`, `efs_meta_degraded_count_begin` |
| invalidation points | `raft_host.c` `snap_import_cancel` and the import completion paths |

## Checks (Mac, this branch)

| Command | Result |
|---|---|
| `make docs-check` | rc 0 |
| `python3 scripts/check_doc_links.py` | 0 dangling, 0 orphan |
| `python3 scripts/check_guide_counts.py` | 13 guides match |
| `sh scripts/rebuild-code-graph.sh`, `python3 tests/test_code_graph.py` | rc 0; PASS, 10 tests OK |
| `graphify explain meta/meta_apply`, `graphify explain server/raft_host --all` | the page is **not** listed by either (see Not done) |
| `graphify explain` on `src_server_raft_host_host_apply`, `src_meta_meta_apply_gc_efs_meta_degraded_count_begin`; `graphify query "apply visibility staged committed visible"` | the page's sections appear as `mentions_symbol` on the symbols; the query finds the page |
| `make all` | fails on Mac before this change's files: `src/common/log_ts.c` (`fopencookie`) and `raft_host.c` (`sys/eventfd.h`) are Linux-only |
| `make src/meta/meta_apply/gc.o` (includes the edited header) | rc 0 |
| `python3 tests/csrc.py` | `csrc self-check ok` |

Reproduce: check out the branch and run the commands above.

## Not done

- `graphify explain meta/meta_apply` and `graphify explain server/raft_host` do not list the page. `components()` in `scripts/build-code-graph.py` sets `guide` only to the directory's `README.md` and reads no hook from `tools/graphify/scope.json`; a Markdown link in the README makes no edge (only the Makefile emits `references_path`). Listing it needs a builder change, not made in this docs-only task: in the component loop, beside the `guided_by` edge, add one edge per `docs/how-it-works/*.md` that the guide links to.
- No Linux build of `raft_host.c`; the change there is a comment.
- Not examined: whether an efsd apply can note a lower and then abort
  (`efs_kv_lsm_apply_end` with NULL), which would lower a known count
  below the live rows; the page names it as not covered.
- Seen, not changed: `publish_inner_as` has no dry check before its
  `gc_queue` note, so a dry PREPARE can note a GC insert it never writes
  (an upward drift).
- `src/meta/meta_apply/gc.h` still says a count moves "after its batch
  committed" (noted on the EK page); not changed here.
