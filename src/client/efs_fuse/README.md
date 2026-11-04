# src/client/efs_fuse/ — the FUSE adapter (split from `efs_fuse.c`, wave 55)

Read `fuse_internal.h` first (shared includes, structs and macros, 465 lines),
then only the file your symptom names. The concatenation order used by
source-splicing tests is `MODULES["efs_fuse"]` in
[`tests/csrc.py`](../../../tests/csrc.py); it is a transition mechanism, not a
contract.

## Files

Symptom → file → tests (`python3 scripts/tests_for.py client/efs_fuse.<file>`):

| Symptom or change | File | Holds | Tests |
|---|---|---|---|
| Crash handler, stats text, feature flags, virtual inodes, `.efs` query results, invalidation thread, `find`, DAC and permission helpers | `support.c` | `g_mountpoint`, `t_req`, `g_fuse_se`, `g_find_res`/`g_find_res_mu` | `.support` (6) |
| LOOKUP memo, GETATTR row cache, READDIR collection | `getattr.c` | `t_lookup_memo_serial`, `t_getattr_row`, `t_getattr_ino` | `.getattr` (3) |
| OPEN / CREATE references, lease acquire, retry fairness, BUSY, READ | `open.c` | `g_open_mu` and the open-ref table | `.open` (7) |
| Write-back pool, bounce buffers, drain budgets | `writeback.c` | `g_wb` | `.writeback` (3) |
| Append reservation, publication, pressure flush and background worker | `append.c` | — | `.append` (5) |
| WRITE admission, store, buffer, FSYNC, FLUSH | `write.c` | — | `.write` (1) |
| CREATE, MKDIR, UNLINK, STATFS, chmod/chown, SYMLINK, LINK, utimens, TRUNCATE | `namespace.c` | — | `.namespace` (5) |
| RELEASE, control drain, RENAME, LSEEK | `release.c` | — | `.release` (1) |
| POSIX locks, flock, `FUSE_INIT`, readiness pipe | `plock_init.c` | `g_fuse_ready_wr` | `.plock_init` (1) |
| LOOKUP, SETATTR, deep-callback sampling | `lookup.c` | — | `.lookup` (1) |
| The `ll_*_run` operation bodies (xattr, opendir, read/readdir dispatch) | `ll_run.c` | — | `.ll_run` (2) |
| `efs_fuse_destroy`, the mutation-gated `ll_*` wrappers, the operation table, option parsing, startup, `main` | `main.c` | stop-control state (`static` per `stop_control.h`) | `.main` (9) |

Six catalogue rows span files and keep the bare `client/efs_fuse` tag
(`opendir_authority_errno`, `fallocate_errno`, `inode_lock_stripes`,
`create_admission_wait`, `command_policy_mount`, the stop-control standalone
compile).

## Contract

- Interface: `fuse_internal.h` is the only header; every file includes it and
  nothing else of the module. The eleven `extern` globals it declares, by
  owner, protection and lifetime:
  - `g_mountpoint`, `g_fuse_se` (`support.c`), `g_fuse_ready_wr`
    (`plock_init.c`): set once during startup in `main.c`/`FUSE_INIT`, read-only
    afterwards; no lock.
  - `t_req`, `t_lookup_memo_serial`, `t_getattr_row`, `t_getattr_ino`:
    thread-local, valid only inside the handler that set them; a helper must
    not keep them across an RPC that may block.
  - `g_find_res` under `g_find_res_mu` (`support.c`): bounded virtual query
    results, recycled on FORGET.
  - `g_open_mu` (`open.c`): protects the open-ref table, `retry_order` and
    `retry_turn`; the lease edge itself is stripe-owned. Never held across an
    RPC (client rule, [`client_internal.h`](../client_internal.h)).
  - `g_wb` (`writeback.c`): its own `mu` and condition variables; `err` and
    `err_ino` retain the first failure until drained; a job is acknowledged
    only after its PUT has fragment quorum, never after a bounce copy.
- Invariants: the FUSE kernel contract
  [§7.7](../../../docs/how-it-works/architecture.md#77-fuse--kernel-interface-contract)
  and the client side of I9, I12–I15, I20–I24
  ([architecture §4](../../../docs/how-it-works/architecture.md#4-invariants)).
  `ENOENT` is only ever an answer for a genuinely absent name (I9): every
  authority or availability failure keeps its own errno.
- Failure behaviour: a handler replies with a positive errno and never
  substitutes a default; a timeout or BUSY is retried on the call's inherited
  budget, then surfaces as that error.

## Calls

- May call: `client/` (`client_internal.h`, `ops.c`, `read.c`, `write.c`,
  `inode_rpc.c`, `stop_control.h`), `commands/source.h` (the `.efs` command
  surface), public `efs/*` headers, libfuse.
- Called by: libfuse through the operation table in `main.c`; tests through
  `tests/csrc.py`. Nothing in `src/` includes `fuse_internal.h`.

## Tests

- `python3 scripts/tests_for.py client/efs_fuse` lists all 50 rows; the dotted
  forms above select one file's rows (add `--run` after `make all`).
- Gate evidence for the split:
  [efs-fuse-split-20261104](../../../docs/status/2026-11/efs-fuse-split-20261104.md).

## Open findings

- None owned here. [K35](../../../docs/status/alpha-1.0-2plus1-checklist.md)
  (fsync `EIO` with a member down) lives in `../data_commit.c`; the
  intermittent TSan race seen during the split is in `../write.c`
  (`efs_dcache_maybe_reclaim`), not in this module.
