# W92 — three additional tiny-file cache rounds

**Archived Oct 11, 2026:** dated profiling and integration evidence. These rounds do not close W92 or establish the current Intel deployment. The [active W92 acceptance ledger](../../status/query-cache-closure-20261010.md) retains unresolved scale and platform gates.
Oct 9 EDT / Oct 10 UTC, `devel-find-stats`; Xorinox files/direct-I/O/TCP.
Three additional 6000-file trees use the original tiny_files.py with four workers,
1..512-byte payloads and up to three directory levels. Every storage daemon has
one 60-second CPU profile per round. Namespace is retained and grows; live
creation rates are observations, not isolated optimization speedups.

The profiles show repeated disk-catalog lookup, tiny writes and persistence
costs. Three focused changes preserve metadata authority and durability:

- `5e64d6f1`: stop exact inode lookups once the unique validated row is found.
- `a04bfec6`: combine adjacent raw key/value bytes into one bounded disk write;
  the index advances only after the complete record succeeds.
- `b4212453`: avoid the duplicate final raw/index fsync; retain the checked first
  fsync on the writing handles, final digests, name/derived-file barriers,
  manifest and directory/checkpoint publication barriers.

The record buffer adds a fixed 16704 bytes to build/restore memory admission;
accounted candidate scratch is 2103112 bytes. Writes remain write-through.
The focused production-code regression covers exact and missing inode lookups,
record bytes, empty values, short-write/ENOSPC failures and injected fsync failure
in raw/index/name/derived/manifest classes. A failed build must return no view;
the previously published checkpoint remains readable and restorable.

Controlled measurements use the same frozen 25425-file namespace and supporting
library. Four-build traces show 1322115 → 1295903 preads after exact lookup,
and 460307 → 381549 pwrites after record assembly (17.1% fewer); fsyncs fall
6893 → 5363 (22.2%). Three alternating trials per implementation show essentially
unchanged fresh build (2.979 → 2.986 s) and no reliable refresh-time improvement
(0.946 → 0.984 s, overlapping ranges). Four-build median CPU is 2.289 → 2.224 s.
Live creation stays at 76–78 files/s. Do not claim foreground or wall-time speedup.

Final deployed code is `b4212453`, matching all three daemons. After restart,
all five retained tiny trees match their full exported paths and totals, plus
328 sampled payload hashes. Root totals are 43425 files / 1428 directories /
10853283 bytes with no refresh error. Ordinary mounts remain opted out, the
diagnostic mount is drained/removed, and services are restored. No wipe or push.
Focused/related gates and all five documentation checks pass; full suite and
sanitizer acceptance are not claimed. Evidence is recorded in the
[raw ledger](../../../results/measure/20261009-w92-three-rounds/README.md).
W92 stays open: derived inode/stat/rollup maintenance still performs a full disk
pass, aggregate retained-cache capacity control and scale/fault acceptance remain
unfinished. This does not close W66 foreground consensus/routing work or W55's
original NFS incident. [Status index](../../status/README.md) · [W92](../../backlog/work-items.md#w92).
