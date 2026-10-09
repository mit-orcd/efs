# Compact immutable inode blocks: a62cbd5f6ca2

Implemented/deployed on devel-find-stats, NUC direct I/O/TCP, .efs enabled.
Inode row/stat values live in reference-counted immutable partition blocks.
Reuse requires the identical retained inode partition and identical retained
partitions for all active lane dependencies. A changed dependency rebuilds
the entire affected block conservatively using authoritative getattr. Each
catalog owns its small global pointer vector/index and fresh directory-only
rollup/link/child-count records. Regular files no longer carry empty rollups.
Shared blocks are charged in full to each catalog's accounting; no cap bypass.
Raw partitions and edge folds retain their existing dependency/lifetime rules.

Differential catalog tests compare incremental and cold builds from the same
pinned source, covering lane-only mutations, earlier-partition insertion and
removal, old snapshots, split/hash transitions and corruption. Added 512-inode
shared-block test retires the original catalog and checks every inode and root
rollup through its successor. Focused tests and full native make -j8 test pass.
Private publication/RPC, two-mount commands, export, permissions, POSIX 212/212,
peer 64/64 and restart reconstruction pass. Full native storage tests finished
after deployment from an unchanged, separate build checkout. Relevant gates
passed before deployment. Loader received one SIGINT and all queued workers
drained before graceful two-mount/four-server shutdown; no data discarded.

Old build 64cdf183d265 stopped publishing at generation 140: 1,138,552 files,
1,069,076,168 accounted bytes, then NOMEM near the unchanged 1 GiB cap. New
build publishes the larger namespace successfully: at generation 112,
1,181,069 files and 1,015,659,920 bytes; later generation 335 reaches 1,191,660
files at 1,024,471,067 bytes. Thus the cap remains enforced and publication
recovers without increasing it. This is per-catalog accounting, not process
RSS; retained exports, source caches and allocator behavior still affect RSS.

Restarted a new bounded 20,000-file loader in tiny-block-a62cbd5f: eight workers,
depth eight, 1,000 files/folder. Simultaneous 30-second 199 Hz four-daemon perf:
10,186 samples, zero lost. Screen at 42 seconds: 6,259 files, 149 files/s.

| Node | CPU, one core=100% | Builder inclusive | Page-fault entry inclusive |
| --- | ---: | ---: | ---: |
| n1 | 26.7% | below report threshold | — |
| n2 | 67.7% | 44.59% | 1.38% |
| n3 | 25.7% | 28.92% | 1.68% |
| n4 | 11.7% | below report threshold | — |

Against the earlier healthy clock-batched profile, n2 page-fault share falls
23.63% to 1.38%; builder inclusive share 65.84% to 44.59%. add_node no longer
appears above 0.5%. Nearby 79 successful warm publications have median 147 ms,
range 101–319 ms, versus previous healthy median 284 ms (244–342 ms). About
48% lower median, with a larger namespace. These live windows differ in
mutation mix, publication frequency and host load; native checks overlapped
part of this capture. No controlled throughput or aggregate CPU speedup claim.
The last prior profile was cap-failing and is not a valid healthy baseline.

Remaining builder self share is 40.43% on n2. Instruction annotation shows
random lookup-index probing/inode comparison work dominates local samples;
global hash index construction and pointer-vector/directory scans remain.
Retained block reuse eliminates global row decoding, not all O(namespace)
work. Directory records still allocate per generation. No hardware ceiling.

After capture, live default find and stats pass with current snapshots and no
recorded refresh error at their sample. Complete pinned NUL export returns
1,187,139 unique paths, 86,635,561 bytes, exit zero. Uniqueness is checked; this
live count is not an independent namespace-completeness oracle (the private
differential/page gates provide that coverage). Large payload retained only
on NUC, with its SHA-256 and completion evidence saved here. Concurrent writes
can produce transient BUSY refresh attempts; valid previous snapshots survive.

Raw perf, per-PID relative reports, annotation, counter samples, publication
excerpts, screen, live command/export results, unit/native/private logs and
deployment record retained here. Source commit a62cbd5f6ca2.
