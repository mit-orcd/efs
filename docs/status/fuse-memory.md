# FUSE client memory — current state and remaining gates

[Status index](README.md) · [Decisions](decisions.md) ·
[Historical evidence](../archive/fuse-memory-20261007.md)

Review updated Oct 7, 2026. The Oct 5–6 diagnosis, failed runs, A/B evidence and
implementation checkpoints are preserved in the archive. The original
"working-tree fixes, Linux gates pending" heading is superseded by committed
foundation `033a842a` and the recorded NUC gates; targeted omissions below
remain open. A body allocator limit is not a process RSS limit.

## Sparse dirty writes bypass reclaim and cache admission has no hard bound

**W67 — closed after NUC live memory/reserve acceptance.** Shared body admission tracks
read/dirty/PUT/merge ownership and reserves capacity before copying writes.
Default hard budget is 256 MiB, with a 64 MiB drain reserve; dynamic collision
metadata has a separate 8 MiB bound. `EFS_DCACHE_BYTES` defaults to a 128 MiB
soft reclaim target. `EFS_DCACHE_HARD_BYTES` and `EFS_DCACHE_DRAIN_BYTES` configure
the hard budget/reserve. Other tables, TLS, bounce and transport storage remain
outside this bound; oversized exports may require explicit larger budgets.

Local allocation/reservation/failure and ownership regressions passed. The
NUC rebuilt unit suite and broad POSIX gates are recorded in the
[30-round ledger](../../results/measure/20261007-roadmap-rounds/SUMMARY.md).
The [memory closure](memory-closure-20261007.md) completes live failed
publication plus physically exhausted scratch under a 1 GiB/no-swap cgroup,
for both I/O modes and 32+32 / production 256+64 MiB values. Same-mount
admission, cold bytes, concurrent sparse pressure and GC/restart pass.
Keep dirty bytes pinned until the matching commit; never drop on retry count.

## Read/readdir reply buffers leak when FUSE workers exit

**W68 — closed after NUC real-worker and injected-failure gates.** A pthread-key
owner/destructor frees read/readdir reply storage on worker retirement.
Local retirement and TLS/allocation-failure regressions passed. Unit success
alone does not prove production RSS across repeated FUSE worker creation and
retirement. The targeted direct/buffered gates and allocation/TLS/growth variants now pass; see the [30-round gate ledger](gate-30-rounds-20261007.md) for retained evidence.

## W60 — demand reads under speculative pressure

Fixed in `73ce8aaa`: atomic speculative admission at half the normal shared
body budget prevents prefetch from consuming request reservations/drain reserve.
Demand scratch reclaims unpinned clean bodies and retries boundedly; pending
fetches and dirty/reply owners retain storage. Later deadline work bounds
accepted GET/queue waits while preserving caller lifetime.

NUC live 32 MiB A/B: old behavior failed 2000/2000 reads; fixed behavior failed
0/2000 with bytes verified. Full NUC POSIX and units passed. The initial fixture
against older servers is superseded by the aligned-build live run in the
[W60 checkpoint](../../results/measure/20261006-d25-w60-ten-rounds/SUMMARY.md).
**Remaining:** full-tree mixed reads on xorinox, including large files, with
zero demand-read ENOMEM/EIO caused by speculation. No promise of admission
under exhaustion by demand/dirty owners. Sub-chunk accounting and dedicated
diagnostic counters remain follow-ups, not claims of completed fixes.

## Recovery and deployment boundaries

D27 memory admission, retained bytes and reserve are implemented, but strict
whole-call timing and fault/RSS gates remain in [the handoff](in-flight.md).
An installed client binary does not update an existing mount; verify its actual
process and build. Controlled stop must drain while mounted and refuse teardown
on unresolved data unless the user's explicit discard policy applies.

## Effective-budget evidence correction (Oct 7)

Earlier continuation runs requested a 32 MiB hard budget and an 8 MiB drain
reserve. The drain was below the 32 MiB slab minimum and fell back to the
64 MiB default: the effective bound was 32 + 64 MiB. New sparse/mixed gates
request 32 + 32 MiB and assert the opt-in allocator trace. Recorded old bytes
and RSS remain valid; they do not establish an 8 MiB reserve.
