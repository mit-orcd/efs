# src/raft/ — raft — the consensus core

## Files

- `raft.c` — the Raft state machine: elections, log replication, membership change; per-group state lives in the struct behind `efs/raft.h`; no I/O inline.
- `raft_log.c` — the Raft log file: record framing, group commit, replay, rotation.
- `raft_disk.c` — durable `efs_raft_store`: per-group state in RAM, authority on disk.
- `raft_disk_internal.h` — shared by `raft_disk.c` and `raft_log.c` only (group state, store callbacks).
- `raft_mem.c` — in-memory store for the simulator and unit tests.

## Contract

- Interfaces: [`include/efs/raft.h`](../../include/efs/raft.h) (pure: no sockets, no threads; the caller delivers messages and ticks; clocks are never a correctness input), [`raft_disk.h`](../../include/efs/raft_disk.h), [`raft_frame.h`](../../include/efs/raft_frame.h).
- Invariants preserved: I1–I4 and I18, stated in [architecture §4](../../docs/how-it-works/architecture.md#4-invariants); §7.1 is the protocol ([sharded Raft](../../docs/how-it-works/architecture.md#71-sharded-raft-and-linearizable-reads)).
- Purity rule (state machines own no hidden globals and do no inline I/O): [developing.md](../../docs/how-it-works/developing.md).
- Lock ownership and lifetime of a store across threads: not stated here; `raft.h` documents the nested per-thread persistence scope.

## Calls

- May call: `common/` and the `wire` helpers only through `efs/*.h` (observed includes: `batch_obs`, `checksum`, `telemetry`, `wire`, `raft_disk`).
- Called by: `kv/` and `meta/` (state machines), `sim/`, `server/raft_host.c`, `mgmt/`, `bench/`.
- Must not: open sockets or threads; reach into another directory's `.c` ([boundaries](../../docs/how-it-works/developing.md)).

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py raft`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
