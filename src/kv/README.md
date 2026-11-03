# src/kv/ — kv — ordered applied state

## Files

- `kv_mem.c` — in-memory ordered KV for the simulator and unit tests.
- `kv_lsm.c` — on-disk LSM store: memtable plus segment list, one internal lock serializing put/get/del/scan/batch.
- `kv_lsm_internal.h` — private structs shared by the `kv_*.c` files.
- `kv_wal.c` — write-ahead log for the memtable.
- `kv_seg.c` — immutable sorted segment files and sparse block index.
- `kv_compact.c` — memtable→segment flush and segment merging.
- `kv_snap.c` — snapshot export/import of the applied state.
- `kv_key.c` — key encoding.
- `kv_order.h` — the one unsigned byte ordering used in memory, segments and snapshot merge.
- `kv_candidates.h` — live LSM candidate selection; no disk-format change.

## Contract

- Interfaces: [`efs/kv.h`](../../include/efs/kv.h), [`kv_lsm.h`](../../include/efs/kv_lsm.h), [`kv_snap.h`](../../include/efs/kv_snap.h), [`kv_key.h`](../../include/efs/kv_key.h).
- `kv_lsm.h` states the thread-safety rule (concurrent operations serialized internally; scan callbacks must not take KV or host locks) — read it there, not here.
- Invariants: I10 (metadata durability) is the one that depends on WAL/segment ordering, in [architecture §4](../../docs/how-it-works/architecture.md#4-invariants); the on-disk layout is not stated in architecture.md.

## Calls

- May call: `common/` helpers via `efs/*.h` (observed: `checksum`, `fault_exit`, `telemetry`, `raft`, `meta_apply`).
- Called by: `meta/`, `server/`, `sim/`, `bench/`.
- Observation: `kv/` includes `efs/meta_apply.h` and `efs/raft.h`; whether that is allowed is not stated in [developing.md](../../docs/how-it-works/developing.md).

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py kv`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
