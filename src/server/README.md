# src/server/ — server — efsd

## Files

- `efsd.c` — daemon entry, startup/stop; `cluster.c` — cluster formation and membership wiring; `discovery.c/.h` — local-subnet discovery (never defines membership).
- `raft_host.c` — the production Raft host (hosted groups, apply pump, ReadIndex, publication, GC, sessions, transactions); owns every hosted group's state. **The file the [W171 plan](../../docs/status/2026-11/structure-plan-20261102.md) splits next** (the split waits for W123). ReadIndex code (`host_read_*`, `host_wait_*`, the view, the waiter slots, the follower RPC): [entry map](../../docs/status/2026-11/readindex-entry-map-20261108.md#entry-map), checked by `make docs-check`.
- `handler.c` — request dispatch per connection; `thread.c` — conn/writer thread stacks; `writer.c` — writer pool; `peer_pool.c` — persistent server→server connections.
- `server_internal.h` — shared `g_server`-style state, export lifetime (use-count guarded by `s->lock`); the anti-example in developing.md.
- `member_store.c/.h` — durable boot counter, member identity and committed membership under `<meta root>/mdraft`.
- `store.c`, `store_nvme.c` — NVMe fragment store and its `efs/store.h` backend; `storage_roots.c/.h` — exclusive root locks, root marker, ancestry barrier.
- `put_upload.c`, `put_fence.c`, `put_orphan.c` — the one data PUT path (ticket-admitted), stripe certificates, orphan fragment delete.
- `packer.c`, `segment_store.c`, `segment_rebuild.c` — W125 packing, packed segment store, rebuild on return (K37).
- `reclaim.c` — W124 node reclaim; `recovery.c` — verified recovery after an unclean stop (K36).
- `iostats.c` — always-on relaxed-atomic I/O counters.
- Header-only helpers: `apply_ring.h`, `apply_wait.h` (W159), `dest_fault.h` (test hook), `directory_sync_group.h`, `publication_session_fast.h`, `query_export_lease.h`, `query_refresh.h`, `read_index_pair_remote.h`, `write_prof.h` — state owned by the includer.

## Contract

- Interface: [`src/server/server_internal.h`](server_internal.h) (private to `server/`); public seams are `efs/store.h`, `efs/transport.h`, `efs/protocol.h`.
- Invariants: I1–I4, I9, I10, I14, I16, I17, I20–I24 are enforced here per [architecture §4](../../docs/how-it-works/architecture.md#4-invariants) and §7.
- Lock ownership: export lifetime is stated in `server_internal.h`; the member-store identity (`s->mstore`) is written and read under the host's `h->mu` once the host runs, other threads read it through `server_member_identity` (K93, `member_store.h`); the host's lock order is not stated in one place.
- Apply visibility of `host_apply`'s scope (staged, committed, visible): [apply-visibility.md](../../docs/how-it-works/apply-visibility.md).
- Split rules and the `raft_host.c` prefix map: [W171 plan](../../docs/status/2026-11/structure-plan-20261102.md).

## Calls

- May call: `raft/`, `kv/`, `meta/`, `wire/`, `data/`, `common/` through `efs/*.h` ([developing.md](../../docs/how-it-works/developing.md)).
- Called by: `bench/` (observed private include `server/server_internal.h`) and tests; nothing else should include server internals.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py server/apply_wait`
- `python3 scripts/tests_for.py server/cluster`
- `python3 scripts/tests_for.py server/directory_sync_group`
- `python3 scripts/tests_for.py server/efsd`
- `python3 scripts/tests_for.py server/handler`
- `python3 scripts/tests_for.py server/member_store`
- `python3 scripts/tests_for.py server/packer`
- `python3 scripts/tests_for.py server/put_fence`
- `python3 scripts/tests_for.py server/put_orphan`
- `python3 scripts/tests_for.py server/put_upload`
- `python3 scripts/tests_for.py server/query_refresh`
- `python3 scripts/tests_for.py server/raft_host`
- `python3 scripts/tests_for.py server/raft_host.read_index`
- `python3 scripts/tests_for.py server/reclaim`
- `python3 scripts/tests_for.py server/server_internal`
- `python3 scripts/tests_for.py server/storage_roots`
- `python3 scripts/tests_for.py server/store`
- `python3 scripts/tests_for.py server/writer`

## Open findings

- [W171](../../docs/status/work-queue.md) — this component's split (`raft_host.c`); [plan](../../docs/status/2026-11/structure-plan-20261102.md).
- [W159](../../docs/status/work-queue.md) — expiry-fixture POSIX run timed out on metadata RPCs of 2–7 s; its row names the raft host.
