# src/client/ — client — efs-fuse

## Files

- `efs_fuse.c` — the FUSE adapter; `client.c` — client setup, connection pool and mount state; `efs_bench.c` — benchmark driver.
- `ops.c`, `read.c`, `write.c`, `inode_rpc.c` — path logic, reads, writes and inode RPCs; `write.c` owns the dirty cache.
- `writer_state.c` — per-file writer state; `data_commit.c` — per-stripe fsync commit record (K35).
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
- `python3 scripts/tests_for.py client/efs_fuse`
- `python3 scripts/tests_for.py client/inode_rpc`
- `python3 scripts/tests_for.py client/node_cache`
- `python3 scripts/tests_for.py client/ops`
- `python3 scripts/tests_for.py client/read`
- `python3 scripts/tests_for.py client/reply_buffers`
- `python3 scripts/tests_for.py client/stage_evict`
- `python3 scripts/tests_for.py client/write`

## Open findings

- [K35](../../docs/status/alpha-1.0-2plus1-checklist.md) — fsync COMMIT on every node returns `EIO` while a member is down (`data_commit.c`).
