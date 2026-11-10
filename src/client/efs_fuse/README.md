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
| `efs_fuse_destroy`, the mutation-gated `ll_*` wrappers, the operation table, option parsing, startup, `main` | `main.c` | — | `g_mountpoint` and the stop-control state (`static`) | `.main` (11) |

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

- `python3 scripts/tests_for.py client/efs_fuse` lists all 56 rows; the dotted
  forms above select one file's rows (add `--run` after `make all`).
- Contract → case maps (Task EU): one row per operation or invariant the
  owner's header states, the case that proves it and the command that runs
  it. The three interface binaries take the case filter of `test_session` (`--list`, a name substring, exit 2 for no match); a command with a case name runs that case alone. "none" means no case in the interface binary
  proves it: the whole-module fallback command lists the file's catalogue
  rows. `scripts/check_guide_counts.py` checks that every case named here is
  a function of the test file the command names.

  `open.h` ([`tests/test_open_interface.c`](../../../tests/test_open_interface.c)):

  | Operation or invariant | Cases | Command |
  |---|---|---|
  | No row: `efs_close_note` 0, the `efs_open_append_*` three answer nothing, `efs_open_release_pending` `EFS_OK`, `efs_open_close_unadmitted` 0, no row created | `test_no_row` | `make tests/test_open_interface && ./tests/test_open_interface test_no_row` |
  | `efs_open_acquire_fileid`: the first open takes one HOLD and fetches the FileID when gen is 0; shared opens add no HOLD | `open_lease_shared_and_failed_hold`, `test_two_opens` | `make tests/test_open_interface && ./tests/test_open_interface open_lease_shared_and_failed_hold test_two_opens` |
  | `efs_open_acquire_fileid` `EFS_ERR_BUSY`: admission full (retried outside the stripe), an unknown OPEN past the caller's deadline (retained), a contender on a held stripe | `open_busy_admission`, `open_busy_verdicts`, `open_busy_deadline` | `make tests/test_open_interface && ./tests/test_open_interface open_busy_admission open_busy_verdicts open_busy_deadline` |
  | `efs_open_acquire_fileid` other errors are the lease RPC's; a failed HOLD retains the row; a reopen of a retained row fails with the edge's error, then opens afresh | `open_lease_shared_and_failed_hold`, `fuse_lease_retention_close_verdicts` | `make tests/test_open_interface && ./tests/test_open_interface open_lease_shared_and_failed_hold fuse_lease_retention_close_verdicts` |
  | `efs_open_acquire_created`: adopts the token, `handed_off`, `EFS_ERR_INVAL` without gen or token, STALE as a completed revocation | `fuse_lease_retention_combined`, `fuse_lease_retention_adoption` | `make tests/test_open_interface && ./tests/test_open_interface fuse_lease_retention_combined fuse_lease_retention_adoption` |
  | `efs_close_note`: 1 on the last close with `*leased`, row kept pending the CLOSE | `open_busy_admission`, `test_two_opens` | `make tests/test_open_interface && ./tests/test_open_interface open_busy_admission test_two_opens` |
  | `efs_open_release_pending`: `EFS_OK` and the row freed, `EFS_ERR_BUSY` while unanswered, the RPC's error otherwise | `fuse_lease_retention_close_verdicts`, `open_retry_lock_budget` | `make tests/test_open_interface && ./tests/test_open_interface fuse_lease_retention_close_verdicts open_retry_lock_budget` |
  | `efs_open_close_unadmitted`: 1 only while the retained CLOSE has no identity | `open_retry_lock_budget` | `make tests/test_open_interface && ./tests/test_open_interface open_retry_lock_budget` |
  | `efs_open_retry_pass`: oldest first, every identity gets a turn, `EFS_ERR_BUSY` past the deadline or on a held stripe | `test_fuse_lease_retention_retry_order`, `fuse_lease_retention_retry_turns`, `open_retry_lock_budget` | `make tests/test_open_interface && ./tests/test_open_interface fuse_lease_retention_retry_order fuse_lease_retention_retry_turns open_retry_lock_budget` |
  | `efs_open_retry_start`, `efs_open_retry_stop`: the retry thread drains a retained CLOSE and stops on request | `fuse_lease_retention_retry_thread` | `make tests/test_open_interface && ./tests/test_open_interface fuse_lease_retention_retry_thread` |
  | `efs_client_ino_is_open`: a retained row counts as open | `open_lease_shared_and_failed_hold`, `fuse_lease_retention_retry_turns`, `test_retained_close` | `make tests/test_open_interface && ./tests/test_open_interface open_lease_shared_and_failed_hold fuse_lease_retention_retry_turns test_retained_close` |
  | `efs_open_retained`: counts rows waiting for a confirmed last close, across colliding buckets | `open_lease_hashed_rows`, `test_retained_rows` | `make tests/test_open_interface && ./tests/test_open_interface open_lease_hashed_rows test_retained_rows` |
  | `efs_open_append_handoff`, `efs_open_append_burst_note`, `efs_open_append_burst_published`: 32 writes or one second publish, the 100 ms handoff, a failed publication publishes again | `test_append_burst` | `make tests/test_open_interface && ./tests/test_open_interface test_append_burst` |
  | `open_edge_mu`: the inode stripe serializes the lease edges and is taken before the table mutex | `open_lease_edge_serialization`, `open_busy_deadline` | `make tests/test_open_interface && ./tests/test_open_interface open_lease_edge_serialization open_busy_deadline` |
  | Lock: the table mutex is never held across an RPC; concurrent opens and closes on distinct inodes (TSan entry) | `test_concurrent_churn` | `make tests/test_open_interface && ./tests/test_open_interface test_concurrent_churn` |
  | Pending CREATEs: `efs_pending_create_alloc`, `efs_pending_create_free`, `efs_pending_create_queue`, drained by the retry pass | none; whole-module fallback | `python3 scripts/tests_for.py client/efs_fuse.open` |
  | `wb_lock_budget` (`test_wb_interface.c` defines its own) | none; whole-module fallback | `python3 scripts/tests_for.py client/efs_fuse.open` |
  | `efs_fuse_open_ino`, `efs_fuse_read_ino` errno answers; `fuse_fi_direct_io`, `fuse_odirect_unaligned` | none; whole-module fallback | `python3 scripts/tests_for.py client/efs_fuse.open` |

  `writeback.h` ([`tests/test_wb_interface.c`](../../../tests/test_wb_interface.c);
  every case is a `test_...` function):

  | Operation or invariant | Cases | Command |
  |---|---|---|
  | Before the pool starts: `efs_wb_pending` 0, both drains `EFS_OK`, `efs_wb_stop` a no-op | `test_unstarted` | `make tests/test_wb_interface && ./tests/test_wb_interface test_unstarted` |
  | `efs_wb_enqueue_owned` starts the workers on the first enqueue and returns the job's PUT rc; a stopped pool restarts fresh | `test_start_stop_restart` | `make tests/test_wb_interface && ./tests/test_wb_interface test_start_stop_restart` |
  | `efs_wb_enqueue_owned` `EFS_ERR_NOMEM` when a worker start fails, buffer released; a writer racing the undo gets `EFS_ERR_IO` | `test_failed_start` | `make tests/test_wb_interface && ./tests/test_wb_interface test_failed_start` |
  | `efs_wb_enqueue_owned` `EFS_ERR_BUSY` when the ring stays full for 8 s | none; whole-module fallback | `python3 scripts/tests_for.py client/efs_fuse.writeback` |
  | `efs_wb_pending`: per inode and for the whole pool, with a PUT in flight | `test_deadline_drains` | `make tests/test_wb_interface && ./tests/test_wb_interface test_deadline_drains` |
  | `efs_wb_sync`, `efs_wb_sync_ino`: the sticky error is returned once, by its own inode's drain or the global drain; another inode is never refused | `test_sticky_error` | `make tests/test_wb_interface && ./tests/test_wb_interface test_sticky_error` |
  | Both drains `EFS_ERR_BUSY` at the caller's deadline; the pending work and the sticky error stay | `test_deadline_drains` | `make tests/test_wb_interface && ./tests/test_wb_interface test_deadline_drains` |
  | `efs_wb_stop`: joins after the in-flight PUT, does not consume the sticky error, races enqueues, drains and a second stopper | `test_drain_across_stop`, `test_drains_race_stop` | `make tests/test_wb_interface && ./tests/test_wb_interface test_drain_across_stop test_drains_race_stop` |
  | `bounce_alloc`, `bounce_release`, `efs_fuse_log_err` | none; whole-module fallback | `python3 scripts/tests_for.py client/efs_fuse.writeback` |

  `find_res.h` ([`tests/test_find_res_interface.c`](../../../tests/test_find_res_interface.c)):

  | Operation or invariant | Cases | Command |
  |---|---|---|
  | `find_res_intern`, `find_res_open`, `find_query_fill_stat`, `find_query_read`: each fetches first and serves the caller's fresh result; READ at EOF and at an offset | `serve_own_result` | `make tests/test_find_res_interface && ./tests/test_find_res_interface serve_own_result` |
  | `find_res_open` `-ESTALE` when the bytes no longer match the interned version | `serve_own_result` | `make tests/test_find_res_interface && ./tests/test_find_res_interface serve_own_result` |
  | Fetch errors pass through (`EACCES`, `EBUSY`, `-EINVAL` for an unparsable term); a failed fetch never serves the table's bytes | `errors_never_serve_cached_bytes` | `make tests/test_find_res_interface && ./tests/test_find_res_interface errors_never_serve_cached_bytes` |
  | Another uid or gid gets its own result; the publisher hint is kept per caller and term | `other_uid_gets_its_own`, `serve_own_result` | `make tests/test_find_res_interface && ./tests/test_find_res_interface other_uid_gets_its_own serve_own_result` |
  | Slots: 40 terms through the 16 slots | `many_terms_through_sixteen_slots` | `make tests/test_find_res_interface && ./tests/test_find_res_interface many_terms_through_sixteen_slots` |
  | The 64 MiB byte budget evicts the oldest; a larger result answers `-ENOMEM` | `byte_budget` | `make tests/test_find_res_interface && ./tests/test_find_res_interface byte_budget` |
  | A third concurrent fetch answers `-EBUSY` without an RPC | `third_fetch_is_busy` | `make tests/test_find_res_interface && ./tests/test_find_res_interface third_fetch_is_busy` |
  | Task EC F1, F2: same term at once, each caller served its own groups' result, never a miss | `same_term_pairs` | `make tests/test_find_res_interface && ./tests/test_find_res_interface same_term_pairs` |
  | Four callers with distinct uids on shared and private terms (TSan entry) | `concurrent_callers` | `make tests/test_find_res_interface && ./tests/test_find_res_interface concurrent_callers` |
  | `find_res_clear` frees every slot (ASan leak check) | `concurrent_callers` (ends with `find_res_clear()`) | `make tests/test_find_res_interface && ./tests/test_find_res_interface concurrent_callers` |
  | `find_res_intern` `-ENOMEM` with no free node; `-EOVERFLOW`, `-ENAMETOOLONG`, `-EILSEQ` | none; whole-module fallback | `python3 scripts/tests_for.py client/efs_fuse.find_res` |

- Gate evidence for the split:
  [efs-fuse-split-20261104](../../../docs/status/2026-11/efs-fuse-split-20261104.md);
  for the per-area headers:
  [efs-fuse-interfaces-20261104](../../../docs/status/2026-11/efs-fuse-interfaces-20261104.md).

## Open findings

- None owned here. [K35](../../../docs/status/alpha-1.0-2plus1-checklist.md)
  (fsync `EIO` with a member down) lives in `../data_commit.c`; the
  intermittent TSan race seen during the split is in `../write.c`
  (`efs_dcache_maybe_reclaim`), not in this module.
