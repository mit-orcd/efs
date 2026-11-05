# src/client/efs_fuse/ — the FUSE adapter (split from `efs_fuse.c`, wave 55)

Read the header of the area your symptom names first: each `<area>.h` opens
with that area's contract (what it owns, which lock protects what, the
lifetime of each object, what callers get back on BUSY, timeout and I/O
failure), then its declarations. `fuse_internal.h` (80 lines) holds only what
every file shares: the includes, `struct efs_file_handle`, the per-request
thread-local `t_req` and six helpers every handler calls. A `.c` file includes
only the headers of the areas it calls, so the includes at its top are its
dependency list. The concatenation order used by source-splicing tests is
`MODULES["efs_fuse"]` in [`tests/csrc.py`](../../../tests/csrc.py) (the twelve
`.c` files, then `fuse_internal.h`, then the headers in the order of the table
below); it is a transition mechanism, not a contract.

## Files

Symptom → file → tests (`python3 scripts/tests_for.py client/efs_fuse.<file>`):

| Symptom or change | File | Header | Holds | Tests |
|---|---|---|---|---|
| Crash handler, stats text, feature flags, virtual inodes, `.efs` query results, invalidation thread, `find`, DAC and permission helpers | `support.c` | `support.h` | `t_req`, `g_fuse_se`, `g_find_res`/`g_find_res_mu` | `.support` (6) |
| LOOKUP memo, GETATTR row cache, READDIR collection | `getattr.c` | `getattr.h` | `t_lookup_memo_serial`, `t_getattr_row`, `t_getattr_ino`, `struct efs_dirh` | `.getattr` (3) |
| OPEN / CREATE references, lease acquire, retry fairness, BUSY, READ; retained last closes and their background drain | `open.c` | `open.h` | `g_open_mu`, the open-ref table (`struct efs_open_ref`), `struct efs_pending_create` | `.open` (6) |
| Write-back pool, bounce buffers, drain budgets, the sticky PUT error | `writeback.c` | `writeback.h` | `g_wb` (`struct efs_wb_state`, `struct efs_wb_job`, `EFS_WB_*`) | `.writeback` (3) |
| Append reservation, publication, pressure flush and background worker | `append.c` | `append.h` | — | `.append` (5) |
| WRITE admission, store, buffer, FSYNC, FLUSH | `write.c` | `write.h` | — | `.write` (2) |
| CREATE, MKDIR, UNLINK, STATFS, chmod/chown, SYMLINK, LINK, utimens, TRUNCATE | `namespace.c` | `namespace.h` | — | `.namespace` (5) |
| RELEASE (last-close lease edge, `CLOSE_ADMIT_SPINS`), control drain, RENAME, LSEEK | `release.c` | `release.h` | — | `.release` (3); the retained-release table and its drain are `open.c`, `.open` |
| POSIX locks, flock, `FUSE_INIT`, readiness pipe | `plock_init.c` | `plock_init.h` | `g_fuse_ready_wr` | none routed; the flock release test is under `.release` |
| LOOKUP, SETATTR, deep-callback sampling | `lookup.c` | `lookup.h` | — | `.lookup` (1) |
| The `ll_*_run` operation bodies (xattr, opendir, read/readdir dispatch) | `ll_run.c` | `ll_run.h` | — | `.ll_run` (2) |
| `efs_fuse_destroy`, the mutation-gated `ll_*` wrappers, the operation table, option parsing, startup, `main` | `main.c` | — | `g_mountpoint` and the stop-control state (`static`) | `.main` (9) |

Six catalogue rows span files and keep the bare `client/efs_fuse` tag
(`opendir_authority_errno`, `fallocate_errno`, `inode_lock_stripes`,
`create_admission_wait`, `command_policy_mount`, the stop-control standalone
compile).

## Contract

- Interface: one header per area, next to its owner, with the contract at
  the top. The ten globals shared between files are declared in their
  owner's header with their lock and lifetime:
  [`support.h`](support.h) (`g_fuse_se`, `g_find_res` under
  `g_find_res_mu`), [`getattr.h`](getattr.h) (the three handler
  thread-locals), [`open.h`](open.h) (`g_open_mu` and the stripe order),
  [`writeback.h`](writeback.h) (`g_wb` under `g_wb.mu`, the sticky `err` /
  `err_ino` and the second, per-inode error channel in `../write.c`, reached
  through `efs_file_sync_error`),
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

## Calls

- May call: `client/` (`client_internal.h`, `ops.c`, `read.c`, `write.c`,
  `inode_rpc.c`, `stop_control.h`), `commands/source.h` (the `.efs` command
  surface), public `efs/*` headers, libfuse.
- Called by: libfuse through the operation table in `main.c`; tests through
  `tests/csrc.py`. Nothing in `src/` includes a header of this directory.

## Tests

- `python3 scripts/tests_for.py client/efs_fuse` lists all 51 rows; the dotted
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
