# src/client/ — client — efs-fuse

## Files

- `efs_fuse/` — the FUSE adapter, one file per area with its interface and contract in `<area>.h` next to it (read `efs_fuse/README.md`, then the area's header; `fuse_internal.h` is the 80-line shared core; the concatenation order is `MODULES["efs_fuse"]` in `tests/csrc.py`): `support.c` crash handlers, stats, features, virtual inodes, invalidation, find, access checks; `find_res.c` the `.find` result cache; `getattr.c` lookup memo, getattr, readdir; `open.c` open references, leases, open, read; `writeback.c` write-back pool; `append.c` append and pressure flush; `write.c` write admission, store, fsync, flush; `namespace.c` create, mkdir, unlink, attributes, link, truncate; `release.c` release, drain, rename, lseek; `plock_init.c` POSIX locks and `FUSE_INIT`; `lookup.c` lookup and setattr; `ll_run.c` operation bodies; `main.c` destroy, gated wrappers, op table, `main`. `client.c` — client setup, connection pool and mount state; `client.c` — client setup, connection pool and mount state; `efs_bench.c` — benchmark driver.
- `ops.c`, `read.c`, `write.c`, `inode_rpc.c` — path logic, reads, writes and inode RPCs; `write.c` owns the dirty cache.
- Map of `write.c` by concern (state, mutexes, entry points, logs, tests, lock order): [write-path-map.md](../../docs/how-it-works/write-path-map.md); `make docs-check` keeps it complete. Where to start:

  | Symptom | Concern |
  | --- | --- |
  | `dcache-flush-fail stage=put` or `stage=put-window` | Flush pipeline, PUT window |
  | `dcache-inode-gone`, `force-discard` | Flush pipeline, Write-back error state |
  | `put_fragments ... only N/M peers live`, `... placing again`, `put quota backpressure` | Fragment PUT send, Node notes and rerouting |
  | `put_fragments ... degraded acknowledgement`, `put-reply fault` | PUT replies and degraded acknowledgements |
  | `report-stuck`, `report-loop`, `report-sync-fail stage=`, `report-slot-timeout` | REPORT driver |
  | `report-pressure`, `report-admission timeout` | REPORT admission and publication slots |
  | `report-stale`, `report-replay`, `report-span-lost` | Stale replay |
  | `publish-contention`, `publish-stalled` | Publication units |
  | `efs: putid table NOMEM`, `rejected unowned staging observation` | PUT ids and ticket ledger |
  | `mtime-pin: table full` | Dirty sets and mtime pins |
  | `close()` or `fsync()` returns EIO | Write-back error state, then the concern of the log line before it |
  | dirty bytes high, reclaim threads busy | Reclaim |
  | write blocks or fails before any PUT | PUT pool and write entry, Dcache store and read |
- `put_window.c` — the PUT window (per-inode count of PUTs between marking a slot clean and recording its chunk; contract in `put_window.h`, guide below); `writer_state.c` — per-file writer state; `data_commit.c` — per-stripe fsync commit record (K35).
- `stage_evict.c` — staging-table evictor; `bufpool.c` — shared body/scratch budget; `node_cache.c` — peer up/down marks.
- `publication_ack_owner.c`, `publication_writeback.c`, `put_ticket_owner.c` — queues owning publication ACKs, writeback intents and the PUT ticket ledger.
- `stop_phase.c` — unmount progress observer; `commands/` — Python generators for command help (not C).
- Header-only: `client_internal.h` (shared client state), `metadata_read_route.h`, `read_peer_order.h`, `reply_buffers.h`, `sparse_metadata.h`, `stop_control.h`.

## Contract

- Interface: [`client_internal.h`](client_internal.h) (no cache lock may be held across RPCs); public headers `efs/metadata.h`, `efs/writer_state.h`, `efs/write_extent.h`.
- Invariants: I12–I15, I20–I24 on the client side, per [architecture §4](../../docs/how-it-works/architecture.md#4-invariants) and the FUSE contract [§7.7](../../docs/how-it-works/architecture.md#77-fuse--kernel-interface-contract).
- Lifetime rules for ACK queues, buffers and stop callbacks are in the comments of their headers; not summarized here.

## Calls

- May call: `common/`, `wire/`, `meta/metadata.c` staging table, `data/` via headers. Observed private includes: `bench/*`, `common/*`.
- Called by: tests and `mgmt/` (observed `client/stop_control.h`). It must not call `server/` internals.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py client/bufpool`
- `python3 scripts/tests_for.py client/client_internal`
- `python3 scripts/tests_for.py client/data_commit`
- `python3 scripts/tests_for.py client/efs_fuse` (all 56; one file at a time: `client/efs_fuse.open`, `.append`, `.writeback`, `.write`,
  `.namespace`, `.release`, `.getattr`, `.lookup`, `.ll_run`, `.plock_init`, `.support`, `.find_res`, `.main`; see [`efs_fuse/README.md`](efs_fuse/README.md))
- `python3 scripts/tests_for.py client/inode_rpc`
- `python3 scripts/tests_for.py client/node_cache`
- `python3 scripts/tests_for.py client/ops`
- `python3 scripts/tests_for.py client/read`
- `python3 scripts/tests_for.py client/reply_buffers`
- `python3 scripts/tests_for.py client/stage_evict`
- `python3 scripts/tests_for.py client/write`

**PUT window (`put_window.c`).** Owns the per-inode count of PUTs that have marked a dcache slot clean but not yet recorded the chunk in a dirty set: 512 slots, an overflow count, the all-inode total, one private mutex and condition variable. `write.c` calls `put_win_open` and `put_win_close` around a PUT and `put_win_wait` (flush) or `efs_dcache_put_win_wait` (readers) to wait; it never reads the state. Contract in [`put_window.h`](put_window.h): `put_win_mu` is a leaf taken under `dcache_io_mu` and `dcache_mu`; every open is closed once on every exit (a close without an open is a caller error the owner does not detect); a wait returns 0, 1, `EFS_ERR_BUSY` at the earlier of its bound and the RPC deadline, or `EFS_ERR_IO`, and a close wakes it. Interface test: [`tests/test_put_window_interface.c`](../../tests/test_put_window_interface.c), every case a `test_...` function, `./tests/test_put_window_interface --list` names them.

| Operation or invariant | Cases | Command |
|---|---|---|
| `put_win_open` then `put_win_close` leaves the inode's window closed; two opens need two closes; windows of other inodes do not block | `test_open_close_closes`, `test_two_opens_need_two_closes`, `test_other_inode_not_blocked` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_open_close_closes test_two_opens_need_two_closes test_other_inode_not_blocked` |
| `put_win_wait` returns 0 at once when nothing is open | `test_wait_returns_at_once_when_closed` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_wait_returns_at_once_when_closed` |
| A `put_win_wait` that overlaps a `put_win_close` ends (returns 1 if it blocked, 0 if the close came first); the wake itself is not pinned | `test_close_wakes_wait` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_close_wakes_wait` |
| `put_win_wait` gives up with `EFS_ERR_BUSY` at its own bound and at the caller's RPC deadline, and releases the mutex | `test_wait_gives_up_at_its_bound`, `test_rpc_deadline_caps_the_wait` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_wait_gives_up_at_its_bound test_rpc_deadline_caps_the_wait` |
| `efs_dcache_put_win_wait`: 0 without the mutex when no window is open, else the 8 s wait | `test_reader_wait` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_reader_wait` |
| More inodes than slots: the overflow count makes every inode busy until its window is closed | `test_more_inodes_than_slots` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_more_inodes_than_slots` |
| Concurrent open, close and wait from several threads end closed (TSan entry) | `test_concurrent_open_close_ends_closed` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_concurrent_open_close_ends_closed` |
| A close without a matching open is a caller error the owner does not detect: with nothing open it changes nothing; with another window open it takes the all-inode total to 0, so `efs_dcache_put_win_wait` says closed while `put_win_wait` still sees the slot | `test_stray_close_with_nothing_open`, `test_stray_close_hides_an_open_window_from_readers` | `make tests/test_put_window_interface && ./tests/test_put_window_interface test_stray_close_with_nothing_open test_stray_close_hides_an_open_window_from_readers` |

**Caller proof (`tests/test_put_window_callers.py`).** Every exit of a flusher closes the window it opened (real `put_window.c`, flusher functions cut out of `write.c`, cache and PUT stubbed): PUT ok, PUT failed and re-dirtied, inode retired, failed batch drain, merge-base allocation failure, merge-base fetch failure, fetch finding the inode gone; `scripts/put_window_mutation.py` deletes each of the nine `put_win_close` calls and lists the scenarios that fail. Scenarios `ok` `fail` `retired` `drain` `nomem` `fetch` `fetch_retired`, one per run: `python3 tests/test_put_window_callers.py ok`; `python3 tests/test_put_window_callers.py fail`; `python3 tests/test_put_window_callers.py retired`; `python3 tests/test_put_window_callers.py drain`; `python3 tests/test_put_window_callers.py nomem`; `python3 tests/test_put_window_callers.py fetch`; `python3 tests/test_put_window_callers.py fetch_retired`.

## Open findings

- [K35](../../docs/status/alpha-1.0-2plus1-checklist.md) — fsync COMMIT on every node returns `EIO` while a member is down (`data_commit.c`).
