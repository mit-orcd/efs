# W92 — bounded incremental disk-cache updates

October 10, 2026, devel-find-stats; exclusive Xorinox, files/direct-I/O/TCP.
Implementation and three additional all-node profiling rounds are in progress.

The builder will reuse immutable derived partitions, fold attributes only when
inode inputs or active lane dependencies changed, and collect a bounded set of
changed inode IDs and affected directories. Changed dentries and every hard-link
parent invalidate immediate aggregates; both rename parents and their ancestors
are included. Directory aggregates are recomputed exactly, including timestamp
extrema after deletion, rather than subtracting counts or guessing minima/maxima.
Unaffected child subtree totals can be reused. Changed derived files are private
copies/reflinks before writing; retained snapshots and old checkpoints stay intact.
The candidate retains all existing authority checks, fsync/digest/manifest barriers
and immutable publication. No authoritative metadata is stored in this cache.

The first version used fixed 512-entry bounds; round two increases them to 1024 within the 4 MiB builder budget. Oversized change/ancestor sets
fall back to the existing full build; IO/persistence errors fail the candidate.
Scratch and copy buffers are charged conservatively; no namespace-sized RAM is
added. Startup can use existing verified checkpoints or rebuild from authority.
Raw/name partitions and bounded sequential change discovery remain; eliminating
all scanning is outside this first implementation. The algorithm must preserve
missing-fragment/publication and routing gates imported from other branches.

Focused regressions pass and compare incremental outputs with fresh full and RAM builders,
including creates, unlink, rename, hard links, directory moves, size/time changes,
removed extrema, retained views, cap fallback, injected failure and cold restore.
Performance acceptance uses a frozen namespace separately from growing-tree live
creation, three all-node 60-second profiles, and cold retained-tree oracles.
No pushes, wipes, containers, D25 or checkpoint-cadence activation are planned.

Round one found generation creation/cleanup overhead after incremental folding.
Round two coalesces derived-cache attempt starts to the existing 2000 ms freshness
interval, including busy count-trigger waves. Startup stays prompt; failures retain
backoff. JSON exposes `refresh_min_ms`; overdue reporting stays truthful and source
contention can still delay publication. This does not change consensus or authoritative
checkpoint cadence. Copy-on-write temporary bytes now count toward disk admission;
hard clone errors fail rather than silently retrying as a copy.

[Status index](README.md) · [W92](../backlog/work-items.md#w92).
