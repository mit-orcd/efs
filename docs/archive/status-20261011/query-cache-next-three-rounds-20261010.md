# W92 — branch integration and three further tiny-file profiles

**Archived Oct 11, 2026:** dated profiling and integration evidence. These rounds do not close W92 or establish the current Intel deployment. The [active W92 acceptance ledger](../../status/query-cache-closure-20261010.md) retains unresolved scale and platform gates.
October 10, 2026 EDT, isolated `devel-find-stats`, exclusive Xorinox,
files/direct-I/O/TCP. Retained namespace preserved; no wipe or push.

## Integration boundary

Every local branch tip captured before this round is an ancestor of the merged
branch. [Boundary and ancestry proof](../../../results/measure/20261010-w92-next-three-rounds/branch-coverage.tsv)
includes fifteen branches. Only three needed new merges: `devel` at `39fb25da`
via `25745e6d`, persistence-pressure at `8ee8b69e` via `a4c0a73b`, and W66 at
`7ff7cb70` via `51880999`. This imports the durable published-ticket retirement
and idle typed-slot reuse fix; it does not activate the rejected NUC credit-order
experiment. Other worktrees and later branch progress remain independent.

The merged complete native `make test` passes, and the final `f5164bb3` source
passes the complete native suite again. Its first attempt stopped at the
documentation link gate because the private build copy omitted linked result
files; copying the actual linked evidence and rerunning passes the complete
suite. Both logs are retained. No gate is disabled or replaced with placeholders.

## Cache fixes

`c37b0519` folds dentries in canonical raw parent/name order, reuses one bounded
parent accumulator and flushes at parent changes and shard boundaries. Directory
child links still write immediately. The name-search index and disk format stay
unchanged. Partial parents spanning shards reload their previous totals. This
adds 600 accounted candidate bytes, without namespace-sized memory.
The regression checks exact file/byte/time aggregates and a short parent write
followed by ENOSPC: no failed candidate is published, and the old checkpoint
restores. Differential tests run at default and 12000-file sizes; quota refusal
and existing persistence-failure tests pass.

`f5164bb3` replaces a 32-slot descriptor hit scan with a fixed shard/file-kind
index. Eviction removes the old mapping; failed opens leave no mapping; closing
the cache clears all mappings. Misses still use bounded LRU selection. Its fixed
16 KiB cost is included in candidate/restore admission. Regressions cover eviction,
failed open, invalid shard/kind, close/reopen and read-window coherence. No fsync
or publication barrier is removed.

## Three all-node profiles

Each round records all three efsd processes for 60 seconds at 99 Hz while the
original tiny_files.py creates 6000 files, four workers, 1..512-byte payloads and
up to three folder levels. No compiler or controlled benchmark overlaps capture.
All nine captures exit 0 with zero lost samples; remote binary checksums are
recorded. The namespace grows and directory placement is random; live rates
are observations.

| Round | Profiled build | Seconds / files per second | New tree directories / bytes |
|---|---|---|---|
| 1 | `51880999` | 45.2 / 133 | 201 / 1524599 |
| 2 | `c37b0519` | 45.1 / 133 | 214 / 1523795 |
| 3 | `f5164bb3` | 45.8 / 131 | 192 / 1536110 |

Each new tree passes independent full exported path sets and totals, plus
66 sampled payload hashes. On xefs1 disk_io self samples are 2.99%, 2.66%,
1.96%; query-thread inclusive samples remain roughly 43–46%. These growing-tree
profiles identify remaining work but cannot isolate a live speedup. Derived
full passes and durable writes/barriers remain significant. There is no third
speculative code change: two focused fixes are measured through subsequent live profiles
and the controlled comparisons below.

## Controlled storage work

All variants use the same frozen 25425-file, 827-directory, 6234928-byte private
KV copy, the same merged supporting library and fresh private output directories.
Each trace covers one fresh build and three unchanged refreshes.

| Variant | pread64 | pwrite64 | fsync | Accounted candidate bytes |
|---|---:|---:|---:|---:|
| Merged baseline | 285379 | 291549 | 5363 | 2897032 |
| Coalesced parent rollups | 173923 | 189853 | 5363 | 2897632 |
| Descriptor index | 173923 | 189853 | 5363 | 2914016 |

Preads fall 39.1% and pwrites 34.9% relative to this round's merged baseline;
fsync counts stay identical. Descriptor indexing targets CPU rather than syscall
counts. Accounted scratch stays within the tested 4 MiB budget. Traced timings
are not speed measurements. Filesystem caches are warm on shared VMs.

Three alternating untraced trials per variant retain every observation:

| Metric | Baseline median (range) | Parent folding median (range) | Final median (range) |
|---|---:|---:|---:|
| Fresh build seconds | 2.803 (2.709–2.942) | 2.633 (2.573–2.704) | 2.716 (2.563–2.754) |
| Unchanged refresh seconds, nine observations | 0.784 (0.716–0.815) | 0.751 (0.686–0.876) | 0.724 (0.672–1.098) |
| Four-build process CPU seconds | 1.578 (1.558–1.637) | 1.388 (1.377–1.420) | 1.342 (1.319–1.406) |
| Four-build process wall seconds | 5.377 (5.317–5.548) | 5.174 (5.125–5.210) | 5.135 (4.993–5.523) |
| Process peak RSS KiB | 15944 (15892–15956) | 15820 (15800–15896) | 15948 (15852–15968) |

Median process CPU falls 15.0%; the descriptor index adds a modest 3.3% reduction
relative to parent folding alone. Median refresh improves 7.7%, but ranges overlap
and a slower final refresh is retained. Fresh wall improvement is modest; no
uniform latency gain, physical-media limit or foreground creation-rate gain is claimed.

## Final functional gates and deployment

Final source passes the complete native suite, core POSIX 212/212 with equivalent
local baseline, two-client concurrent POSIX 64/64 and extras 4 pass / 0 fail /
1 unsupported shared writable mmap skip. Persistence passes 26/26 prepare and 26/26 verify across
full daemon restart and fresh FUSE mounts. These are distinct from the sampled
performance runs. The source fixes are deployed as `f5164bb3` on all three nodes,
with human version/build IDs, direct-I/O arguments and TCP environment recorded.
The final restart encounters a stale SSH-multiplexer binary-copy wait after clean
shutdown. Task-owned copy sessions are cancelled, identical installed binaries
are verified, cluster mux sockets are closed and cold startup resumes. Original
and recovery logs remain; no retained namespace is deleted.

All eleven retained tiny trees pass cold full exported path sets and totals:
78000 files and 724 sampled payload hashes. Root snapshot reports 80756 files,
2927 directories and 344207151 logical bytes, including retained suite fixtures;
complete=true, refresh_error=0 and refresh_overdue=false. The diagnostic mount
is drained/removed; gateway and both ordinary clients remain mounted with `.efs`
disabled. Matching runtime source hashes and binary checksums are recorded after
source synchronization. No retained EFS data is deleted.

## Remaining work

W92 stays open for full derived-stat passes, aggregate retained-cache capacity
control, incremental maintenance and billion-file scale/fault acceptance.
W66 authoritative consensus/routing and W99 proactive capacity protection remain
separate. Native FUSE evidence does not close the original NFS-specific W55 gate.
No sanitizer, hardware power-loss or cold physical-media acceptance is claimed.

[Raw ledger](../../../results/measure/20261010-w92-next-three-rounds/README.md) ·
[Previous checkpoint](query-cache-merged-three-rounds-20261009.md) ·
[W92](../../backlog/work-items.md#w92) · [Status index](../../status/README.md).
