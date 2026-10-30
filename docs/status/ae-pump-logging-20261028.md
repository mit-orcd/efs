# AE — routine pump logging, October 28, 2026

> **Port note (Oct 30).** Evidence ported from `agent/devel-task-ae` onto devel `7514b2145`; see [AE port](ae-product-port-20261030.md) for what was and was not ported. Its K55/K56 were renumbered **K72/K73** (devel already used K55/K56). Commit hashes, "current product" and "isolated branch" wording describe the original branch, not devel.

**IMPLEMENTED; qualification INCOMPLETE.** Current product `65bd82f6f` on
`agent/devel-task-ae`, base `714d7e6fc`; no merge, push or deployment.
Native slow/blocked/full-sink checks pass scoped, but the broad gate and
remaining real-HCA latency investigation prevent completion. [AB](ab-obs-dump-20261028.md)
shares the one reporter. No release acceptance is claimed.

## Failure and scope

Baseline devel `714d7e6fc` runs I/O/KV and optional batch output on its Raft
pump; periodic `raft-obs` output also holds `h->mu`. A bounded stderr pipe
whose reader stops therefore stops consensus/application progress. Three
35-second blocked-window runs completed only 32–60 writes versus the offered
700, with four writes taking over 10 seconds in each. Two runs also returned
four `EBUSY` errors and left four zero-length files after those failed
truncating writes. These are failed operations, not acknowledged corruption
and not successes recovered by a retry. Slow-reader pipes also failed. Normal
files completed all 700 writes in all three runs. `/dev/full` returns ENOSPC
immediately and did not stall the baseline. The original failed evidence and
intermediate harness failures remain retained.

The saturated pipe also exposed unconditional `opid-receipt: replay` logging
on the request path. It uses the **same** reporter as the periodic records;
its replay count and bounded per-identity diagnostics remain observable.
Critical role changes, halts, fences and failure lines retain their existing
synchronous path. The change does not promise nonblocking critical logging.

## Mechanism and bounds

One host-owned reporter, joined before its KV/host is freed, has four pending
fixed-size slots plus one active record. A snapshot is bounded by 16 KiB;
pending periodic snapshots coalesce and event records have 512-byte formatting
bounds. Producers use try-lock admission, bounded copies and no sink I/O.
Every refused/coalesced record increments a cumulative overflow counter;
formatting/sink errors are counted separately. Both are reported on subsequent
successful output. These are diagnostic losses, never losses of authoritative
I/O/error counters or consensus work. A blocked reporter cannot grow a queue.

AB takes only a numeric snapshot under the owner locks. Formatting, I/O/KV
sampling and batch formatting run on the reporter. Sender/finisher diagnostic
updates use atomics where the new snapshot reads across thread ownership.
Cadence uses monotonic time. There is no new environment reader or mode.

The reporter preserves timestamps from the installed stderr wrapper and
serializes whole records with critical stdio output. Pipes/terminals use an
independent nonblocking file description: changing flags on `dup(2)` would
also change critical stderr. Regular files instead share the original file
offset, preventing overwrite under ordinary shell redirection. Socket stderr
uses per-send nonblocking flags, preserving its original descriptor flags.
Stop interrupts a blocked pipe/socket writer. An uninterruptible filesystem
write is still subject to the daemon's existing overall shutdown deadline;
this is not a promise to cancel arbitrary kernel storage I/O.

## Reproduction and evidence

Host intel2, user maik, 8 CPUs, system fuse3 3.16.2; private fresh three-member
loopback TCP clusters, one FUSE mount, 2 GiB/node quotas, fragment I/O always
O_DIRECT, packing and `.efs` active, expiry off. Four writers each rewrite a
16-file working set of 512-byte files at five operations/second; every rewrite
fsyncs. A 10 Hz stat probe runs concurrently. All retained payloads are checked
exactly after sink recovery; failed writes remain failures.

Reproduction (fresh archived build on an otherwise idle Linux host):

```sh
make -j8 all tests/faults/efsd
./tests/test_telemetry
python3 scripts/pump_logging_profile.py --work PRIVATE/fixture \
  --output EVIDENCE/case --sink blocked --obs 1 --require-progress
python3 tests/test_pump_logging.py
make tsan-check
TMPDIR=PRIVATE/tmp EFS_GIT_ID=TESTED_COMMIT \
 EFS_GIT_BRANCH=agent/devel-task-ae EFS_GIT_DESCRIBE=TESTED_COMMIT \
 EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 \
 scripts/make_test_summary.sh
```

`PRIVATE` and `EVIDENCE` must be new paths outside Git. `--sink` also accepts
`file`, `slow` (at most 2,560 bytes/s after formation) and `full` (`/dev/full`).
The progress gate refuses errors, writes over 10 seconds and five-second
windows without committed foreground progress. The gate also requires
positive overflow counts after recovery, existing line formats and checked
unmount/daemon exits. It does not retry failed application operations.

Raw evidence: `intel2:/home/maik/efs-artifacts/taskABAE-20261028/`. Baseline
`baseline3-*` is the corrected repeated harness. Earlier `baseline-*` used a
removed CLI flag; `baseline2-*` includes a `/proc` thread-exit sampler race.
Those are retained harness failures, not attributed to the product. Baseline
measurement script SHA-256:
`a0b633357e2dc35ef1384bb0e01e1fb986950f71a0ae36b281ce6c494db009f2`.
Product progression and failures are retained, not replaced by later PASSes:

- `6ace68613`: repeated blocked/slow tests still failed. Read/apply wait
  diagnostics printed while holding `cv_mu`; routing them through the same
  reporter removed that wakeup dependency. Matched old/new controls with an
  independent fsync-interposer trace FD retain the old 24-write/two-late FAIL
  and the fixed 140/140 PASS. The 10 ms real-fsync delay is unchanged.
- `9d7e93ef2` plus matched probes: 15/15 private file/blocked/slow/ENOSPC
  runs pass, all offered writes complete with zero errors/late writes. A real
  64 KiB full tmpfs in a private user/mount namespace also passes and reports
  positive sink errors after space is released. The namespace is unmounted.
- `a2fe895c5`: full native gate 548 commands / 3 FAIL / 11 NOTRUN (795 s).
  Two source-extracted mock tests lacked the new reporter field/stubs; fixed.
  The third is B6 slow-fsync F0 EIO at 60 s, reproduced by the **same command**
  on untouched `714d7e6fc` on intel2. This is an inherited failure, not a PASS.
  The separate K35 durability fix `1ab9fef83` is not included here.
- TSan at `a2fe895c5`: 19/20 pass; existing reclaim-journal `rj.recovered`
  increment raced its status reader. The baseline file is identical at this
  site. `6130bafa1` makes its increment/status/final diagnostic atomic; K73
  records this diagnostic race, not observed durability corruption.
- `0cc3e8761`: full native gate 549 / 2 FAIL / 11 NOTRUN (730 s): inherited
  B6 slow plus W165's fault fixture halting before its intended CREATE.
  TSan reports no data race, but one regular-file reporter unit assertion
  incorrectly assumes its first try-lock snapshot cannot be dropped. The
  test now resubmits periodic snapshots until a complete record appears,
  still checking critical prefixes/suffixes and file offsets. No production
  lossless-telemetry promise or relaxed deadline was introduced.
- W165's post-ReadIndex corruption hook now rechecks arm/ready, and the test
  installs its marker before population can trigger a split. A deterministic
  extracted-site test fails on the old code and passes the new code; the
  original early-halt attribution remains a fixture-race explanation, not a
  proved historical diagnosis. No directory/read oracle was weakened.
- `bb57ef69b`: focused units/parsers/recipe and three serve-first subcases
  pass. Two complete W130 confirmations pass, but the third **FAILS after
  resync: 95/96 names, missing `f0064`**. All daemons stop cleanly; failed
  stores remain for OPEN K48. Three untouched-baseline standalone W130 runs
  pass, so this failure is **not waived** by a baseline exception. Later
  success cannot erase it. TSan is 20/20 PASS (558 s). Its eight-job `test-all` also fails the
  blocked-log regression itself: one EIO and one 29.88 s write, with 81
  successful writes. Closed fixture retained under `open-k55-bb57/`. This
  is not waived; later isolated passes do not override it.

The initial interposer-pressure controls logged the interposer's own trace to
blocked stderr. Those are retained harness failures; the corrected control
writes trace to inherited FD 198 without changing fsync semantics. Constructor
failure tests now require a daemon to refuse startup if its required reporter
cannot open the sink; an old whole-daemon eventfd negative control stays alive
and fails that test. Eventfd stays open to avoid dangerous FD reuse.

## Real HCA repeat and limits

Node9900, one physical host/HCA, three private processes on
`172.16.99.0:19191–19193`, mlx5_0, 2 GiB/member, O_DIRECT, packing and query
active, fresh stores. Existing production FUSE PID 2380112 (Oct 27 14:48:34,
`10.1.99.1:17432`, `/data1/efs/orcd-mini-mnt`) remains untouched. This is
single-host transport qualification, not distinct-host/fabric-failure coverage.

Hardware `0cc3e8761` includes common RDMA normal-up/close logging through the
same reporter and cherry-picked strict FIN-aware TCP accounting `38f3cecba`
(from its owner's branch, retained with `-x`): real post-setup TCP violations
stay synchronous; normal TCP FIN is not misclassified as one application byte.
RDMA segmented normal/churn/burst/burst-asym and xprt tests pass, as do telemetry
and TCP-accounting units. Forced RDMA and management counters demonstrate RDMA
use. Dropped normal connection diagnostics cannot prove absence of every TCP
byte; no such blanket claim is made.

All twelve v3 cases report PASS for their error/late/progress/payload checks,
with final expected-version readback and server applied-prefix advancement.
**That is not uniform throughput success:** TCP blocked delivered 700 writes
in each repeat, RDMA blocked delivered **242, 90, 700**; p99 write latency in
the first two was 4.06/7.97 s. These satisfy the test's existing >10 s and
five-second-progress checks but not the offered rate. Investigation remains
open; do not call AE complete from those PASS labels alone.

| Transport / sink | Writes, median [min,max] | stat p99, ms | Daemon CPU sum, s |
| --- | --- | --- | --- |
| TCP file (20 s) | 400 [400,400] | 10.911 [0.726,50.463] | 5.07 [5.05,5.35] |
| TCP blocked (35 s) | 700 [700,700] | 15.831 [0.689,26.127] | 6.44 [5.86,6.68] |
| RDMA file (20 s) | 400 [400,400] | 41.268 [20.795,51.574] | 6.72 [6.56,6.73] |
| RDMA blocked (35 s) | 242 [90,700] | 46.383 [10.848,46.494] | 3.63 [2.43,7.83] |

All blocked runs recover positive overflow on every node. Ordinary stat
syscall latency includes client caching; it is not isolated wire-RPC latency.
Hardware fault-server SHA-256:
`9f397a48a60d2f7811136ba9df90f2bc128cf9a9ac33aefb1eb6467a9f6df164`.
Other hashes, exact environment and before/after process inventory are retained
under `node9900:/data1/efs/evidence/taskABAE-20261028-node9900-a2fe895c5/`.
That directory name is historical; v3 tested **0cc3e8761**, not a2fe895c5.
Hardware repeats do not cover later startup-refusal and fault-fixture changes;
their positive/negative checks are native. Earlier v2 RDMA blocked FAIL (353
writes, four >10 s) and Python-3.9 `F_SETPIPE_SZ` harness failure remain.

Hardware reproduction adds to the command above:
`--transport rdma --rdma-dev mlx5_0 --listen PRIVATE_IP --ports UNUSED_1 UNUSED_2 UNUSED_3 --versioned`;
repeat file/blocked three times. Ports must be verified unused and isolated
from deployments. The runner's normal-file cases use 20 s and blocked 35 s.
The actual shared D4-kit driver, source/probe identities and repeated timing
results are described in [AB](ab-obs-dump-20261028.md).

Intel2 build/store pruning and final manifest sealing remain pending. Failed
K48 and the unresolved bb57 blocked-log stores are explicit open-investigation
bulk-retention exceptions.
All raw logs/results remain outside Git; no failed evidence is discarded.

## Remaining routine-output dependency and current repair

A same-cluster paired control (`370bc94c4` harness, unchanged hardware
product `0cc3e8761`) removes fresh-cluster placement as the explanation:
35 s freely drained completes 700 writes, then 35 s blocked completes 246,
p99 4.024 s. Two more paired runs complete 700/700; these were attached with
gdb and are explicitly **diagnostic, excluded from clean timing aggregates**.
Two earlier wchan-triggered runs captured no pipe-write stack and do not
establish absence of a logging dependency. The v5 stacks do catch the packer
blocked in `pk_group -> fprintf -> ts_write -> writev`, while its pump is
polling. This demonstrates a remaining background stall, not proof that it
alone caused the entire throughput loss.

`38ab684a3` routes successful packer pass/totals/compaction/backfill summaries,
subsequent query-publication summaries, zero-error heartbeat GC summaries and
ordinary session ACK diagnostics through the **same** bounded reporter.
Non-ACK session events, including fences, stay synchronous. Heartbeat summaries
with error counters remain critical, as do packer failures/degraded records.
`65bd82f6f` keeps the initial catalog-ready event synchronous for existing
startup consumers; `327e25b18` supplies the real session-apply extraction test
with the logger declaration. No consensus, storage or error verdict changes.

Native **65bd82f6f** focused checks PASS and TSan **20/20 PASS, 611 s**,
including recovery, segment rebuild and D4. The overlapping-load full gate
finished **550 commands / 3 FAIL / 11 NOTRUN, 820 s, build 13 s, seven
OVERBUDGET**: session-verdict extraction compile dependency, inherited B6 slow
EIO, and the blocked-log foreground EIO. `10654cf73` fixes the extraction's
logger stub without changing assertions; its portable focused compile/run
passes locally. Native rerun of that harness and controlled-host K72
investigation remain owed. No broad PASS is claimed. Real-HCA **65bd82f6f** qualification is now **PASS
scoped**: three TCP plus three RDMA paired runs each complete **700 drained +
700 blocked** writes, zero errors/late writes, exact final-version readback of
all 64 retained files, server applied-prefix advance and positive overflow on
each node. The stricter criterion requires at least 630/700 in each 35 s phase;
all six meet the full offered rate. RDMA segmented normal/churn/burst/burst-asym,
xprt, telemetry and TCP-accounting units pass. These are fresh runs, not
relabelling older raw PASSes or erasing their misses.

| Final paired transport | Drained write p99, ms | Blocked write p99, ms | stat p99, ms (whole 70 s) | Daemon CPU sum, s (70 s) |
| --- | --- | --- | --- | --- |
| TCP, three runs | 113.066 [6.620,113.505] | 77.912 [6.349,112.636] | 15.766 [0.607,46.554] | 16.94 [15.69,17.26] |
| RDMA, three runs | 7.708 [6.749,55.934] | 6.472 [6.410,55.642] | 0.599 [0.581,46.511] | 20.36 [20.15,20.87] |

Entries are medians [min,max]. CPU/stat windows include both phases, so they
are not directly comparable to the earlier 20/35 s independent-case totals.
Every run has 1,400 successful writes; final file versions, not every historical
payload version, are read back. There is no capacity-speedup or distinct-host
claim. Fault-server SHA-256:
`eab8df278af5bfd76b18abaacdd1de1bdb6f4f6516ad54776ffda2dfe7c9ff0b`;
normal daemon:
`7a0ee3ab3439d83ecad2571c612db5f1593a90efc4c0d0c9a55a4001524472ad`.
Exact build manifest is `binary-sha256-v6.txt`; runner and phase helper are
retained under raw `controllers/`. Private builds/stores are eligible for
pruning after stopped-process/mount checks; raw failures and stacks remain.
Native full gate and the literal D4 kit remain pending; AB/AE/K72 are not closed.

K48 failed stores have been copied and hash-verified on intel2 at
`/home/maik/efs-artifacts/taskABAE-20261028/open-k48/` (manifest SHA-256
`47fc4eece29a6a4a32a2762ff40085db5150d63c8b6c23ac20e1043fbbb68b3a`).
They are closed originals/copies, never replayed for this task. Retained helper
binaries are current **same-source bb57 builds**, not asserted bit-identical to
the standalone failed run's ELFs; its original runtime identity is in the logs.
This is the explicit open-investigation exception to normal store pruning.

Node9900 cleanup is complete: only
`/tmp/efs-taskABAE-20261028-a2fe895c5` was removed with `~/bin/edelete`,
after clean unmount/daemon exits and no-private-process/mount checks.
All private builds, stores and generated payloads (including failed throughput
fixtures) were pruned; text failures, stack captures, results, identities and
controller methods remain in the cited evidence directory. Its `PRUNED.txt`
records the deletion. The final **598-file manifest was fully hash-verified**;
`SHA256SUMS` SHA-256 is
`8a2a4e6c51d5fb42873f3982c68a60b86a730e0f7c9ff16f7cfd0705ea566bd6`.
Production FUSE PID/start/command remained identical after cleanup. No
production cluster was stopped, formatted or deployed.

## K48 copied-store follow-up (not a waiver)

The failed main W130 target is inode **385**, group 0; `f0064` is the CREATE
that triggers the intended replica-3 halt. The healthy-quorum listing includes
all 96 before resync; the later listing after resync omits that name. This is
more specific than the older count-only incident, but establishes no cause.

The existing `scripts/forensics/k48_store_scan.c`, compiled against this
fixture's **bb57ef69b** `libefs.a`, opens **disposable copies only** of each
closed `w130/nN/mdraft/kv`. All three copies contain 96 physical live dentries,
including `f0064` on shard 2778, child inode 266625. Static READDIR returns all
96 at page sizes 1, 2, 4, 8, 16, 32, 64 and 128 on every replica. The final
parent rows are SPLITTING with all 64 lane bits set. The saved originals still
match their full manifest after inspection. Outputs and scanner hash are under
raw `k48-forensics/`; original stores remain under `open-k48/`.

Reproduce inspection on an otherwise idle host with exact-source library:
`cc -std=gnu11 -O0 -g -Iinclude scripts/forensics/k48_store_scan.c libefs.a -pthread -lm -ldl -libverbs -o PRIVATE/scan`,
copy each saved KV to a new `PRIVATE/nN`, then `PRIVATE/scan PRIVATE/nN 385`.
Opening an LSM can recover/repair its WAL; never open an original capture.
This is static forensic evidence after shutdown, **not** a replay of the failed
live FUSE/DIR/read-proof interleaving. The next K48 attribution run needs
serving member, page cursor/proof and client/FUSE trace; static success never
turns the observed 95-name result into a PASS.

## Host collision / qualification hold

While the final intel2 native gate ran, process inventory found another
agent's Task AK full gate (`~/efs-taskAK/src`, PID 999000) active on the same
host. At discovery it had elapsed 6:26 versus 4:26 for this task's full gate,
so it also overlaps the last portion of the preceding TSan run. TSan still
reports 20/20 PASS; this is stressed race-check evidence, not an exclusive-host
timing. The native full-gate run remains retained with its actual outcomes;
background load does not waive any failure or prove its cause. It cannot
satisfy the task brief's one-agent-per-host qualification condition.

The **not-yet-started D4 queue was held before its first case**. No foreign
process was stopped. A coordinator reservation of intel2 after AK's current
run is requested before the uncontended full gate and literal D4 measurements.
Raw `host-overlap-65.txt` records the overlap. Hardware node9900 results and
cleanup are independent and complete. AB/AE/K72 stay incomplete pending these
native gates; the shared D4 kit has not been represented as executed.

The overlapping-load **65bd82f6f** full gate has now also failed the blocked-log
regression: **120 successful writes, one write EIO, zero successful writes over
10 s**. Failed path is worker 0/file 10. The failed fixture is retained at
`intel2:/home/maik/efs-artifacts/taskABAE-20261028/open-k55-65-efs-pump-logging-0t_3is0u/`;
its n1 runtime log confirms the build identity. Host contention is context,
**not a waiver or a proved explanation**. A controlled-host reproduction and
fix/classification are still owed before K72 closes; successful isolated and
hardware tests do not override this failure. The same run also reports the
previously reproduced B6 slow-fsync EIO. No timeout or integrity check is relaxed.

## Closed native checkpoint and continuation

All task-owned intel2 daemons/mounts are stopped. After preserving methods,
logs and the named open-investigation copies, **81 obsolete private scratch
paths** (old builds/fixtures/source archives, including the old short TMPDIR)
were pruned; `PRUNED.txt`/`PRUNED.json` list them. Inactive `gate-38ab`
(product65), `baseline-meter`, `screen` and `source65.tar` remain under
`/home/maik/efs-taskABAE-20261028/` solely for continuation. No Task AK tree or
production path was touched. The D4 benchmark never started.

Native checkpoint `SHA256SUMS-10654cf73` covers **8,319 closed regular files**,
fully hash-verified, plus nine separately recorded relative query-cache
symlink targets. Its SHA-256 is
`4fad1aabee671f72afb837efbcac5c86155bb54c0d34354be5bcaebaef5ee08a`.
Future runs must use new output names and retain this checkpoint.

Retention limit: the latest K72 copy used `shutil.copytree`'s default
symlink dereferencing, so its generated `query-cache/current` targets became
content copies. File contents/logs/KV are retained and hash-verified, but this
is **not a filesystem-exact image**, and exact original cache-link replay is
forfeited after scratch pruning. The earlier `open-k48` and `open-k55-bb57`
copies used `cp -a` and retain their relative symlinks. No full-payload or
exact-interleaving replay claim is based on any copied store.

Next work after exclusive host ownership is confirmed: reproduce the remaining
EIO with controlled drained/blocked fsync-pressure runs and capture the waiting
threads, fix or classify it without weakening the oracle, rerun strict native
`test-all`, then execute the literal matched D4 kit. Keep K48 attribution and
B6's independently reproduced failure visible. AB/AE/K72 remain incomplete.
