# W90: sustained scaling and shared 1 GiB ranges

Benchmark-only follow-up on `devel-bench`, xorinox_efs, October 8, 2026. All scratch and source snapshots are under `/data1/efs/bench/w90-20261008`. Production EFS services and global filesystem/device settings were not changed. Nothing was pushed.

## Recommendation

Keep immutable files as the production baseline. Improve and measure file-body flush scheduling before choosing a container backend. Hole-punch containers remain an experimental candidate, with ownership, reader pins, durable mapping publication and recovery gates required before integration. Do not enable filestreams globally based on these samples.

## Workload and limits

XFS on HP SSD FX900 Pro 4 TB (`/dev/nvme1n1`), kernel `6.12.0-211.61.1.el10_2.x86_64`, 4 KiB blocks, 16 allocation groups, `noatime,logbufs=8,logbsize=256k`. No global cache drops or mount/device changes. Host activity is not isolated. These are independent local-storage tests, not network/RDMA, GPU, cluster throughput or production durability tests.

Churn: 64 KiB payload plus 4 KiB checksum tail, 50/50 reads/replacements, direct I/O, batches of 32, 120-second measured intervals per worker. Logical containers are 1 GiB. Each worker has 2 GiB data-slot capacity plus 1 GiB spare; occupancy is experiment-local, not disk fullness. Capacity scales with worker count: QD8 has 16 GiB data capacity and 8 GiB spare. This is not fixed-total-working-set scaling or endurance testing. Setup/warmup and final verification are outside timing; required final batch barriers are timed.

## Matched final-build churn comparisons

All round-two cases use the final candidate executable and sources. Files defer body fsyncs to each batch boundary, fsync every still-live pending body, then fsync the directory. Containers flush dirty shared bodies and the directory. The index remains volatile, so neither policy establishes crash-safe storage-engine completion.

| Suite | Workers | Layout | File flush | Ops/s | Read p99 ms | Write p99 ms | Write max ms | Allocated GiB | FIEMAP extents |
|---|---:|---|---|---:|---:|---:|---:|---:|---:|
| sustained-80-qd4-files | 4 | files | deferred | 10983 | 0.317 | 6.015 | 870.7 | 6.400 | 98688 |
| sustained-80-qd4-filestreams | 4 | punch + filestreams | — | 10623 | 0.303 | 2.543 | 771.0 | 6.402 | 98660 |
| sustained-80-qd4-punch | 4 | punch | — | 14197 | 0.301 | 2.655 | 986.7 | 6.402 | 86721 |
| sustained-95-qd1-files-r2 | 1 | files | deferred | 2408 | 0.249 | 3.359 | 740.9 | 1.900 | 29298 |
| sustained-95-qd1-filestreams-r2 | 1 | punch + filestreams | — | 1745 | 0.250 | 1.607 | 966.0 | 1.901 | 29265 |
| sustained-95-qd1-punch-r2 | 1 | punch | — | 2168 | 0.269 | 1.607 | 949.5 | 1.901 | 28145 |
| sustained-95-qd4-files-r2 | 4 | files | deferred | 2777 | 0.419 | 45.311 | 774.9 | 7.600 | 117192 |
| sustained-95-qd4-filestreams-r2 | 4 | punch + filestreams | — | 10751 | 0.301 | 2.463 | 789.0 | 7.602 | 117182 |
| sustained-95-qd4-punch-r2 | 4 | punch | — | 13037 | 0.301 | 2.479 | 937.9 | 7.602 | 111616 |
| sustained-95-qd8-files-r2 | 8 | files | deferred | 15562 | 0.353 | 8.447 | 412.5 | 15.200 | 234384 |
| sustained-95-qd8-filestreams-r2 | 8 | punch + filestreams | — | 31363 | 0.353 | 2.863 | 361.8 | 15.205 | 234257 |
| sustained-95-qd8-punch-r2 | 8 | punch | — | 31061 | 0.353 | 2.911 | 198.2 | 15.204 | 221472 |
| sustained-batched-qd1-r3 | 1 | files | deferred | 2511 | 0.249 | 3.247 | 587.7 | 1.900 | 29298 |
| sustained-batched-qd4-r3 | 4 | files | deferred | 10896 | 0.309 | 5.983 | 546.4 | 7.600 | 117192 |
| sustained-batched-qd8-r3 | 8 | files | deferred | 16231 | 0.353 | 8.383 | 403.4 | 15.200 | 234384 |

FIEMAP counts physical filesystem mappings after flushing, outside timing; they do not measure NAND placement. A logical 1 GiB file is not necessarily one contiguous extent. `st_blocks` excludes directory/journal cost. No power-loss/crash/recovery gate or NAND write-amplification claim is made.

The final deferred-file repeats observed 2,408–2,511 ops/s at QD1, 2,777–10,896 at QD4, and 15,562–16,231 at QD8. The QD4 variation is substantial. Punch-container repeats observed 2,168–2,435, 11,697–13,037 and 31,010–31,061 respectively. Containers remain promising under concurrency, but a stable universal speedup is not established. At 80%/QD4 the gap narrowed to 10,983 files versus 14,197 punch ops/s. Filestreams did not yield a consistent win across these 64 KiB worker counts.

## Earlier immediate-file-flush reference

These first-round files fsync each replacement body immediately. Setup was later optimized to flush the initial live set together; measured policies were unchanged until deferred file batching was added. Do not treat this older file baseline as the optimized comparison above.

| Suite | Workers | Layout | File flush | Ops/s | Read p99 ms | Write p99 ms | Write max ms | Allocated GiB | FIEMAP extents |
|---|---:|---|---|---:|---:|---:|---:|---:|---:|
| sustained-95-qd1-files-r1 | 1 | files | immediate | 695 | 0.277 | 1.775 | 581.0 | 1.900 | 29298 |
| sustained-95-qd1-filestreams-r1 | 1 | punch + filestreams | — | 1898 | 0.248 | 1.599 | 787.5 | 1.901 | 29282 |
| sustained-95-qd1-punch-r1 | 1 | punch | — | 2435 | 0.242 | 1.591 | 987.2 | 1.901 | 28018 |
| sustained-95-qd4-files-r1 | 4 | files | immediate | 304 | 0.333 | 432.127 | 1000.2 | 7.600 | 117192 |
| sustained-95-qd4-filestreams-r1 | 4 | punch + filestreams | — | 10608 | 0.299 | 2.463 | 906.5 | 7.602 | 117176 |
| sustained-95-qd4-punch-r1 | 4 | punch | — | 11697 | 0.301 | 2.463 | 927.3 | 7.602 | 111922 |
| sustained-95-qd8-files-r1 | 8 | files | immediate | 329 | 0.351 | 561.151 | 995.4 | 15.200 | 234384 |
| sustained-95-qd8-filestreams-r1 | 8 | punch + filestreams | — | 31360 | 0.353 | 2.879 | 223.9 | 15.205 | 234095 |
| sustained-95-qd8-punch-r1 | 8 | punch | — | 31010 | 0.355 | 2.911 | 202.8 | 15.204 | 220172 |

## Same-range observations

Every range case uses one prepopulated logical 1 GiB file, 64 KiB payloads and 4 KiB checksum tails. Direct and buffered cases are separate. Buffered reads can be cache hits. Disjoint lanes verify exact expected versions; hot reads share immutable record zero; hot locked updates protect I/O, version/digest changes and required flushes with a per-record pthread read/write lock. This lock is process-local. Shared-file fsync can couple disjoint workers.

| Suite | Access | Workers | I/O | Persist | Status | Ops/s | Cycle p99 ms | Lock p99 ms | Read I/O max ms | Sync max ms | Mixed observations |
|---|---|---:|---|---|---|---:|---:|---:|---:|---:|---:|
| ranges-buffered | disjoint | 8 | buffered | batch | PASS | 71634.6 | 2.863 | 0.000 | — | — | 0 |
| ranges-buffered | hot-read | 8 | buffered | batch | PASS | 736888.3 | 0.013 | 0.000 | — | — | 0 |
| ranges-buffered | hot-locked | 8 | buffered | batch | PASS | 11202.4 | 6.687 | 6.463 | — | — | 0 |
| ranges-buffered | hot-unlocked | 8 | buffered | batch | OBSERVATION | 131657.7 | 0.483 | 0.000 | — | — | 0 |
| ranges-confirm-each | hot-locked | 1 | direct | each | PASS | 3291.5 | 0.739 | 0.000 | 0.286 | 10.778 | 0 |
| ranges-confirm-each | hot-locked | 1 | direct | each | PASS | 3326.0 | 0.735 | 0.000 | 0.298 | 1.039 | 0 |
| ranges-confirm-none | hot-locked | 1 | direct | none | PASS | 30.3 | 462.847 | 0.001 | 663.282 | 0.000 | 0 |
| ranges-confirm-none | hot-locked | 1 | direct | none | PASS | 24.6 | 463.690 | 0.001 | 463.637 | 0.000 | 0 |
| ranges-direct | disjoint | 1 | direct | batch | PASS | 6473.6 | 0.501 | 0.000 | — | — | 0 |
| ranges-direct | hot-read | 1 | direct | batch | PASS | 8513.1 | 0.155 | 0.000 | — | — | 0 |
| ranges-direct | hot-locked | 1 | direct | batch | PASS | 25.7 | 448.511 | 0.001 | — | — | 0 |
| ranges-direct | hot-unlocked | 1 | direct | batch | OBSERVATION | 22.1 | 399.359 | 0.000 | — | — | 0 |
| ranges-direct | disjoint | 4 | direct | batch | PASS | 23581.7 | 0.543 | 0.000 | — | — | 0 |
| ranges-direct | hot-read | 4 | direct | batch | PASS | 17981.8 | 0.241 | 0.000 | — | — | 0 |
| ranges-direct | hot-locked | 4 | direct | batch | PASS | 15.6 | 1605.631 | 1605.631 | — | — | 0 |
| ranges-direct | hot-unlocked | 4 | direct | batch | OBSERVATION | 50.6 | 561.151 | 0.000 | — | — | 0 |
| ranges-direct | disjoint | 8 | direct | batch | PASS | 45086.7 | 0.643 | 0.000 | — | — | 0 |
| ranges-direct | hot-read | 8 | direct | batch | PASS | 20213.3 | 0.485 | 0.000 | — | — | 0 |
| ranges-direct | hot-locked | 8 | direct | batch | PASS | 15.9 | 4112.383 | 4112.383 | — | — | 0 |
| ranges-direct | hot-unlocked | 8 | direct | batch | OBSERVATION | 87.5 | 557.055 | 0.000 | — | — | 0 |
| ranges-punch | hot-locked | 4 | direct | batch | PASS | 20.6 | 1163.263 | 1163.263 | — | — | 0 |
| ranges-punch | hot-locked | 4 | buffered | batch | PASS | 8029.9 | 4.031 | 3.711 | — | — | 0 |
| ranges-punch | hot-unlocked | 4 | direct | batch | OBSERVATION | 43.9 | 401.407 | 0.000 | — | — | 57 |
| ranges-punch | hot-unlocked | 4 | buffered | batch | OBSERVATION | 24552.3 | 0.747 | 0.000 | — | — | 20762 |
| ranges-race-v2 | hot-unlocked | 8 | direct | none | OBSERVATION | 62.2 | 599.874 | 0.000 | 599.831 | 0.000 | 0 |
| ranges-race-v2 | hot-unlocked | 8 | buffered | none | OBSERVATION | 151794.4 | 0.101 | 0.000 | 2.463 | 0.000 | 0 |
| ranges-split | hot-locked | 4 | direct | batch | PASS | 23.7 | 1003.519 | 1003.519 | — | — | 0 |
| ranges-split | hot-locked | 4 | buffered | batch | PASS | 6515.3 | 4.863 | 4.543 | — | — | 0 |
| ranges-split | hot-unlocked | 4 | direct | batch | OBSERVATION | 49.7 | 481.892 | 0.000 | — | — | 9 |
| ranges-split | hot-unlocked | 4 | buffered | batch | OBSERVATION | 36610.7 | 0.599 | 0.000 | — | — | 73951 |
| ranges-sync-batch | disjoint | 1 | direct | batch | PASS | 5842.4 | 0.497 | 0.000 | 524.286 | 0.661 | 0 |
| ranges-sync-batch | hot-locked | 1 | direct | batch | PASS | 23.8 | 258.047 | 0.001 | 563.510 | 0.454 | 0 |
| ranges-sync-batch | disjoint | 8 | direct | batch | PASS | 44894.6 | 0.651 | 0.000 | 10.844 | 15.758 | 0 |
| ranges-sync-batch | hot-locked | 8 | direct | batch | PASS | 15.2 | 4930.550 | 4929.987 | 664.514 | 0.486 | 0 |
| ranges-sync-each | disjoint | 1 | direct | each | PASS | 3076.1 | 0.763 | 0.000 | 0.277 | 10.404 | 0 |
| ranges-sync-each | hot-locked | 1 | direct | each | PASS | 3191.7 | 0.731 | 0.000 | 0.591 | 11.272 | 0 |
| ranges-sync-each | disjoint | 8 | direct | each | PASS | 18399.1 | 1.135 | 0.000 | 10.412 | 15.014 | 0 |
| ranges-sync-each | hot-locked | 8 | direct | each | PASS | 2696.3 | 26.367 | 25.727 | 0.679 | 11.013 | 0 |
| ranges-sync-none | disjoint | 1 | direct | none | PASS | 5970.1 | 0.261 | 0.000 | 426.798 | 0.000 | 0 |
| ranges-sync-none | hot-locked | 1 | direct | none | PASS | 14.7 | 495.504 | 0.001 | 495.467 | 0.000 | 0 |
| ranges-sync-none | disjoint | 8 | direct | none | PASS | 49327.4 | 0.351 | 0.000 | 11.351 | 0.000 | 0 |
| ranges-sync-none | hot-locked | 8 | direct | none | PASS | 19.6 | 4339.604 | 4339.149 | 363.089 | 0.000 | 0 |

Ordinary unsplit unlocked samples observed zero mixed records. This is not an atomicity violation report or a safety guarantee. Deliberately splitting an update into two writes, or punching before rewriting, exposed mixed/incomplete reads without coordination. Locked versions passed. The diagnostic checks self-consistency only; a complete stale version or lost logical update is not detected. Unsafe cases are OBSERVATION, never storage-protocol PASS.

A surprising direct hot-range penalty was isolated to read I/O in short mixed read/write samples: none/batch could stall a read for roughly half a second, while per-write fsync dramatically improved progress. The repeated controls are listed above. This suggests a read-after-write/lower-layer interaction, not a proven XFS or SSD root cause. Timed write and flush maxima were much smaller in those none/batch samples. Do not generalize this into a global persistence-policy change. A sampled XFS journal wait from an older churn run is retained separately and does not diagnose the hot-range delay.

## Ownership recommendation

Concurrent immutable readers can share a range. Disjoint records should not need a whole-container application lock. For a changing object, write an unpublished new location, flush its body, conditionally publish its mapping against the expected version, and pin resolved locations through reader I/O completion. Punch/reuse only after authoritative retirement and all relevant references/readers have drained; use allocation generations to reject stale actors. Checksums detect some corruption; they are not ownership, freshness or retirement authority.

The [candidate recovery sketch](../../../docs/backlog/storage-engine-v2.md#candidate-range-ownership-and-index-recovery-plan) is deliberately a proposal. The benchmark has no durable index, replay ledger, distributed locking, recovery, or production publication protocol.

## Reproduction and evidence

Use `./efs-bench.sh --storage-churn` for timed scaling, and `./efs-bench.sh --storage-ranges` for shared-range cases. See the [churn guide](../../../docs/how-it-works/storage-churn.md) and [range guide](../../../docs/how-it-works/storage-ranges.md). Every suite preserves commands, raw stdout/stderr, results, mount facts, and binary/source hashes. Executables remain on xorinox and are omitted from this Git evidence copy. Source archives preserve each measured epoch; do not reconstruct earlier results from the final source alone.

Final targeted validation: 44 unittest methods plus benchmark CLI gate PASS; benchmark and daemon builds completed without compiler warnings. Scratch is empty after all cases. See `validation/acceptance.txt` and raw logs.

Range evidence: {'PASS': 32, 'OBSERVATION': 10}. Churn cases: 24. Validation logs are retained under `validation/`.
