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
  atomic `efs_kv_batch`, so a refused or failed mutation wrote nothing.
  `EFS_ERR_NOT_FOUND` is only an absent row or name (I9).
- Ownership smell: the write check in `rows.c` calls `pub_stage_owner`
  (`pub_stage.h`), the bottom layer reaching up, because a staged publication
  owns its chunk key until resolved.
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
  module; the dotted forms in the table select one file's rows (add `--run`
  after `make all`).
- `tests/test_meta_apply.c` compiles and links the real module through
  `efs/meta_apply.h`, as do the other C tests above; only
  `test_readdir_migration_race.py` and `test_retained_create_authority.py`
  splice source text (`csrc.module('meta_apply')`).
- Split evidence:
  [meta-apply-split-20261105](../../../docs/status/2026-11/meta-apply-split-20261105.md).

## Open findings

- None owned here; see [`../README.md`](../README.md) for the `meta/` rows.
