# Retained partition inode keys: 800122c5e86d

Removed generation-wide open-addressed inode hash-table construction. Each
immutable inode block now retains a compact sorted array of 64-bit inode keys,
validated as strictly increasing against the encoded source keys. Lookup
routes by inode shard and binary-searches this key array, mapping the local
ordinal through that catalog's node_begin range. Shared unchanged blocks keep
their key arrays; changed blocks rebuild them along with validated rows/stats.
Extra key bytes are fully accounted; global hash allocation/probing is gone.
Lane invalidation, permissions, snapshot retention and cap semantics unchanged.

Committed/deployed build 800122c5e86d on NUC, direct I/O/TCP, .efs enabled.
Generator received one SIGINT and drained queued writes before graceful mount
and four-server shutdown; existing data preserved; deployment exit zero.
Relevant gates before deployment: differential catalog, lookup boundaries
(empty/below-first/above-last/missing), shared-block lifetime, same-source full
oracle, command/export/permissions, POSIX 212/212, peer 64/64 and restart PASS.
The full native suite passed on preceding block implementation a62cbd5f6ca2;
it was not repeated for this narrower lookup change. No claim otherwise.

Fresh workload tiny-index-800122c5: eight workers, depth eight, 1,000 files per
folder, bounded 8,000 files. Simultaneous four-daemon 30-second perf at 199 Hz:
6,879 samples, zero lost. At 41 seconds: 5,880 files, 143 files/s.

| Node | CPU, one core=100% | Builder inclusive sampled cycles |
| --- | ---: | ---: |
| n1 | 15.3% | below report threshold |
| n2 | 36.1% | 28.30% |
| n3 | 16.7% | 22.98% |
| n4 | 8.4% | below report threshold |

Nearby 79 successful warm n2 publications: median 57 ms, range 39–92 ms,
versus prior block implementation median 147 ms, range 101–319 ms. About 61%
lower median. n2 builder cycle share falls 44.59% to 28.30%; self share 40.43%
to 19.63%. The later publication has 1,218,148 files, 5,120 directories,
1,039,528,699 accounted bytes, build 73 ms. The cap is still 1 GiB.

These live windows differ in namespace, mutations and host load; the previous
window overlapped native checks. No controlled throughput or overall CPU
speedup claimed. Cycle percentages are per-PID relative shares; inclusive rows
overlap. Remaining per-generation pointer-vector scans, directory rollups,
changed-block getattr and snapshot/name/transaction work remain software work.
There is no global lookup-index construction left to profile.

After capture, live capped find and stats pass with current snapshots and no
refresh error at their sample. Complete pinned NUL export: 1,224,379 unique
paths, 89,268,556 bytes, exit zero, elapsed 22.0876 seconds. No duplicates.
This validates loaded export operation, not independent full namespace
completeness; private differential/page gates provide that oracle. Prior
export duration was not recorded precisely, so no export speedup claimed.
Large payload retained on NUC only; completion metrics saved here.

Raw callgraphs, reports, counter snapshots, instruction annotation, live
command/export metrics, publication excerpts, screen, gate logs and deployment
record retained in this directory. Source commit 800122c5e86d.
