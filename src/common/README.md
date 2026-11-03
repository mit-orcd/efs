# src/common/ — common — shared primitives

## Files

- `common.c`, `version.c`, `log_ts.c`, `telemetry.c`, `batch_obs.c` — utilities, version string, log timestamps, telemetry mailbox, fixed-memory batch histograms.
- `network.c`, `rdma.c`, `rdma_message.c` — TCP/RDMA connections and message framing; `network.c` owns the down-mark cache.
- `protocol.c`, `checksum.c`, `record_decode.c`, `frag_record.c`, `placement.c` — message helpers, checksums, fragment-record decoding, identity-bound fragment record, placement hash.
- `member_view.c`, `membership.c` — live client member view and the committed membership record (K33/W153).
- `session_revoke.c`, `request_receipt.c` — I23 revocation barrier coordinator, W155 receipts.
- `publication.c`, `publication_ack.c` — shared object namespace and ACK helpers.
- `pack_ledger.c`, `packed_reader.c`, `packed_segment.c` — packed segment ledger, reader and format.
- `settings.c` — effective-settings table (one row per A6 inventory entry).
- Header-only: `parse_host_port.h`, `query_stage_trace.h`, `rdma_memcheck.h`, `recorder_start.h`, `recorder_stop.h`, `stage_limit.h`, `tcp_counts.h`.

## Contract

- Interfaces: the matching `efs/*.h` headers in [`include/efs/`](../../include/efs/) (`network.h`, `rdma.h`, `placement.h`, `settings.h`, `membership.h`, …).
- Invariants: I25 (integrity), I20–I23 helpers, per [architecture §4](../../docs/how-it-works/architecture.md#4-invariants); placement formulas have one home in [architecture §5](../../docs/how-it-works/architecture.md#5-state-placement).
- Settings inventory contract: [W171 plan](../../docs/status/2026-11/structure-plan-20261102.md) (phase 2c).
- Lock ownership: per-file, in each header; not stated centrally.

## Calls

- May call: only the C library and other `common/` files.
- Called by: every other directory.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py common`
- `python3 scripts/tests_for.py common/checksum`
- `python3 scripts/tests_for.py common/frag_record`
- `python3 scripts/tests_for.py common/network`
- `python3 scripts/tests_for.py common/pack_ledger`
- `python3 scripts/tests_for.py common/packed_reader`
- `python3 scripts/tests_for.py common/packed_segment`
- `python3 scripts/tests_for.py common/placement`
- `python3 scripts/tests_for.py common/protocol`
- `python3 scripts/tests_for.py common/publication`
- `python3 scripts/tests_for.py common/publication_ack`
- `python3 scripts/tests_for.py common/query_stage_trace`
- `python3 scripts/tests_for.py common/rdma`
- `python3 scripts/tests_for.py common/rdma_memcheck`
- `python3 scripts/tests_for.py common/record_decode`
- `python3 scripts/tests_for.py common/recorder_start`
- `python3 scripts/tests_for.py common/request_receipt`
- `python3 scripts/tests_for.py common/session_revoke`
- `python3 scripts/tests_for.py common/settings`
- `python3 scripts/tests_for.py common/tcp_counts`
- `python3 scripts/tests_for.py common/telemetry`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
