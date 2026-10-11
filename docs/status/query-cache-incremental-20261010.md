# W92 — incremental disk-cache updates and three Xorinox profiles

October 10, 2026, `devel-find-stats`, exclusive Xorinox VMs, files/direct-I/O/TCP.
Final code deployed as `d83f3f06`; native/live/cold verification passes.

## Integration and implementation

All 15 local branch tips captured at task start are included, with new merges from
`devel`, W66 and persistence-pressure. Other agents' checkouts are untouched.
[Captured branch boundary and ancestor proof](../../results/measure/20261010-w92-incremental)
record the integration boundary; later commits elsewhere are not implicitly included.

`10febf8b` reuses immutable derived partitions and folds attributes when inode
inputs or active lane dependencies change. A bounded changed-inode/dirty-directory
set includes every live hard-link parent, both sides of changed dentries, and
ancestors. Directory aggregates are recomputed exactly, including timestamp extrema
after deletion; clean child subtree totals are reused. Revised inode partitions
conservatively re-fold all rows because PVER/DSEQ/lane inputs may change without
changing an inode row. Discovery still scans the namespace sequentially.

Before writing a shared derived inode, copy-on-write creates a private reflink or
checked copy. Old views remain immutable; native manifest version 2 is unchanged.
`f3386fe5` charges temporary copy bytes to admission and propagates hard clone IO
errors. The completed manifest/digest/fsync barriers and authoritative source-cut
checks are retained. IO failures reject a candidate rather than hiding them behind
fallback. This cache remains derived from authoritative metadata.

`ba89a878` increases fixed sets from 512 to 1024 entries within the 4 MiB builder
budget. Oversized changes fall back to a full build. It also coalesces attempt starts
to the existing 2000 ms freshness interval, including busy count-trigger waves.
Startup remains prompt; failure backoff and truthful overdue reporting remain.
JSON exposes `refresh_min_ms`. Source contention can still delay publication;
this is not a hard two-second completion guarantee or a consensus/checkpoint change.

`d83f3f06` scans current aliases once: changed old dentries already invalidate
removed/renamed parents, so rescanning all old aliases adds no dependency. Fresh
builds skip 4096 probes for derived files that have not yet been created.

## Live profiles

Each round uses 6000 new files, four producer workers, depth three, up to 100 files
per populated directory, random payloads of 1–512 bytes. Three node daemons are
profiled concurrently for 60 seconds at 99 Hz. Compilation/frozen benchmarks do
not overlap these captures. Each round passes a recursive oracle: all 6000 paths,
sizes and byte totals plus 66 deterministic payload samples. All nine captures
have zero lost samples; remote raw perf files remain available with integrity hashes.

| Round | Code | Creation seconds | Files/s | Cache worker inclusive samples, nodes 1 / 2 / 3 |
|---|---|---:|---:|---|
| 1 | incremental, 512 bound | 48.3 | 124 | 63.07% / 77.46% / 77.89% |
| 2 | coalesced, 1024 bound, COW admission | 36.3 | 165 | 26.56% / 41.33% / 42.83% |
| 3 | single live-alias scan, fresh-build probe removal | 37.8 | 159 | 27.25% / 41.44% / 41.05% |

Round one published roughly twice per second, with 230–360 ms builds. Faster
incremental work exposed generation link/unlink overhead. Coalescing reduces
sampled cache cost on all nodes. Round three is within run variation of round two;
there is no demonstrated additional live throughput gain. These are sequential
growing namespaces, not randomized equal-state A/B tests. Inclusive percentages
are sampled shares, not CPU utilization, and cannot be added across nested frames.

## Fixed-namespace comparison

The stopped private KV fixture has 25425 files, 827 directories and 6234928 logical
bytes. Exact builder variants are compiled against the same merged supporting
library, with fresh cache directories. Twelve alternating process trials give
three fresh builds and nine unchanged refreshes per variant. All counts match.
No compiles or live perf captures overlap timing runs.

| Variant | Fresh median, s | Unchanged refresh median, ms | Refresh range, ms | Process CPU median, s | Accounted builder MiB |
|---|---:|---:|---|---:|---:|
| 0 | 2.562 | 719.69 | 659.95–765.95 | 1.319 | 2.779 |
| 1 | 2.817 | 43.37 | 41.27–51.84 | 0.854 | 3.174 |
| 2 | 2.633 | 43.52 | 42.31–49.91 | 0.841 | 3.471 |
| 3 | 2.691 | 41.72 | 41.08–165.48 | 0.910 | 3.471 |

Variant 0 is the pre-incremental builder; variants 1–3 match the live round builder code. Daemon scheduling is outside
this fixed builder fixture.
The final median unchanged refresh is about 17 times faster than baseline and folds
zero attributes. Fresh builds do not improve materially. Process CPU/RSS include
KV open/close, one fresh build and three refreshes; RSS ranges are retained in the
JSON summary and are not the builder memory limit. The final accounted builder is
3639656 bytes, independent of namespace size. The 165 ms refresh outlier remains
in the reported range. These unchanged-refresh timings do not quantify arbitrary
mutation batches or eliminate sequential discovery cost.

Whole-process strace includes fresh construction and KV work: baseline 498909
syscalls, initial incremental 353192, final 349096. Final fresh-build probe removal
accounts for exactly 4096 fewer failing unlink calls. Raw totals include 166180
writes, 91960 reads and 3068 sync calls in the final process; they are not cache-only
refresh counts. Existing durability barriers remain. Trace runtimes are not used
as benchmark timings.

## Correctness and remaining acceptance

Focused native gates pass: independent fresh RAM-vs-incremental comparison of every
inode/stat and every directory rollup/link; lane-only cross-shard changes; hashed
and tombstone dentries; earlier-row insertion/removal; extrema deletion; cross-parent
rename; surviving/removed/final hard-link aliases; rmdir; 1100-change full fallback;
retained snapshots; 12000-node disk oracle; 1 MiB budget behavior; cancellation;
partial ENOSPC, fsync refusal, clone failures and old-checkpoint preservation.
The complete native suite passes on the final merged source. Live core passes
212/212, two-client passes 64/64, and extras passes 4/5 with one explicit
`mmap_write_read` skip (`ENODEV`, shared writable mmap unsupported on this mount).
Persistence passes 26/26 prepare and 26/26 verify after full daemon/client restart.
All 14 retained trees pass: 96000 files and 922 sampled payload hashes. Post-restart
root stats reports `refresh_error=0`, no pending changes and `refresh_min_ms=2000`.
Normal gateway/client mounts have `.efs` disabled after diagnostic cleanup.

Early fixture accounting assertions and a fresh-build fallback initialization bug
were caught by focused tests and corrected before deployment. A generated-doc
mismatch was regenerated; original failed logs and passing retries are retained.

W92 remains open for eliminating full discovery scans, aggregate retained-cache
capacity control and billion-file scale/fault acceptance. Cold restart is a process
restart test, not a hardware power-loss claim. No pushes, wipes, container changes,
D25 or authoritative checkpoint-cadence activation were performed.

[Raw logs, drivers, exact variants and summaries](../../results/measure/20261010-w92-incremental)
· [Status index](README.md) · [W92](../backlog/work-items.md#w92).
