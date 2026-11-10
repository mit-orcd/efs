# src/client/efs_fuse/ — the FUSE adapter (split from `efs_fuse.c`, wave 55)

Read the header of the area your symptom names first: each `<area>.h` opens
with that area's contract (what it owns, which lock protects what, the
lifetime of each object, what callers get back on BUSY, timeout and I/O
failure), then its declarations. `fuse_internal.h` (80 lines) holds only what
every file shares: the includes, `struct efs_file_handle`, the per-request
thread-local `t_req` and six helpers every handler calls. A `.c` file includes
only the headers of the areas it calls, so the includes at its top are its
dependency list. The concatenation order used by source-splicing tests is
`MODULES["efs_fuse"]` in [`tests/csrc.py`](../../../tests/csrc.py) (the thirteen
`.c` files, then `fuse_internal.h`, then the headers in the order of the table
below); it is a transition mechanism, not a contract.

## Files

Symptom → file → tests (`python3 scripts/tests_for.py client/efs_fuse.<file>`):

| Symptom or change | File | Header | Holds | Tests |
|---|---|---|---|---|
| Crash handler, stats text, feature flags, virtual inodes, invalidation thread, the `.find` directory, query credentials, DAC and permission helpers | `support.c` | `support.h` | `t_req`, `g_fuse_se` | `.support` (5) |
| `.find/<term>` query results: fetch, per-caller match, LOOKUP intern, OPEN copy, GETATTR size, slots, the 64 MiB budget, the fetch limit, unmount clear | `find_res.c` | `find_res.h`: `find_res_intern`, `find_res_open`, `find_query_fill_stat`, `find_query_read`, `find_res_clear` | the result table (`static`, private with its mutex and byte count; slots are never handed out) | `.find_res` (2; `tests/test_find_res_interface.c` links the real `find_res.c`) |
| LOOKUP memo, GETATTR row cache, READDIR collection | `getattr.c` | `getattr.h` | `t_lookup_memo_serial`, `t_getattr_row`, `t_getattr_ino`, `struct efs_dirh` | `.getattr` (3) |
| OPEN / CREATE references, lease acquire, retry fairness, BUSY, READ; retained last closes and their background drain | `open.c` | `open.h`: `efs_open_acquire_*`, `efs_close_note`, `efs_open_release_pending`, `efs_open_retry_pass`, `efs_client_ino_is_open`, `efs_open_retained`, `efs_open_append_*` | the open-ref table (`static`, private with its mutex; rows are never handed out), `struct efs_pending_create` | `.open` (8; `tests/test_open_interface.c` links the real `open.c`) |
| Write-back pool, bounce buffers, drain budgets, the sticky PUT error, worker stop | `writeback.c` | `writeback.h`: `efs_wb_enqueue_owned`, `efs_wb_pending`, `efs_wb_sync`, `efs_wb_sync_ino`, `efs_wb_stop` | the pool (`static g_wb`, private with its mutex) | `.writeback` (4; `tests/test_wb_interface.c` links the real `writeback.c`) |
| Append reservation, publication, pressure flush and background worker | `append.c` | `append.h` | — | `.append` (5) |
| WRITE admission, store, buffer, FSYNC, FLUSH | `write.c` | `write.h` | — | `.write` (2) |
| CREATE, MKDIR, UNLINK, STATFS, chmod/chown, SYMLINK, LINK, utimens, TRUNCATE | `namespace.c` | `namespace.h` | — | `.namespace` (5) |
| RELEASE (last-close lease edge, `CLOSE_ADMIT_SPINS`), control drain, RENAME, LSEEK | `release.c` | `release.h` | — | `.release` (5); the retained-release table and its drain are `open.c`, `.open` |
| POSIX locks, flock, `FUSE_INIT`, readiness pipe | `plock_init.c` | `plock_init.h` | `g_fuse_ready_wr` | none routed; the flock release test is under `.release` |
| LOOKUP, SETATTR, deep-callback sampling | `lookup.c` | `lookup.h` | — | `.lookup` (1) |
| The `ll_*_run` operation bodies (xattr, opendir, read/readdir dispatch) | `ll_run.c` | `ll_run.h` | — | `.ll_run` (2) |
| `efs_fuse_destroy`, the mutation-gated `ll_*` wrappers, the operation table, option parsing, startup, `main` | `main.c` | — | `g_mountpoint` and the stop-control state (`static`) | `.main` (10) |

Six catalogue rows span files and keep the bare `client/efs_fuse` tag
(`opendir_authority_errno`, `fallocate_errno`, `inode_lock_stripes`,
`create_admission_wait`, `command_policy_mount`, the stop-control standalone
compile).

## Contract

- Interface: one header per area, next to its owner, with the contract at
  the top. The globals shared between files are declared in their
  owner's header with their lock and lifetime:
  [`support.h`](support.h) (`g_fuse_se`), [`find_res.h`](find_res.h) (no
  global: the `.find` result table, its mutex and byte count are private
  to `find_res.c`; every operation fetches the caller's result first and
  the header states what each answers on a miss), [`getattr.h`](getattr.h) (the three handler
  thread-locals), [`open.h`](open.h) (no global: the open-ref table and
  its mutex are private to `open.c`; the header states the operations,
  that a retained last close counts as open, and how `append.c` reaches a
  row's burst state; the inode edge stripe `open_edge_mu` is public and is
  taken first),
  [`writeback.h`](writeback.h) (no global: the pool and its mutex are
  private to `writeback.c`; the header states the five operations, what
  each returns on BUSY, timeout and I/O failure, the sticky error and the
  second, per-inode error channel in `../write.c`, reached through
  `efs_file_sync_error`),
  [`plock_init.h`](plock_init.h) (`g_fuse_ready_wr`);
  [`fuse_internal.h`](fuse_internal.h) declares `t_req`. `g_mountpoint` is
  private to `main.c`. Nothing in this directory is declared twice.
- Invariants: the FUSE kernel contract
  [§7.7](../../../docs/how-it-works/architecture.md#77-fuse--kernel-interface-contract)
  and the client side of I9, I12–I15, I20–I24
  ([architecture §4](../../../docs/how-it-works/architecture.md#4-invariants)).
  `ENOENT` is only ever an answer for a genuinely absent name (I9): every
  authority or availability failure keeps its own errno.
- Failure behaviour: a handler replies with a positive errno and never
  substitutes a default; a timeout or BUSY is retried on the call's inherited
  budget, then surfaces as that error. The per-area headers say which errno
  each area returns for BUSY, timeout and I/O failure.
- Unmount (K90): `efs_fuse_destroy` retries what is transient within its 60 s
  stop bound and decides what is not. Each attempt PUTs every dirty chunk, new
  partial chunks included, then REPORTs. A fresh `ENOSPC` answer ends the drain:
  the chunks are named by `force-discard ... cause=unmount-enospc`, the stop is
  unclean (exit 1, stop record). The application already got `ENOSPC` at its
  FSYNC or FLUSH.

## Calls

- May call: `client/` (`client_internal.h`, `ops.c`, `read.c`, `write.c`,
  `inode_rpc.c`, `stop_control.h`), `commands/source.h` (the `.efs` command
  surface), public `efs/*` headers, libfuse.
- Called by: libfuse through the operation table in `main.c`; tests through
  `tests/csrc.py`. Nothing in `src/` includes a header of this directory.

## Tests

- `python3 scripts/tests_for.py client/efs_fuse` lists all 55 rows; the dotted
  forms above select one file's rows (add `--run` after `make all`).
- Gate evidence for the split:
  [efs-fuse-split-20261104](../../../docs/status/2026-11/efs-fuse-split-20261104.md);
  for the per-area headers:
  [efs-fuse-interfaces-20261104](../../../docs/status/2026-11/efs-fuse-interfaces-20261104.md).

## Open findings

- None owned here. [K35](../../../docs/status/alpha-1.0-2plus1-checklist.md)
  (fsync `EIO` with a member down) lives in `../data_commit.c`; the
  intermittent TSan race seen during the split is in `../write.c`
  (`efs_dcache_maybe_reclaim`), not in this module.
