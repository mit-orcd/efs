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

[`h->mu` contention plan and review](raft-host-mu-plan-20261016.md):
**NOTRUN; plan only.** Measure per-phase timing → routing snapshot →
race-free result-ring reads → reassess → conditional REPORT change →
proposal queue only if justified. Routing and the result ring are A/B'd
separately.

[Waiter wake-up outside `cv_mu`](orcd-mini-waiter-wake-20261016.md):
**deployed to orcd-mini (`4aff22c62`); smoke 216/216 PASS.**
`host_waiters_wake` decides under `cv_mu` and signals after unlocking.
- Under the user's workload, condvar re-acquire is 6.8% of the leader's lock
  waits (45.1% on `f4a373b13`).
- The leader's lock waits are now ~72% `h->mu`: proposals 29%, verdict
  reads 17%, leader-cover picks 14%.
- Private A/B: 21.4% → 2.2%; creates/s and latency unchanged within noise.

[orcd-mini profile on `f4a373b13`](orcd-mini-profile-f4a373b13-20261016.md):
**profiling only.** Under the user's workload, LOOKUP ReadIndex lock waits
stay at 0.8% (9.1% before the fix). The leader's top lock wait (45%) is woken
waiters re-acquiring `cv_mu` after `host_waiters_wake` signals while holding
it; proposals on `h->mu` come next.

[ORCD-test devel qualification `48644535f`](orcd-test-beta-48644535f-20261016.md):
**deployed; overall FAIL.** RDMA/direct-I/O, .efs enabled, expiry off;
packing/containers removed. Live core POSIX 1,512/1,512, cross-host 67/67,
cold durability 189/189, 109,452 exact mixed cycles and 4,715 queries PASS.
Faults 24/24 PASS; private GC passes on unchanged repeat after a peer-startup
proof race. Follow-up on node9901 fixes the Valgrind KV/query/daemon leaks
(per-thread KV buffer; daemon 5,160 → 0 B), both instrumented deadlines, the
missed `TEST_BINS +=` units and the GC peer proof race; ORCD-test rerun pending. The .efs cleanup-wrapper name mismatch now has a focused PASS
with the committed cleanup-name correction, including physical cleanup.
Annotated RDMA unit rechecks PASS. Kernel W121 and automatic recovery W71
remain open; this is not large-namespace/400-GiB acceptance.

[ReadIndex admission without `h->mu`](orcd-mini-readindex-lock-20261016.md):
**deployed to orcd-mini (`8391950f0`); PASS under the user's tiny-file
workload** (POSIX smoke 216/216). Fresh metadata readers admit themselves from
the published view plus a per-group `ri_begun`, raised before probes leave.
The pump starts requested rounds, so LOOKUP and GETATTR no longer queue on
the Raft host mutex. orcd-mini, 2 runs before and 2 after:
- ReadIndex lock waits 8.4% → 1.4% of efsd context switches;
- node9900 stat p50 2.34 → 1.70–1.82 ms;
- writer 1,420–1,506 → 1,793–1,797 files/s.

The private-cluster A/B agrees. One run after the change had a create p99
spike of 24–27 ms, which is not explained.

[W123 — automatic crash recovery](../backlog/crash-safe-durable-versions.md): **proposed new feature; source review only.** Reuse root barrier watermarks if epoch history, exact write tags and retained file versions prove recovery. No extra fsync-time metadata commit remains a hypothesis; eight-byte/zero-I/O cost claims are unproven. Implementation and acceptance NOTRUN; W71 remains open.

[orcd-mini deployment of `09f8af09c`](orcd-mini-deploy-09f8af09c-20261016.md)
(now running `4aff22c62`, waiter wake-up outside `cv_mu`; smoke POSIX 216/216 PASS):
**deployed; live POSIX 216/216, two-client 67/67, durability 27/27 PASS.**
`make test` has 4 environmental failures. GC crash-restart and fault-suite
integration FAIL on the known W71 uncertain-recovery fence, the same as the
previous deployment `4d6530dd8`. Includes the RDMA credit fix.

[Crash-path gates under the W71 fence](crash-gate-fence-fixtures-20261016.md):
**PASS (scoped), test-only change.** The fault suite uses a fresh cluster for
each server kill and asserts fsync EIO after restart: 24/24 PASS (RDMA
mlx5_0, direct). The GC gate asserts the EIO after its four-server crash, then
fresh-formats before POSIX and the offline-member section: PASS, with POSIX
216/216 and two-client 67/67. The W71 recovery blocker stays open.

[W122 — effective settings discovery](../backlog/work-items.md#w122): **PROPOSED**
read-only settings catalogs for `efsd` and `efs-fuse`, exposed through management
and the portal with defaults, effective values and startup/runtime scope. The
portal launch-environment draft is partial; backend implementation and acceptance
are NOTRUN. Small-cluster AMD/Intel qualification is sufficient.

[Native LMCache versus NIXL for AMD/NVIDIA GPU transfers](efs-kv-gpu-api-comparison-20261016.md): **API review only; implementation and hardware tests NOTRUN.** Prefer NIXL/UCX data movement plus EFS cache control and a GPU-aware adapter extension. Both current LMCache L2 paths use host buffers.

[LMCache through native NIXL: EFS integration direction](efs-lmcache-nixl-direction-20261016.md): **design only; implementation and performance NOTRUN.** Target native asynchronous EFS transfers; AMD GPU-direct support and shared publication/eviction require qualification.

[ORCD Raft/KV decisions](orcd-raft-kv-20261016.md): mixed correctness **PASS**
(297,651 exact readbacks; 57,935 queries), loaded POSIX **FAIL** (seven
15-second timeouts). Keep Raft batching, 100 ms checkpoint interval, concurrent
KV readers and 32 cache stripes; scan-seek cache benefit remains unproven here.
Copied-lock snapshot test fixtures fixed; 40/40 repeated KV runs PASS.
Orderly restart, 49 cross-client markers, 224 fixed payloads and seven cold
queries PASS; quiet POSIX reruns 21/21 PASS do not close loaded timeouts.
W121 remains open with kernel remediation deferred.

Experimental tiny-file packing and container-storage work was removed from
devel on October 16; its code, tests and records are kept at tag
`archive/packing-containers-20261016` (branch `packing-containers`).

Experimental group A was removed from devel on October 16: the manual repair
experiment, the staged D25 logical resize, data-GC admission, Raft admission
pressure, receiver feedback with early retire and group drain, and
`EFS_SKIP_META_FLUSH`. The last commit with them is tag
`archive/experimental-group-a-20261016` (`d57da8af6`). Their durable numbers
fail closed: Raft tags 45–51/55–57 halt apply and are refused on submit, KV
kinds 36–41 and wire messages 131–150 stay reserved, PREPARE kind 9 is
refused, and daemons refuse roots carrying repair markers
(`tests/test_retired_root_markers.py`). **Removal PASS (scoped):** node9901
`make all tests` clean; `make test` 765 commands with 4 environmental
failures: LeakSanitizer is broken on that host (a trivial leak program fails
too; the three affected tests pass with `detect_leaks=0`), and
`check-architecture` needs the omitted `results/` (5/5 on the checkout).

[RDMA credit overrun](orcd-mini-rdma-credit-20261016.md): **PASS on
orcd-mini under the user's workload** (`09f8af09c`): group-2 commits 229 →
~6,500/s; loaded create/stat p50 71/21 → 6.1/2.7 ms. Next bottleneck: LOOKUP
ReadIndex on the Raft host mutex. DATA plus CREDIT frames could exceed a peer's
posted receives; the RNR retry turned 30 µs request/reply rounds into 2 ms
and Raft commits into 26 ms on orcd-mini. `e84d42cbb` plus the
review fix `bbebedea8` bound each window at half the smaller receive pool
(6 receives by default). Private 3-node RDMA cluster: 16-thread creates
348–388 → 2,529–2,675 files/s; stat p90 2.3–4.5 → 0.3 ms.

[orcd-mini truncate](orcd-mini-truncate-20261016.md): **scoped PASS
(functional).** A staged DEL's garbage vlen wedged one replica's apply
(`apply lane-fence rc=-5` forever; truncate EIO, mkdir/rm EBUSY), fixed in
`502277d19`. Truncating files over ~1 GiB returned EIO (32-chunk lane-fence
cap); `4d6530dd8` pages the fence and reads the TRUNCATE verdict. A 100 GiB
`dd if=/dev/null` now completes in 4.3 s; data/regrow checks PASS. Crash
during paging NOTRUN.

[ORCD queued-read-vote comparison](orcd-queued-read-vote-20261016.md):
**NO—keep reverted.** Frozen `fa29f9277`, native RDMA/direct, `.efs` enabled,
no expiry; OFF–ON–ON–OFF across seven clients. POSIX
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
ACK-window fixes are committed; the loaded gate stays open. Queries, expiry
and seven-client load are excluded. This predates
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
TCP/direct/files with commands and expiry disabled.

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
