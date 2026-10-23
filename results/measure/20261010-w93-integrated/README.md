# W93 final branch integration — Oct 10

The completed `5b2df4bd32be` acceptance is retained in the neighboring
`20261010-w93-closure` directory. Concurrent changes arrived during that run.
`merge-tips.json` freezes the final integration boundary, including the new
`devel-intel` branch. This revalidation uses engine `37ad6a8790c8-integration`,
separate build/fixture roots and fresh service workload directories.

Native, full default/candidate durability/POSIX/peer, maximum KV/marker EIO,
partition and per-group cohort gates precede service deployment. The original
ABBA method is repeated on this integrated binary, with preserved service stores
and default restoration/cold checks. The completed acceptance is recorded below.

Remote roots: `/data1/efs/evidence/w93-integrated-build` and
`/data1/efs/evidence/w93-integrated-20261010`. Source/controller hashes are retained.

The first merged engine cdc5c265 passed native/partition checks but returned
EAGAIN in concurrent_appends (211/212). Retained logs identify a saturated
16-receipt window and one-second admission while holding the append stripe.
Commit 37ad6a87 retries before reservation outside that stripe within the
inherited deadline. Actual-source regression gates catch the old code.
Original failures and source provenance are retained separately.

Integrated acceptance is now complete. The rebuilt native suite, partition /
publication / session / voter gates, three full POSIX 212/212 + peer 64/64
configurations, sync/append/dirty-stop/shutdown/WAL/GC and maximum 1,000 ms
KV/marker EIO/crash gates all pass. Maximum faults also pass with overlap/no-op
combined. All 32 actual-core one/two-group, one/sixteen-entry SATA/NVMe cases
retain own-group ACK coverage and exact cold records. Initial cdc failures and
old-source negative exits are preserved separately, not counted as passes.

Four 900-second ABBA rates are 60.67 / 83.18 / 75.06 / 60.54 files/s:
**+30.54%** across candidate/control means. All workers drain without errors or
caps; all replicas cover their final leader cuts. Operation percentiles improve;
all mixed/idle tails and occasional longer maxima remain visible. The matched
6,000-read/group default/cadence-only/full-bundle/default matrix shows lower
full-bundle p50/p95/p99 against both controls. Inbox pressure remains separate
W97 work; this is a bundled local-workload result, not a hardware ceiling.

Both cold verification passes check 512 first/last payloads through two mounts.
Final state at 2026-10-10 11:30:26 UTC proves all four active daemon hashes and
both active FUSE hashes match the installed merged binaries: TCP/direct IO,
10 ms defaults, FLOW=1; commands, diagnostics, overlap/no-op, group drain and
early retirement off. No owned loader/profiler remains. The accepted bundle is
opt-in. Clean shutdown is required before downgrade to pre-cursor code.

`source-provenance.json` records the engine and frozen controller hashes;
`4a962397` adds active-FUSE identity checks outside timed workloads.
`binary-provenance.json` distinguishes human versions from server/FUSE HELLO IDs.
`artifact-manifest.json` hashes retained files; compiled binaries, rollback
binaries and perf control FIFOs are intentionally omitted. Original private
stores and rollback binaries remain on NUC. Fixture collection copies only
small metadata and compressed logs, never service payload stores. The unused
`debug-gc.py` is retained diagnostic history, not the executed gate source.

**Raw evidence note (2026-10-23):** the bulk data in `w93-integrated-20261010`, `w93-integrated-build` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
