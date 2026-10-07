# Next-five NUC acceptance — Oct 7

[Package results and limitations](../../../docs/status/next-five-gates-20261007.md).
All logs, including failed attempts, are retained verbatim. `fixture-metrics.json`
links each fixture to its originating log; failed metrics are not passing results.
`authority-and-binaries.json` retains partition responses and binary hashes.

| Gate | Passing log |
| --- | --- |
| Full Linux units after placement repair | [units](next5-release-units.log) |
| W27 2048-file trace, W69 immediate stats, W76 four-op failover and direct POSIX | [combined](next5-direct-combined.log) |
| Latest direct release, failover, du and POSIX | [direct](next5-release-direct.log) |
| Latest buffered release, four-op server restart and POSIX | [buffered](next5-release-buffered.log) |
| Real direct PUT/collision, two roots per server and LOOKUP barrier | [direct faults](next5-direct-faults-fixed.log) |
| Real buffered PUT/collision, two roots per server, LOOKUP barrier and immediate stats | [buffered faults](next5-buffered-faults-fixed.log) |
| 512 concurrent identity owners | [admission](next5-w76-concurrent.log) |
| Stored publication receipt, cross-parent transaction and voter-change partitions | [authority](next5-w82-all-views-repeat.log) |
| Production transaction coordinator authority guard | [coordinator](next5-w82-txn-unit.log) |
| Cold GNU du buffered allocation/time | [du](next5-du-buffered.log) |

The pre-repair `next5-{direct,buffered}-faults*.log` failures establish the
same-generation fragment rerouting bug; the explicitly `fixed` logs establish
its repaired behavior. `next5-buffered-faults.log` is a separate harness failure
(counting injections across remounts), not the production duplicate proof.
