# Next five implemented-fix packages — NUC acceptance

Requested after the 30-round gate pass, baseline `92f4319d`. The five selected
packages are W27, W69, W76, W82 and W85. Private fixtures preserve normal service
stores and unrelated benchmark/documentation work. Both NUC storage locations
are used; the ambiguous PUT gates also give every server two storage roots.

## Results and accounting

| Package | Result | Evidence |
| --- | --- | --- |
| W27 | **Closed** | Shared successful-local-PUT REPORT predicate; nonzero observed mappings rejected; real pending ownership retained; 2048 fsync/close files verify cold with zero identity misses; GC/restart passes |
| W69 | Correctness accepted; performance measurement open | Real LOOKUP reply held across chmod cannot reinsert stale attributes; direct/buffered immediate stats and POSIX pass; cold GNU du measured; unidentified Spark workload still owed |
| W76 | **Closed** | 512 concurrent production identity owners; four callers send nothing at saturation and recover after release; all four lost successful replies replay across leader death and full-server restart |
| W82 | **Closed** | Reachable isolated leader refuses publication receipts, transaction source/destination/inode views, session and inode/name views; voter shrink/restore and coordinator guard regression pass |
| W85 | **Closed after repair** | Real FUSE reply loss/collision with two roots per server retains generation and placement, probes on retry, verifies bytes, inventory/quota, deletion and restart |

| Open category | Previous | Now |
| --- | ---: | ---: |
| Unfinished correctness/investigations | 15 | 15 |
| Implemented/partial awaiting gates | 15 | **11** |
| Unresolved decisions | 3 | 3 |
| Future features | 18 | 18 |
| Total open packages | 51 | **47** |

These remain grouped packages. Four closures do not activate D25, implement
mount-wide session fencing, add repair, or establish other-host/RDMA acceptance.
W69 remains in its existing category; GNU du is not presented as the unidentified
historical Spark measurement. The tool/workload identity was requested.

## Actual repairs

W27's old status incorrectly said a nonzero-node staging fallback remained.
`3a1b4a52` already rejected unowned observations in both builders. `5d26932e`
centralizes that predicate and adds a regression with nonzero peer nodes.
The miss diagnostic now reports rejection accurately; its count/rate-limit
state uses atomic operations instead of racing concurrent reporters.

W85's new live gate first found duplicate same-generation fragments on different
nodes. Outer retries treated an ambiguous failed reply as permission to reroute,
even though the first node had accepted its PUT. The original copy was outside
the changed publication mapping. `389a8baf` permits relocation only before the
object's first send. Retries keep the generation and target nodes, so an unknown
outcome is probed on its original node. First-send NEW optimization remains.
Future relocation with tracked ownership/cleanup is separate work.

After repair, each direct/buffered fault fixture records **216 unique physical
objects and 14,155,776 charged bytes** (216 × 65,536), no duplicate fragment
identities, correct cold contents, and empty inventory/zero quota after restart.
The earlier duplicate-bearing failures remain in the evidence.

## Gate scope

- Both latest normal release modes: **216 POSIX passes, zero failures, one
  unsupported shared-mmap skip; 64/64 two-client tests**. Physical inventory,
  pending queues, quota and late-PUT fences are checked after restart.
- W76 maximum ownership uses extracted production allocator/callers with a
  transport send spy; it is a real 512-thread test, not 512 live FUSE callbacks.
  The default FUSE active limit is 32. Live RPC gates cover CREATE, LINK,
  RENAME and UNLINK with byte-identical retry identities and one effect.
- W82 has no standalone public transaction-decision query RPC. The live test
  performs a cross-parent namespace transaction and rejects stale resulting
  views; the production coordinator callback regression independently verifies
  failed authority cannot trigger a decision lookup or be interpreted as absence.
  Stored typed-publication receipts are tested through actual RPCs.
- W69 holds a real LOOKUP after its row was captured, performs fchmod via an
  already-open descriptor, releases the old reply and verifies a later stat.
  A concurrent old reply may finish; it must not become fresh memo authority.
- Five cold-client GNU du scans over 128 files report **524,288 allocated bytes**.
  Direct timings: 14.32, 14.10, 13.93, 14.13, 14.05 ms. Buffered timings:
  12.46, 13.63, 13.26, 13.02, 12.96 ms. These are elapsed scans, not a baseline
  speedup claim or Apache Spark measurements.
- Full rebuilt Linux units pass, including the added coordinator regression
  and concurrent identity admission. Normal-client strings exclude both new
  fault controls; fault builds are used only for explicit owned fixtures.

## Retained attempts and commits

Early harness failures are retained: an indentation error before fixtures
started; a scan omitting second roots; fault events from two client processes
counted together; and an unsupported management command name. The corrected
PUT gate enables its fault on a fresh client and reads only that process phase's
log tail. Production duplicate-bearing failures are distinct from these harness
errors and prompted the placement fix. Passing repeats do not erase failures.

Commits: `5d26932e` REPORT predicate/diagnostic; `00fe0adb` metadata and concurrent
identity gates; `389a8baf` stable PUT placement; `f761fc2a` live fault/recovery
fixtures; `b286cd4e` cold GNU du measurements.

[Raw logs, fixture hashes/metrics and authority records](../../results/measure/20261007-next-five/SUMMARY.md).
