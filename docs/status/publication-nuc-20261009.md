# Agent 4 — D29/W88 and D30/W23/W89 on NUC, October 9, 2026

Branch: `devel-compaction-pressure`, integrated with `devel` at `863054ca`.
This is progress evidence, not package closure or pressure-policy activation.

## Publication and replay

The real client session admission helper passed ordered CREATE/REGISTER/ESTABLISH,
exact retry, redirects, bounded overload retries, lost ACK, deadline and fenced
session tests. The NUC private four-daemon test exercised that actual helper,
committed-but-unapplied mapping proof, all-daemon crash, exact receipt locator,
retirement floor and refusal to revive the fenced session after restart.

Both same-group and cross-group actual-fragment replay fixtures passed on NUC:
eight-span history folding/eviction, lost final ACK, physical reclamation, old
object re-PUT, exact cold FUSE bytes and durable retirement after another crash.
Both passed again with the bounded sweep implementation.

## Bounded lane cleanup

Historical LANE_SWEEP loops over an entire lane despite its small wire payload.
A new LANE_SWEEP_BATCH command names at most eight exact chunk indices. Apply
queues the source deletes and every base/delta GC reference in one atomic batch,
then retains the lane while any chunks remain. BUSY is completed partial progress:
the log advances and the reaper proposes another command. IO/NOMEM still prevent
apply advancement. Legacy command semantics remain unchanged for log replay.

A retry must name the same keys: simply rescanning the first eight keys on each
apply attempt would consume a different batch after a storage/apply-marker crash.
The regression verifies that a failed atomic write preserves references, an
exact retry preserves the next batch, all nine possible object identities per
chunk are queued, malformed key sets are rejected, and the final step is
idempotent. An actual LSM WAL reopen after an ungraceful child exit independently
verifies that the next five chunks survive replay of the first eight-key command.
The strict NUC build and complete integrated suite passed.
The production decoder/verdict harness covers malformed lengths, batch caps,
BUSY progress, storage failures and forwarded verdicts.

The reaper uses bounded selection when it hosts the lane group. The existing
remote whole-lane forwarding path remains pending a bounded selection RPC. REAP_DONE
also still loops over append reservations/receipts. Those paths and the admission
expansion charge must be audited before claiming a universal metadata-work bound.

## Cluster handling and remaining work

All test daemons and clients ran as Linux user `efs`, direct I/O and TCP, with
commands disabled and pressure enabled only in the private fixtures. Installed
stores and binaries were not replaced. The approved second mount was borrowed
under the existing reservation locks and restored to the installed cluster.
A restoration descriptor leak was found and corrected: the replacement installed
FUSE client now closes the reservation descriptors before launch. No reservation
remains held by the restored process.

Production FUSE still uses legacy REPORT publication. Completing the package
requires pre-write FileID and authoritative geometry admission, immutable request
and accepted-byte ownership across every write/flush path, ordered terminal receipt
consumption/retirement, bounded remaining cleanup paths, and sustained NUC pressure
and recovery evidence with the final production client. Pressure remains off on
the installed cluster; the new session helper alone does not complete this migration.

Evidence: [NUC run directory](../../results/measure/20261009-nuc-publication/README.md).
