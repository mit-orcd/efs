# src/meta/ — meta — POSIX operation state machine

## Files

- `meta_apply/` — applies metadata commands to the KV (create/link/rename/setattr/append, publication, GC, packing, transactions' local steps), one file per area; its [README](meta_apply/README.md) maps symptom to file, header and tests. State beyond the KV: the two per-anchor counts in `gc.c` and the STALE reason in `publish.c`.
- `txn.c` — cross-shard transaction coordinator/participant records and decisions.
- `session.c` — pure session and open-lease state machine over one KV, no I/O.
- `lock.c` — POSIX/OFD lock state.
- `expiry.c` — expiry/scratch lifetime rules.
- `dir_layout.c`, `dir_spread.c` — directory placement, layout epochs and spreading.
- `name_index.c` — name→inode index helpers.
- `pack_mapping.c` — mapping of packed files to segments.
- `put_ticket.c`, `put_orphan.c` — durable PUT tickets (bounded admission, replay floors) and abandoned-upload collection (D31).
- `append_plan.c` — transactional APPEND completion.
- `metadata.c` — the client-side staging table (not the server state machine).
- `query_catalog.c`, `query_policy.c`, `query_disk.c`, `query_walk.h`, `query_disk_*.h`, `query_build_time.h` — rebuildable local query cache over the authoritative KV; helper headers are included by `query_disk.c` only.

## Contract

- Interfaces: [`efs/meta_apply.h`](../../include/efs/meta_apply.h), [`meta_cmd.h`](../../include/efs/meta_cmd.h), [`txn.h`](../../include/efs/txn.h), [`session.h`](../../include/efs/session.h), [`lock.h`](../../include/efs/lock.h), [`metadata.h`](../../include/efs/metadata.h), [`put_ticket.h`](../../include/efs/put_ticket.h), [`query_catalog.h`](../../include/efs/query_catalog.h).
- Invariants: I5–I9, I16–I19, I23, I24, stated in [architecture §4](../../docs/how-it-works/architecture.md#4-invariants); operation matrix in [§6](../../docs/how-it-works/architecture.md#6-operation--participant-matrix-authoritative).
- `metadata.h` states that its callers hold table locks; no socket or FUSE calls inline ([developing.md](../../docs/how-it-works/developing.md)).

## Calls

- May call: `kv/`, `raft/` headers and `common/` helpers (observed private includes: `common/query_stage_trace.h`, `common/stage_limit.h`).
- Called by: `server/` (handlers, raft host), `client/` (`metadata.c` staging table), `sim/`, `mgmt/`.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py meta/append_plan`
- `python3 scripts/tests_for.py meta/expiry`
- `python3 scripts/tests_for.py meta/lock`
- `python3 scripts/tests_for.py meta/meta_apply` (per file: `meta/meta_apply.<file>`, see [meta_apply/README.md](meta_apply/README.md))
- `python3 scripts/tests_for.py meta/metadata`
- `python3 scripts/tests_for.py meta/name_index`
- `python3 scripts/tests_for.py meta/pack_mapping`
- `python3 scripts/tests_for.py meta/put_ticket`
- `python3 scripts/tests_for.py meta/query_catalog`
- `python3 scripts/tests_for.py meta/query_disk`
- `python3 scripts/tests_for.py meta/query_policy`
- `python3 scripts/tests_for.py meta/session`
- `python3 scripts/tests_for.py meta/txn`

## Open findings

- [K5](../../docs/status/alpha-1.0-2plus1-checklist.md) — session fencing (W134/W72), VERIFY.
- [K43](../../docs/status/alpha-1.0-2plus1-checklist.md) — writer `EBUSY`/`EIO` when a member is SIGKILLed; abort-decided reap fix, VERIFY.
