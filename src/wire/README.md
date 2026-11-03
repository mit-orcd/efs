# src/wire/ — wire — the protocol codec

## Files

- `wire.c` — versioned frame and command encode/decode; no state.

## Contract

- Interface: [`efs/wire.h`](../../include/efs/wire.h) (decode packs commands into a caller-owned buffer); message ids in [`efs/protocol.h`](../../include/efs/protocol.h).
- Protocol and compatibility rules: [architecture §7](../../docs/how-it-works/architecture.md#7-protocol-summaries).
- Versioning policy beyond that: not stated.

## Calls

- May call: nothing but `efs/raft.h` types (observed includes: `raft`, `wire`).
- Called by: `raft/`, `common/`, `server/`, `data/` transports. Encode/decode only ([developing.md](../../docs/how-it-works/developing.md)).

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py wire`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
