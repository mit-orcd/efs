# W92 — three additional Xorinox profiling rounds

Started Oct 9 EDT / Oct 10 UTC, on `devel-find-stats`. Files engine,
direct fragment I/O, TCP, all three storage VMs. No wipe or push.

Each live round creates a separate retained tree with the original
`~/git/cluster/testing/tiny_files.py`: 6000 files, 4 workers, 1..512-byte
payloads, maximum 100 files per folder and depth 3. Directory shape and bytes
are randomized by that script. All daemons are sampled at 99 Hz for 60 seconds;
creation continues to completion. Live rates are observational, because namespace
grows between rounds. Each change is also measured against the same frozen
25425-file namespace using the same supporting library and four catalog builds.
The first build is fresh; the next three refresh unchanged partitions.
The frozen harness (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261009-w92-tiny-profile/frozen_catalog_bench.c`)
and [production-code regression](../../../tests/test_query_disk_io.c) are retained.

The oracle checks every exported file path and exact directory/byte counts,
then samples payload hashes. Repeat after final restart. This is a cache and
creation workload, not a physical-disk throughput or power-loss test. Ordinary
mounts opt out of commands; only the authorized diagnostic mount opts in.

## Round 1 — exact inode matches

All-node profiles retained as node111/112/113-round1-perf.txt. Node 1 inclusive
locate share was 11.34%, pread 17.90%, pwrite 8.18%; inclusive CPU shares are
not wall time or utilization. Stop searches at exact matches in validated,
strictly increasing inode rows. Missing keys and boundaries remain checked.
The focused regression requires one cold read for a median exact hit.
Commit: `5e64d6f1`.

The same four-build trace reduces preads 1322115 → 1295903 (2.0%);
median unchanged refresh 0.950 → 0.926 s in the first sequential trial is
too small/noisy to claim a robust timing gain.

## Round 2 — combine raw record writes

The next all-node profiles show node 1 pwrite at 8.88% inclusive sampled CPU.
Combine adjacent raw key/value fields into one bounded record before writing
the index. The per-record regression asserts two writes instead of three, exact
bytes, zero-value support and no index advancement after partial ENOSPC.
Fixed builder scratch rises by 16704 bytes, accounted in build and restore
admission (2086408 → 2103112 bytes); no namespace-sized allocation.
Commit: `a04bfec6`. Frozen four-build pwrite count falls 460307 → 381549
(17.1%); pread count is unchanged from round 1. Timing is noisy and did not
show a fresh-build improvement in this single sequential comparison.

Live round 1: 78.3 s / 77 files/s, 112 populated directories.
Live round 2: 79.2 s / 76 files/s, 111 populated directories. Both retain all
6000 files. The filesystem oracle verifies all exported paths and aggregates,
with 66 payload samples per tree. These rates are not a controlled A/B.

## Round 3 — one checked raw/index barrier

Third all-node profiles on a04bfec6 continue to show cache persistence costs.
The b4212453 fix skips the redundant final fsync for new raw/index files,
which raw_build already synced using their writing descriptors. Those first
barriers remain checked before close; raw/index digests are still verified,
and name/derived files, manifest, directories and checkpoint barriers remain.
Injected EIO in each file class must refuse the candidate while the old published
checkpoint stays readable and restorable. The fault harness tracks descriptor
classes on open, avoiding a Linux-only /proc dependency.

Frozen four-build fsync calls fall 6893 → 5363 (22.2%); pread/pwrite counts
retain the prior reductions. Live round 3 on a04bfec6: 76.9 s / 78 files/s,
129 populated directories. Final b4212453 is checked independently and deployed
after that profiling pass; do not label the live rate a final-build A/B.

## Limitations and remaining paths

Each profile is inclusive CPU sampling of efsd, not physical I/O time or core
utilization. Round 2 node 1's final approximately four seconds overlap a private
build; profiles identify paths and do not establish comparative CPU speedup.
Offline strace durations likewise are not throughput. All frozen trials use
private metadata, warm filesystem cache and the same supporting source, without
hardware cache eviction; host/VM activity can vary.

store_local is only around 0.5–0.7% inclusive sampled CPU on these workloads.
No fragment durability, O_SYNC/direct-I/O or parent-directory persistence rule
is relaxed. The larger unresolved cost is repeated full derived-stat/rollup
construction. Incremental dependency-aware maintenance and aggregate cache
space/retention remain W92 work. This does not close W66 or NFS-specific W55.

Final gates: focused I/O/lookup/record/fsync failure injection; 12000-file
RAM/disk differential, retained views, rename, checkpoint corruption/refusal,
quota admission; related catalog, name-index and metadata gates. Full suite and
sanitizer acceptance are not claimed. All five architecture-document checks pass.

## Final controlled comparison

Alternating baseline/final order 0,3,3,0,0,3 supplies three fresh builds and nine
unchanged refreshes for each implementation. All 24 builds have identical
25425 files, 827 directories and 6234928 bytes. Process CPU is measured with
wait4 per trial, not cumulative child RSS; raw data in w92-repeat-results.jsonl.

| Metric | Baseline 6e841662 | Final b4212453 | Observation |
| --- | ---: | ---: | --- |
| Four-build pread calls | 1322115 | 1295903 | 2.0% fewer |
| Four-build pwrite calls | 460307 | 381549 | 17.1% fewer |
| Four-build fsync calls | 6893 | 5363 | 22.2% fewer |
| Median fresh build, 3 trials | 2.979 s | 2.986 s | essentially unchanged |
| Median unchanged refresh, 9 builds | 0.946 s | 0.984 s | no demonstrated wall-time gain |
| Median four-build user+system CPU, 3 trials | 2.289 s | 2.224 s | observed 2.8% less CPU |
| Accounted builder scratch | 2086408 B | 2103112 B | fixed +16704 B |

Fresh-build ranges overlap (baseline 2.886–3.238 s; final 2.910–3.386 s), as
do refresh ranges (0.900–1.069 s; 0.878–1.298 s). No reliable wall-time or
foreground creation improvement is demonstrated; the supported gains are fewer
cache syscalls and slightly lower measured process CPU. Per-stage earlier JSON
trials and strace summaries are retained rather than selecting only fast runs.

| Live round / profiled code | Files | Populated dirs | Descendant dirs | Bytes | Seconds | Files/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 / 6e841662 | 6000 | 112 | 204 | 1543655 | 78.3 | 77 |
| 2 / 5e64d6f1 | 6000 | 111 | 178 | 1543560 | 79.2 | 76 |
| 3 / a04bfec6 | 6000 | 129 | 216 | 1531140 | 76.9 | 78 |

The retained root after these trees has 43425 files, 1428 directories and
10853283 logical bytes; exact root totals pass the final cold gate. Directory counts
above exclude each tree's own root; populated dirs differ from all descendants.

## Deployment and cold verification

Final binaries b4212453 match on all three daemons (SHA256 in validation.json
and node111/112/113-final-runtime.log); arguments retain --direct-io and TCP.
All five tiny trees (three new 6000-file trees and the two previous 12000-file
trees) match every exported path, directory/byte totals and 328 sampled payload
hashes after full daemon restart and fresh diagnostic mount. Root totals match
43425 files / 1428 directories / 10853283 bytes with no refresh error.
Cold-oracles.log retains outputs and external model hashes. Models remain on
xefs1 under /home/efs; raw perf.data remains on each node, with integrity logs
recording all nine hashes and successful profiler exits.

A gateway stop helper initially raced an already exited client after unmount,
refusing the stop and retaining servers. Verified the process/mount were gone
and retried the orderly stop; no force-discard or wipe. Both failure and retry
logs are retained. Final services and ordinary client mounts restored; commands
remain opted out there. Diagnostic mount drained/removed and no profiler jobs
remain. Source/docs/evidence are synced alongside /tmp/efs. No push.
