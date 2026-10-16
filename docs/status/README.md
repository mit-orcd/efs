# Project status — work queue and evidence

Reviewed and reorganized October 15, 2026. This directory is the work queue and
acceptance index; [backlog](../backlog/README.md) holds detailed work items and
parked ideas. W numbers identify work, not priority. Explicit human direction
takes precedence over dated assignments. Implementation, scoped test PASS and
full acceptance are separate states.

## Start here

| Need | Read |
| --- | --- |
| Ordered work and unresolved source findings | [Work queue](work-queue.md), preserving existing order and recorded verdicts |
| Execution handoff and scope caveats | [Handoff](in-flight.md) |
| Accepted architecture and pending decisions | [Decision register D1–D36](decisions.md), [specification/public-path review](spec-implementation.md) |
| Find a feature, platform or dated test report | [Complete topic catalog](catalog.md) |
| Earlier assignments, chronology and superseded captures | [Status archive](archive/README.md), [project archive](../archive/README.md) |

## Recent evidence and remaining acceptance

[ORCD queued-read-vote comparison](orcd-queued-read-vote-20261016.md):
**NO—keep reverted.** Frozen `fa29f9277`, native RDMA/direct, `.efs` enabled,
no packing/containers/expiry; OFF–ON–ON–OFF across seven clients. POSIX
failures15/0/10/0;22,182 mounted queries PASS, no repeatable net benefit.
**W121 OPEN, kernel remediation deferred:** [fork/read diagnosis](orcd-w121-fork-read-20261016.md). Correct daemon replies can reach stale application pages on ORCD; the performance harness now isolates query-launch processes. Original evidence: 255 immediate readbacks returned wrong bytes after successful
fsync across both variants. W119 remains open; this is not full acceptance.


[Raft follower batching default](intel-raft-follower-batching-20261016.md):
**scoped PASS (performance)** measured on `3fba43af7` with the opt-in bundle
(`EFS_RAFT_BATCH_OVERLAP=1`, `EFS_RAFT_BATCH_NOOP=1`, 100 ms applied interval),
now the default for both engines: lease edges 25 → 3.7–3.9 ms, AMD small-file
creates 77 → 345/s, busy-follower pump drains 12–16 → 1.3–1.6 ms. Same day:
retained-close fix and hashed open table (`ae3a49d8a`), concurrent LSM reads
(`3fba43af7`). Review found and fixed a nested-read deadlock in the
concurrent reads (`6b5c34624`); merged with the AMD durability branch.
Qualification NOTRUN.

[ORCD fresh-store read/close baseline](orcd-read-close-baseline-20261016.md):
W119 native RDMA/direct baseline on frozen852caf1ae, features disabled,
exclusive after graceful crawler/service stop. Three fresh-store core runs
216/216 PASS (11.3/11.0/11.1s), local peer67/67 PASS; no seven-client run. TCP leg pending scope clarification;
no A/B conclusion or AMD timeout closure.

[AMD asynchronous durability fixes](amd-async-durability-20261015.md):
**durability scoped PASS, loaded POSIX FAIL** on `c4178ea81`, branch
`devel-amd-durability-20261015` from `84ffe7a68`. Early ACKs retained; all-node
COMMIT, boot verifiers and persistent unclean-recovery refusal pass focused
and live RDMA/direct gates. Native, serial216/216, peer67/67 and cold27/27
pass; jobs16 is214/216 (`names_crazy_dirs`, `concurrent_creates_same_dir`
time out at15s). Follow-up `1a009c5ab` retains the memtable-prefix and
REPORT owner-preparation fixes (full native PASS); two quiet jobs16 runs
remain214/216; the restored follow-up is213/216 with an additional mtime
timeout, while peer passes67/67. Measured delay is principally fresh quorum waiting, not
connection checkout. A queued-vote scheduling experiment worsened loaded
results and was reverted. Clean-restart follow-up `b6454a7f8` passes focused
and live RDMA/direct durability tests: mounted writer/peer survive two clean
daemon restarts. Unclean recovery still fences COMMIT indefinitely; verified
recovery and per-file loss tracking remain missing. W71/W119 stay open; no
seven-client run or power-cut claim. Deployed follow-up `e53d20aa1`: core
serial216/216, peer67/67 and cold27/27 PASS; jobs16 remains FAIL215/216
(`names_crazy_dirs`, unchanged15s). Extras1 PASS/4 SKIP. No independent
reviewed commits met the selected merge scope; dependent changes deferred.
Single-case strace/perf follow-up `f62519501` removes the measured512-slot
wake scan; focused safety/sanitizer tests PASS. Isolated names case passes,
but quiet jobs16 remains215/216 then214/216: names still times out at15s.
W119 stays OPEN. Jobs16 correlation now reproduces the names timeout during
create/read/list: LOOKUP averages5.98ms,5.95ms in parent ReadIndex; follower
queued probes average8.89–9.48ms. Open/close dominate the traced case, not
directory enumeration. Follow-up close phases identify append-lock and REPORT
waits (one names close:53ms lock,56ms REPORT). Opportunistic queued-probe
draining and wider append stripes were not retained; the stripe comparison
was confounded by an independent AMD build. Original scheduling/64 stripes
restored; opt-in timing and deadline regressions retained. W119 remains OPEN;
no accepted timeout fix. Restored jobs16 FAIL205/216 (ten timeouts, one
unlink EBUSY); independent Intel-client storage benchmarks prevent claiming
a quiet isolated comparison. User moved the investigation to exclusive NUC: source852caf1ae build and
seven focused units PASS. User subsequently authorized TCP/direct on NUC:
clean four-node baseline core216/216 twice (16.9/21.2s) and peer67/67 PASS; isolated names0.8s
and deep>64 1.5s PASS, original deadlines/features unchanged. NUC RDMA remains
NOTRUN (no device/Soft-RoCE kernel); AMD timeout closure is not claimed.


[Asynchronous PUT acknowledgement](intel-async-durability-20261015.md):
**scoped PASS** on `3edc7b846` (async ACK from `d8af2c325`, concurrent-fsync
COMMIT fix, 200 ms read-mapping TTL with refresh-ahead and close-to-open), now
the only data PUT mode. On Intel/xorinox 2 GiB writes 151 → 166–180 MiB/s,
with fsync 141 → 176–179, four writers 94–101 → 173 (network-bound: 1.5×
erasure-coded bytes on 2.5 GbE); reads 237–255 MiB/s (88–92% of the link).
Two-client peer suite 67/67 PASS. Crash/power-loss qualification NOTRUN.

[AMD ordinary RDMA/direct POSIX](amd-rdma-posix-20261015.md): **FAIL loaded,
scoped functional PASS** on `c8d057391`, branch `devel-amd-posix-20261015`
from devel `761c91c42`. Full native/strict transport, serial216/216, peer67/67,
cold27/27 and paused-follower32MiB pass. One-mount jobs16 is215/216:
unusual-name directories still exceed15s. Fresh-read scheduling/log-lock and
ACK-window fixes are committed; the loaded gate stays open. Queries, packing,
containers, expiry and seven-client load are excluded. This predates
asynchronous PUT acknowledgement (`d8af2c325`) and does not qualify it.

[Grouped fragment durability](intel-cohort-durability-20261015.md) (superseded):
opt-in `EFS_PUT_COHORT=1` in `5cef06ac4`, removed by `d8af2c325`. On Intel with the xorinox
client, four concurrent writers 94–101 → 119–132 MiB/s and single-stream
146–149 → 169–170 MiB/s, with half the device flushes per MiB; local model
2.5–3x. Kept as evidence for why per-fragment barriers limited writes.

[Intel read metadata and read-cache fixes](intel-read-metadata-cache-20261015.md):
**PASS (scoped)** on `b3e93177c138` (compact GETCHUNKS replies) and
`49d721a4f180` (read-cache admission past the speculative budget), Intel
cluster with the xorinox client. Wire bytes per byte read 1.24→1.08 (1 MiB)
and 10.49→1.15 (32 KiB); single-stream 1 MiB 124→218 MiB/s, 32 KiB 12→43 MiB/s.
POSIX/peer/persistence/fault qualification NOTRUN; old clients reject the new
servers, so the AMD Intel-export client is stopped.

[ORCD TCP/direct, `.efs` disabled rerun](orcd-tcp-direct-disabled-20261015.md):
**FAIL** on the same `b3e93177c138` product source:895 PASS/120 FAIL/520 NOTRUN
in the seven-client round, peer61/67, clean stop refused. All23 successfully
prepared persistence cases verify after authorized forced restart; private
TCP POSIX/peer/GC, faults and memory gates pass. Current ORCD profile is
TCP/direct/files with commands, packing, containers and expiry disabled.

[ORCD latest-devel qualification, October 15](orcd-devel-qualification-20261015.md): deployment
PASS on `b3e93177c138`; qualification **FAIL**. Seven-client POSIX, cross-host
peer, enabled commands and clean stop have failures; W119/W120 track follow-up.
Valgrind and the standalone native-RDMA fault suite pass their selected scopes.

[October 15 open-item reconciliation](open-item-reconciliation-20261015.md) identifies superseded
implementation claims, existing closures, deferred work and gates that remain.
It includes current-source checks; it is not new runtime qualification.

These are entry points to exact-source reports, not claims about the currently
running clusters. Newer source commits do not inherit earlier test verdicts.

| Area | Evidence and limits |
| --- | --- |
| Intel/AMD sequential reads | [October 15 read coordination](intel-read-coordination-20261015.md) and [AMD read profile](intel-amd-read-profile-20261015.md) are measurement evidence. [Read metadata and read-cache fixes](intel-read-metadata-cache-20261015.md) record scoped PASS for `b3e93177c138`/`49d721a4f180`; full qualification of those sources remains NOTRUN. |
| Intel write path | [Eight-writer deployment](intel-default-eight-writers-20261015.md), [pressure draining](intel-pressure-drain-20261014.md), [sync batching](intel-sync-batching-20261014.md). Scoped smoke/native results; observations do not establish universal throughput gains. [Grouped durability design](intel-cohort-durability-20261015.md) is a local model only. |
| REPORT / W100 | [Newer-snapshot report](intel-report-newer-snapshot-20261014.md): W100 closed by scoped human acceptance October 14. Older OPEN entries are historical; this does not establish the cause or resolution of the original ORCD EIO. |
| Integrated RDMA source | [Merged devel qualification](devel-rdma-merge-qualification-20261014.md). AMD software-RDMA evidence does not replace ORCD hardware gates. |
| `.efs` functional versus scale work | [Three work packages and feature matrix](three-work-packages-20261013.md), [AMD package 2](amd-package2-query-20261013.md), [ORCD package 1](orcd-package1-20261013.md), [later query fixes](orcd-query-name-ordinals-20261013.md). General 400 GiB acceptance remains recorded as on hold; targeted fixes and marker checks do not close it. |
| Small-cluster follow-ups | [ORCD findings reproducible on AMD/Intel](orcd-small-cluster-followups-20261013.md). Preserve unresolved ordinary-I/O and scale obligations separately. |
| Packing and containers | [Packing ledger](packing-tcp-direct-20261012.md) retains its own unfinished gates; [container testing policy](container-testing-policy-20261012.md) excludes container qualification until explicitly reauthorized. Neither is enabled by this index. |
| Expiry | [Implementation](expiry-implementation-20261012.md) and [foreground fairness](expiry-foreground-latency-20261013.md) retain scoped evidence. Expiry stays excluded from the three-package profiles. |

The full chronological index is [preserved intact](archive/status-index-before-restructure-20261015.md).
Its old deployment instructions and grouped open counts are historical, not a
live inventory or an instruction to resume paused work.

## Maintaining this index

Keep this page short: link the current item ledger rather than prepending run
narratives. Put exact source/build, feature flags, environment, commands,
PASS/FAIL/NOTRUN, limitations and remaining gates in the evidence page, following
[the project evidence rule](../../AGENTS.md#evidence-and-reproducibility).
Raw captures stay outside Git; existing evidence is not deleted before an
adequate summary and reproduction instructions exist.

Add each new finding under an unused W identity in the [queue](work-queue.md),
with class, state, evidence, next action and acceptance gate. Check the queue,
[work-item details](../backlog/work-items.md) and decision register for collisions.
Keep supporting pages discoverable in the [catalog](catalog.md). Accepted design
is not completed implementation; close only against the named acceptance gates.

Archive only superseded records, preserve their findings and source-specific
verdicts, and update incoming links. Archiving does not close an item, cancel a
gate or authorize deployment. Keep unresolved work linked from its queue/ledger.
