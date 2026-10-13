# W92/W109: bounded LOCAL recursive-subtree continuation

**Implemented and AMD-qualified on `e77f60305bb4dcab344ba3ed1f95e93943dc4d5a`.**
Global W91/W92/W109 acceptance remains OPEN for the final integrated source at
ORCD scale. No deadline, visit/output budget, helper count, cache ABI or storage
format change is part of this fix.

## Problem and correction

Exact 9b2fac55 used an immutable parent-key range for immediate LOCAL listings or
recursive LOCAL leaf queries. Once a requested root had a child directory,
recursive paging visited the global name index across 4096 partitions, checking
scope after each candidate. A tiny nested tree could need many empty continuation
pages solely because unrelated names exist. This was source-confirmed after
ORCD reported 30-second commands/options timeouts; the ORCD trace itself was not
reproduced on AMD.

The fix discovers the requested subtree's immutable LOCAL directory ranges and
merges their next matching entries by their **original global name-index
ordinals**. Thus ordering, cursor representation/bounds, monotonic resume and
replay semantics stay compatible with the old global path. It reads no live
namespace/layout state and keeps canonical placement and directory
parent/generation checks, tombstone filtering, hardlink aliases, normal path
construction and the existing output/authorization callback.

Discovery and enumeration spend the same request candidate budget. Discovery
may consume at most one third of it. A failed bounded discovery falls back with
only the unspent budget; it emits no partial output before fallback. Cancellation
and authorization BUSY propagate as errors, including after some output.
Output-cap AGAIN preserves the next un-emitted global ordinal. Completion
requires both rollup-proven directory/entry coverage and exhausted ranges.

This is a conservative bounded path: root and discovered descendants must be
LOCAL; at most 63 descendant directories plus the root fit the fixed 64 frames;
canonical files+directories must fit one quarter of the request visit budget.
Larger trees, non-LOCAL descendants, unexpected discovery coverage or dense
retained raw records keep the prior global path. Their scale completion is
**not closed** by this change. No new retained session state or namespace-sized
query allocation is created.

## Resource and focused proof

The query-local state is 28,208 fixed bytes; a compile-time assertion bounds it
to 64 KiB. This is a reader-state bound, not a claim that builder sort admission
accounts for concurrent readers. An AMD gcc -O3/-std=c99/-D_GNU_SOURCE
-fstack-usage build measured subtree_page 46,528 bytes (dynamic,bounded),
subtree_next 17,808 bytes (static), and efs_query_disk_page 5,184 bytes
(dynamic,bounded); callee frames are additional. Production reader/helper
admission and thread stack configuration were unchanged. Eight concurrent
retained-generation readers, 16 requests each, match the independent oracle.

A real KV fixture has 20,004 unrelated names and a LOCAL subtree with 13 file
bindings (including a hardlink), two descendant directories (one empty), and
an unlink. Equal names outside the subtree surround name searches. At the
unchanged 512-visit request budget, every EXACT/PREFIX/SUFFIX/CONTAINS full page
matches the RAM page oracle's exact count and order/hash. A three-entry output
cap completes in at most five pages; each page replay gives the same cursor and
content, with strictly advancing cursor and no missing/duplicate entries.
Tiny budgets 1/16/31/46/61 can use the global path then resume with 512 without
changing the cursor interpretation or final result. Cancellation during
discovery emits nothing; denial after three entries still returns BUSY.
A later rename/build cannot alter the retained generation's page, and a
checkpoint/cold restore resolves the new name correctly.

The negative control disables only the new bounded-path guard. It fails the
same 512-visit completeness oracle and is still incomplete after eight pages.
This reproduces unrelated-catalogue dependence rather than comparing changed
timeouts. The existing sort-scratch measured-peak negative control also passes.

## Exact-source AMD qualification

AMD-395; four localhost `efs` daemons 17432–17435, two native FUSE mounts;
files/TCP/direct, `.efs` client/server enabled, policy version 0/mask 3;
packing/containers/expiry/repair experiment disabled. Data quota 17 GiB/node,
query caps 16/32 GiB unchanged and no live cap overrides. Installed binaries are
the frozen source, not an uncommitted shared-checkout mixture. The later
reported ORCD helper retry-window fix is **not included** in these results.

| Gate | Verdict on e77f6030 |
| --- | --- |
| Full normal native and clean fault rebuild | PASS, both full make test exits 0 |
| Six complete-library ASan/UBSan/leak query harnesses | PASS: disk, catalog, delta dirs, disk_io, policy, refresh |
| Same-budget 64-directory/3,000-file semantic oracle | PASS at 4/16 MiB |
| Measured sort accounting and recursive negative controls | PASS |
| Genuine baseline 19b14609 v5 transition, immediate retirement, next refresh | PASS both publishers: 29,902 rows, 37,128,092→28,994,748 physical bytes; old dirs gone before restart; next 29,904-row refresh PASS |
| Retained reader and orderly nodes 1/2 quorum loss/restoration | PASS: 12 v1–v4 BUSY/no-text replies, 807 bytes retained unchanged; old token STALE; complete 296-path/1,188,864-byte dual-mount payload check |
| Mounted commands, options, pages and permission checks | PASS, all exit0, with prior unrelated load trees retained |
| Three quiet 8-writer/2-reader load rounds | PASS: 31,392/31,392 exact 256-byte peer payloads; both readers span 7 generations/round; only expected BUSY/no-text allowed |

| Round | Verified files | Count+bytes convergence(s) | Writer p99(ms) | Reader p99(ms), both readers |
| --- | ---: | ---: | ---: | --- |
| 1 | 10,862 | 1.503 | 20.90 | 138.13 /136.15 |
| 2 | 10,768 | 0.622 | 22.14 | 135.42 /138.29 |
| 3 | 9,762 | 0.616 | 24.76 | 131.66 /128.91 |

These are correctness/functional short runs, not performance tuning or ORCD
large-catalogue acceptance. Existing 14-second load admission, 12/30-second
command/future deadlines and 30-second command/options budgets stay unchanged.
The v5→v6 physical reduction is from the earlier compact inode representation;
this query fix does not compress cache files. AMD ran e77f6030 at this checkpoint; it now runs the
[qualified f7 helper integration](amd-package2-helper-integration-20261013.md).

All 859 Makefile/src/include/deps/scripts/tests inputs match across full native,
complete-library sanitizer and live candidate source. Sorted JSON manifest
SHA256 8b077a6b8d4f7f4d93b9dbcd62d96c2920a132478937addc825bbf550efef32d.
Build stamp 2026-10-13T19:44:21Z on amd-395, branch devel-amd-package2.
Binary SHA256:

- efsd: 86e56ea37ec6831e90604d987cc628597d9b81dd80e8317daec535328ea5810f
- efs-fuse: 7f9905918e285a6425ee92f63bae4aafcc22de68546f9cde5c1c13d12f011346
- efs-mgmt: a4e20aff38711bda335d04f98adc5db0ec61b0ced8d82236c1df43ad07ddbc65

## Reproduction and remaining gate

From exact e77f6030 on Linux with libibverbs and a normal full libefs.a build:

```sh
make -j8 tests/test_query_disk_io tests/test_query_disk
./tests/test_query_disk_io
python3 tests/test_query_subtree_negative.py
python3 tests/test_query_sort_peak_negative.py
EFS_QUERY_TEST_BRANCHES=64 EFS_QUERY_DISK_MAX_MIB=4 EFS_QUERY_DISK_TOTAL_MIB=16 ./tests/test_query_disk
```

Use the full clean normal/fault, separate complete-library sanitizer, exact-ID
binary build and exclusive mounted commands from the
[AMD qualification reproduction](amd-package2-transition-load-20261013.md#final-checked-sort-qualification--2026-10-13), substituting e77f6030 and fresh external
output/fixture names. Baseline remains genuine 19b14609. Retain the deterministic
lifecycle fixture until both publishers, immediate retirement, second refresh
and dual-mount payload checks finish; controllers restore service after fault
phases. Clean only owned synthetic fixtures afterwards. No further cluster
wipe was required after the documented 9b clean-slate run.

Raw logs/JSON/stack report stay outside Git on AMD under /data1/efs/package2-e77-*
and package2-subtree-*. Every normal/fault/san/transition/quorum/commands/options/
pages/load-1/2/3/mounted exit marker is 0. Evidence SHA256:

- normal.log: b80952c63e41e51f16fed5a855c07eac4c79e502c363144f999801ccbf7400b7
- fault.log: 6d231fe441aa1c7c36b4b7af27de44b37b2b3c0a9569e444812087837ba4f521
- san.log: a206f61d72fe52d40d6a1bcb0e7a7fdb73f6e86385193f4e7ff8cc2396cd2115
- transition/result.json: 151371b2edcc4eed0401f1bc4b15be84e45da741e1fa6d9541ed2e23a55bb0a2
- quorum/result.json: 294bde5bc637496111ee58261392528f358fae98420fcacd1af80ad03d107073

Remaining: qualify the final integrated source on the full-size ORCD namespace;
verify commands/options/virtual/load, retained generations and legitimate 32 GiB
refusal/recovery without extending budgets. Keep non-LOCAL/larger-tree global
fallback explicit; do not mark all recursive layouts bounded or global W92
closed from this LOCAL-subtree proof. Other ordinary-file EOF findings are
outside this change. No raw files committed and no push.
