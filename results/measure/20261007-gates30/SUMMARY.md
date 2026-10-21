# NUC implemented-fix gate rounds — Oct 7

Retained commands, input hashes, exit codes and raw logs. Each passing round
validates its specified scope, not every remaining obligation of its package.
The fixture metrics include failed attempts; consult the corresponding log.

| Round | Gate | Result | Log |
| ---: | --- | --- | --- |
| 1 | W54/W38 direct authoritative folds | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/gates30-round01-repeat.log`) |
| 2 | W54/W38 buffered fold and cold GC | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-a/round02.log`) |
| 3 | W59 sustained direct writes and cold bytes | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-a/round03.log`) |
| 4 | W59 sustained buffered writes and cold bytes | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-a/round04.log`) |
| 5 | W69 immediate local mutation stats direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-a/round05.log`) |
| 6 | W69 immediate local mutation stats buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-a/round06.log`) |
| 7 | W56 root rename/name reuse direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-a/round07.log`) |
| 8 | W56 root rename/name reuse buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-a/round08.log`) |
| 9 | W60 bounded mixed demand/prefetch direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round09.log`) |
| 10 | W60 bounded mixed demand/prefetch buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round10.log`) |
| 11 | D17 cold dense/sparse allocation direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round11.log`) |
| 12 | D17 cold dense/sparse allocation buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round12.log`) |
| 13 | W68 real worker retirement direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round13.log`) |
| 14 | W68 real worker retirement buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round14.log`) |
| 15 | W67 sparse bounded admission direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round15.log`) |
| 16 | W67 sparse bounded admission buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round16.log`) |
| 17 | W75/W85 RPC integrity, ambiguous PUT/restart/accounting | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round17.log`) |
| 18 | W75 parallel GET payload and metadata anchor | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round18.log`) |
| 19 | W65 pooled/partial-reader signal shutdown | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round19.log`) |
| 20 | W82 reachable stale leader partitions | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round20.log`) |
| 21 | W76 lost committed namespace replies direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round21.log`) |
| 22 | W76 lost committed namespace replies buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round22.log`) |
| 23 | W84 namespace bounds/deep/spread direct | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round23.log`) |
| 24 | W84 namespace bounds/deep/spread buffered | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round24.log`) |
| 25 | W85 hint eviction/concurrency production helper | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round25.log`) |
| 26 | W27 phantom ownership and ambiguous REPORT helper | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round26.log`) |
| 27 | W23/W89 strict pressure measurement schema | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-b/round27.log`) |
| 28 | Full Linux unit and architecture regressions | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-c/round28.log`) |
| 29 | Combined direct POSIX/GC/namespace/worker gate | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-d/round29.log`) |
| 30 | Combined buffered POSIX/GC/namespace/worker gate | PASS | raw log (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-gates30/attempt-d/round30.log`) |

[Package disposition and limitations](../../../docs/status/gate-30-rounds-20261007.md).
