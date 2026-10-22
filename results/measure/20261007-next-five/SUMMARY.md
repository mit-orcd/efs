# Next-five NUC acceptance — 2026-10-07

## 1. Purpose and linked item

NUC acceptance run for the five implemented-fix packages **W27, W69, W76,
W82, W85**, requested after the 30-round gate pass. Linked status page:
[docs/status/next-five-gates-20261007.md](../../../docs/status/next-five-gates-20261007.md)
(run added by commit `ceadd6d26` "docs(status): close four next-five gates and
retain W69 performance obligation").

## 2. Verdict

**PASS** for W27, W76, W82, and W85 (W85 closed *after* the placement repair
`389a8baf`; the pre-repair duplicate-fragment failures are retained as evidence).
**W69: correctness PASS, performance obligation OPEN** — cold GNU du was measured
but the unidentified historical Spark workload measurement is still owed.

## 3. Source commit and build identity

Tested baseline `92f4319d` plus gate commits `5d26932e` (REPORT predicate),
`00fe0adb` (metadata/concurrent identity gates), `389a8baf` (stable PUT
placement), `f761fc2a` (live fault fixtures), `b286cd4e` (cold du measurements).
Binary sha256 sets recorded in `authority-and-binaries.json` for three build
trees (`next5-release-src`, `next5-fixed-fault-src`, `next5-direct-release-src`);
per-fixture `source_tree_sha256` and binary hashes in `fixture-metrics.json`.
Build flags in `next5-build.log` (`-O3 -march=native`, build time
2026-10-07T21:25:22Z); `EFS_BUILD_ID`/`EFS_VERSION` compiled as "unknown".

## 4. Environment

Single NUC host, 4 efsd server nodes on one machine with two storage locations
(`/data1/efs`, `/data2/efs`); ambiguous-PUT fault fixtures give every server two
storage roots. FUSE clients on the same host; kernel transport loopback, both
buffered and direct I/O modes exercised. Ports in the 20700 range. Fixture
example: W85 direct fault fixture uses 4 roots across both disks, port 20700
(`fixture-metrics.json`).

## 5. Method

Gate scripts are the repo's fixture/gate harness (per-fixture manifests record
`test_script_sha256`; binaries built from the listed commits with `make`-driven
`gcc -O3`). Workload per package: 2048 fsync/close-file cold verification (W27);
LOOKUP reply held across fchmod then re-stat, plus immediate stats and POSIX
(W69); 512-thread production allocator/caller admission with transport send spy,
and lost-reply replay across leader death and full-server restart for
CREATE/LINK/RENAME/UNLINK (W76); isolated-leader publication receipt refusal,
cross-parent transaction views, voter shrink/restore (W82); real FUSE reply
loss/collision with two roots per server (W85). Both release modes then run the
POSIX suite, two-client suite, cold GNU du (5 scans, 128 files), restart
inventory/quota/GC checks. Early harness failures (indentation error,
second-root scan omission, cross-process fault counting, bad mgmt command name)
are retained and excluded via `normal-fault-exclusion.txt`.

## 6. Results

| Gate | Passing log |
| --- | --- |
| Full Linux units after placement repair | units (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-release-units.log`) |
| W27 2048-file trace, W69 immediate stats, W76 four-op failover and direct POSIX | combined (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-direct-combined.log`) |
| Latest direct release, failover, du and POSIX | direct (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-release-direct.log`) |
| Latest buffered release, four-op server restart and POSIX | buffered (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-release-buffered.log`) |
| Real direct PUT/collision, two roots per server and LOOKUP barrier | direct faults (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-direct-faults-fixed.log`) |
| Real buffered PUT/collision, two roots per server, LOOKUP barrier and immediate stats | buffered faults (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-buffered-faults-fixed.log`) |
| 512 concurrent identity owners | admission (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-w76-concurrent.log`) |
| Stored publication receipt, cross-parent transaction and voter-change partitions | authority (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-w82-all-views-repeat.log`) |
| Production transaction coordinator authority guard | coordinator (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-w82-txn-unit.log`) |
| Cold GNU du buffered allocation/time | du (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261007-next-five/next5-du-buffered.log`) |

| Check | Direct | Buffered |
| --- | --- | --- |
| POSIX suite | 216/217 pass, 0 fail, 1 skip (73.7s) | 216/217 pass, 0 fail, 1 skip (65.9s) |
| POSIX 2-client | 64/64 pass | 64/64 pass |
| Post-repair fault fixture | 216 unique physical objects, 14,155,776 charged bytes, no duplicate fragments | same |
| Cold du, 128 files (5 scans) | 524,288 bytes; 14.32/14.10/13.93/14.13/14.05 ms | 524,288 bytes; 12.46/13.63/13.26/13.02/12.96 ms |

The pre-repair `next5-{direct,buffered}-faults*.log` failures establish the
same-generation fragment rerouting bug; the explicitly `fixed` logs establish
its repaired behavior. `next5-buffered-faults.log` is a separate harness failure
(counting injections across remounts), not the production duplicate proof.

## 7. Limitations and remaining gates

- W69 performance measurement against the unidentified historical Spark
  workload is still owed; GNU du is not that baseline.
- These closures do not activate D25, add mount-wide session fencing, add
  repair, or establish other-host/RDMA acceptance.
- Open package totals after this run: 47 (was 51); 15 correctness
  investigations, 11 implemented awaiting gates, 3 decisions, 18 future.
- W76's 512-owner test uses extracted production allocator/callers with a send
  spy — a real 512-thread test, not 512 live FUSE callbacks (FUSE active
  limit 32).

## 8. Evidence index

| sha256 (16) | File |
| --- | --- |
| 67979f8efd23f0d1 | fixture-metrics.json — per-fixture logs, gates, tree/binary hashes |
| ced365514c841c2c | authority-and-binaries.json — W82 partition records, binary hashes |
| df39a9220e29a8b2 | next5-release-units.log — full unit suite after placement repair |
| b20188e58e06e93b | next5-release-direct.log — direct release gates incl. POSIX/du/failover |
| db3744fa478ba56e | next5-release-buffered.log — buffered release gates incl. restart POSIX |
| b4abd211ce1deff6 | next5-direct-faults-fixed.log — W85 direct fault gate after repair |
| 7af4d374a540f359 | next5-buffered-faults-fixed.log — W85 buffered fault gate after repair |
| ef630e210b558543 | next5-w76-concurrent.log — 512 concurrent identity owners |
| 6b5bda8741adc18f | next5-w82-all-views-repeat.log — W82 live partition/authority gate |
| 6f02135942a1147d | next5-w82-txn-unit.log — coordinator authority-guard regression |
| b9be5a4bf1a75725 | next5-du-buffered.log — cold GNU du buffered measurements |
| 44e42f807bd5c1e9 | next5-direct-combined.log — W27/W69/W76 combined direct gate |
