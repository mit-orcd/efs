# src/data/ — data — erasure coding and transports

## Files

- `erasure.c` — 2+1 XOR parity encode/decode; stateless.
- `store_mem.c` — fault-injecting in-memory fragment store behind the `efs/store.h` vtable.
- `transport_loop.c` — in-process paired queues (simulator/unit tests; single-threaded).
- `transport_conn.c` — adapter over an existing `efs_conn` (TCP or RDMA); ownership by the `own` flag.

## Contract

- Interfaces: [`efs/erasure.h`](../../include/efs/erasure.h), [`store.h`](../../include/efs/store.h), [`transport.h`](../../include/efs/transport.h) (recv mallocs the payload; the caller frees).
- Invariants: I11–I15, I25, in [architecture §4](../../docs/how-it-works/architecture.md#4-invariants); publication in [§7.3](../../docs/how-it-works/architecture.md#73-data-publication).
- The production NVMe store is `server/store.c`, not here.

## Calls

- May call: `common/` (`network`, `protocol`) via headers.
- Called by: `client/`, `server/`, `sim/`.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py data`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
