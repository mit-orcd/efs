# W92 tiny-file/cache profiling — Oct 9, 2026

Final branch code/deployed binaries: `6e841662` on `devel-find-stats`.
Files engine, direct fragment I/O, TCP, runtime user efs on all three Xorinox VMs.
No wipe. Both generated trees remain in EFS; diagnostic storage-node mount was
removed after cold verification. Normal gateway/client mounts remain opted out.

## Workload and daemon profiles

Copied the user's [tiny_files.py](tiny_files.py) from ~/git/cluster/testing to
xefs1:/home/efs/tiny_files.py. Ran on its explicitly authorized diagnostic mount:

```sh
python3 /home/efs/tiny_files.py /home/efs/w92-tiny-mnt/w92-tiny-baseline \
  --max-files 12000 --max-files-per-folder 100 --max-depth 3 \
  --max-file-size 512 --workers 4
```

Repeated with output directory w92-tiny-after. Payloads are random base64 prefixes
of 1..512 bytes; folder counts/depths are random. No per-file fsync is requested
by this script; close/drain and cold sampled-byte verification are separate gates.
Baseline (production 47e7b886) completed in 53.4 s / 225 files/s; first optimized
build 56b845a4 completed in 141.0 s / 85 files/s. **This is not an isolated
throughput A/B:** the second run retains the first tree (larger namespace) and
contains other merged devel changes. Do not attribute the live drop or claim
improved file-creation throughput from this run. A same-revision/same-namespace
foreground creation investigation remains open under W66/W92 measurement work;
do not weaken metadata authority/persistence for speed.

All three daemons were sampled with `perf record -F 99 -g -p PID -- sleep 60`.
The producer and profiler ran on xefs1; one profile per server. Before profiles
found repeated disk-backed binary searches/pread dominating query construction.
The percentages below are inclusive shares of sampled daemon CPU, not core use
or wall time; retained namespace and foreground changes confound comparisons.

| Inclusive symbol | Node 1 before/after | Node 2 before/after | Node 3 before/after |
| --- | --- | --- | --- |
| host_query_thread | 57.64% / 42.11% | 62.32% / 63.78% | 69.54% / 64.92% |
| __libc_pread | 36.01% / 15.47% | 40.93% / 24.23% | 45.91% / 22.88% |

[Baseline](node1-baseline-perf.txt), [after](node1-after-perf.txt), and corresponding
node2/node3 reports retain full compact symbol tables. Raw perf.data files remain
in /home/admin/w92-tiny-profile on each node; final-runtime logs retain SHA256s.
The profile's remaining fsync chain included 5.17% beneath query disk construction
on node 1, motivating the second immutable-file reuse change. The live after
profile covers 56b845a4; the frozen comparison and final cold gates cover 6e841662.

## Isolated cache-only measurements

Identical copies of stopped /data1/mdraft/kv were verified by diff before opening.
[Harness](frozen_catalog_bench.c) opens each private copy, pins metadata, builds
one fresh cache and performs three unchanged refreshes. Original and final cache
implementations use the same other branch/library code. No production KV or data
is modified. Both produce 25425 files, 827 directories and 6234928 bytes on every
round (also equal to the two tiny-tree models plus the earlier clean oracle).

| Controlled metric | Before | Final | Change |
| --- | ---: | ---: | ---: |
| Fresh catalog build | 3.933 s | 3.226 s | 18.0% shorter |
| Median unchanged refresh, 3 repeats | 1.977 s | 0.955 s | 51.7% shorter; 2.07× |
| Accounted per-generation memory | 1823752 B | 2086408 B | fixed +262656 B |
| 12000-file differential harness pread calls | 11317099 | 5640565 | 50.2% fewer |
| Same harness pwrite calls | 420760 | 420760 | unchanged |

[Frozen before](frozen-before.jsonl), [final](frozen-after.jsonl), and
[resource observations](frozen-resource.jsonl) retain raw timing/counts. Trials
are sequential, before then after; there is no hardware cold-cache or endurance
claim. Resource peak is cumulative child max RSS, not a per-generation bound.
Strace compares the original backend and first read-window fix against the same
12000-file deterministic RAM/disk regression; traced duration is not throughput.
[Before syscall summary](before-12000-strace.txt), [after](after-12000-strace.txt).

## Changes and correctness

56b845a4: 32 bounded 8 KiB read windows, included in builder memory admission;
filename construction only on FD miss. Writes remain write-through; overlapping
successful writes update windows, partial failed writes invalidate them, and FD
reuse resets them. No unflushed data is retained only in read windows.
6e841662: unchanged hard-linked raw/index/name inodes reuse their already durable
digests/barriers. Changed files, new derived rows, manifest, candidate directory
and checkpoint publication retain required persistence. Restore still verifies
lengths/digests. This is not incremental derived statistics: all inode/stat rows
and directory rollups still get rebuilt on disk.

Production-code [I/O regression](../../../tests/test_query_disk_io.c) covers block
boundaries, large reads, EOF/overflow, overlapping writes, partial ENOSPC and FD
reuse. Default/12000-file RAM/disk differential, retained views, rename, restore,
watermark/corruption refusal, orphan recovery and quota refusal pass. Related
catalog/name-index/metadata tests pass. Full code suite was not rerun. Optional
ASan/UBSan linking was unavailable because the VM lacks its libasan runtime;
no sanitizer pass is claimed. [Unit output](unit-gates.log), [oracles](oracles.log).

The [independent live oracle](tiny_oracle.py) compares all streamed paths and
exact directory/byte totals with a filesystem model, plus 65 payload hashes per
tree. Both 12000-file trees pass before and after the final full daemon restart
with a fresh mount. Normal services restored; matching daemon hashes/build IDs
and final root publication are in node1/2/3-final-runtime.log. Initial BUSY/stale
checkpoint refusal was followed by successful rebuild/publication.

Remaining W92 work: incremental derived maintenance, aggregate cache-space
control, larger/fault/platform acceptance and synthetic 2^32-entry index gate.
No billion-file or full metadata-failure-envelope acceptance; no pushes.
