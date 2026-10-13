# Reviewed devel branch integration — October 13, 2026

User requested merging all other committed work into `devel`, with review of
purpose and overlapping parallel fixes. Starting source is `6b554814` (already
contains `devel-efs` through `52571794`). The inventory includes local branches,
remote-tracking refs and detached managed-worktree tips. All missing detached
tips were covered by the three branches below. The final inventory includes a successful `git fetch origin`; no branch,
remote-tracking or managed-worktree tip remains ahead of devel. No push,
service deployment or release promotion is claimed.

## Review and overlap decisions

| Input | Missing commits at start | Purpose and disposition |
| --- | --- | --- |
| release-v0.3.2-pre-alpha `b2e6cf06` | 21 | Integrate Mac session/CREATE, directory-group affinity, captured KV durability, fair lease retries, GC/writer pacing and profiler cleanup. Preserve exact historical FileIDs on replay. The source's three mounted Mac timeouts remain open. The unimplemented ancestry-cache proposal is documentation only. |
| devel-container `43b6176d` | 114 | Integrate committed experimental-container lifecycle, guarded reclamation, read controls, flock ownership, errno/permission fixes and opt-in observers. Keep trial/revert history; do not resurrect reverted receive scheduling. Shared files behavior must also pass. Two Intel mounted timeouts remain open; container activation/qualification is not part of this merge. |
| devel-expiry `e83a7db4` | 37 | Integrate committed opt-in expiry lifecycle/inspection and independent bounded Raft history/cache/cadence fixes. The NUC evidence belongs to its exact source/profile. EFS_EXPIRY remains off by default and explicitly off for ordinary-file integration. Current `.efs` expiry scope remains deferred. |

Specific resolutions, rather than choosing one whole branch:

- CLOSE: combine allocated-identity priority and publication from ORCD with Mac
  retry-turn fairness. Keep one bounded split retry pass; never discard an
  ambiguous owner. Preserve starvation and unbounded-stripe negative controls.
- Reads: preserve ORCD frozen quorum/apply floors, lost-wakeup protection and
  exact contexts; add container pre-send admission tickets and paired floors.
  Fully accounted old votes can be consumed without authorizing a newer round;
  progress-bearing votes still require ordered receive. Test these separately.
- KV: container deferred-flush holds cannot safely use Mac's detach without
  matching flush-hold accounting. Refuse detachment with BUSY while that policy
  is active, retaining the existing synchronous fallback and both nested/single
  hold refusal tests. Ordinary files retain the bounded captured-prefix worker.
- GC: retain atomic shutdown refusal, consumption of started replies, Mac
  between-record budgets/cursors and container joined physical batches. Cursor
  advancement covers attempted records only; all helpers finish before return.
- Raft storage: combine ORCD buffered rotation/cached CRC with expiry's bounded
  command cache and mapped derived index. Evicted payloads must be read and CRC
  validated from the old log before rotation; replacement offsets publish only
  after durability. Capture each append's physical offset before fsync releases
  the mutex, rather than adopting a later writer's end offset.
- Numbering: expiry's churn rename collided with existing W113 client telemetry.
  Keep telemetry W113, assign churn W114 and retain expiry W90. Update links and
  regenerate architecture; historical artifact filenames are unchanged.
- Test adapters: retain actual production bodies and semantic assertions while
  adding newly required declarations/fields and narrowing extraction boundaries.
  Separate independent Raft rounds so a valid vote in one scenario cannot make
  a later stale-vote scenario start already ready. Keep negative controls.

The compact query-link prototype remains in the named pre-merge stash and is
not part of these committed inputs. This merge does not resolve query ENOSPC,
original ORCD 5.569/22.044-second stalls or full-scale/platform acceptance by
ancestry alone. `.efs` on/RDMA/direct/no packing/no containers remains a separate
qualification profile; release branches and existing deployments are untouched.

## Validation

Exact combined source `986b4ec02083a8d561612627cccad7130c93a4c9` PASSes the
complete normal Linux build and `make -j2 test` on AMD as nonroot `efs`. The
suite executes 285 distinct Makefile-listed Python regression programs plus
its native binaries and subtargets. Every failure from the earlier independent
270/286 sweep is exercised successfully by the final full suite; the added
concurrent append-offset test also passes its old-behavior negative control.
All 849 product/test input checksums match the committed source, zero missing
or mismatched inputs. Kernel `7.0.0-38-generic`, GCC Ubuntu 15.2.0-16ubuntu1,
x86_64, libfuse 3.18.2; `make -j2`, default native synthetic fixture sizes and
private temporary stores. No mounted application suite runs in this gate.
Diagnostic binary build strings are `unknown` because upload excludes Git;
verified inputs and the following hashes establish the tested build identity.

| Artifact | SHA256 |
| --- | --- |
| efsd | `2365abdcc29168dc28dfee80105cb7bd0c85a72b8a7453ca7b2ac1bfafef19f2` |
| efs-fuse | `70d3fefc3f578a5e5535d6ca6f1d094562af07db64168f1211b06d8c37bc0ddf` |
| efs-mgmt | `63fe8a9003317fbe26cfb31c3f11db97f02c416ca9c86f1874453cacbbd27730` |
| libefs.a | `9eebd0de9fd2f8ac03e7d3f42e91648e61d5b37c545ce595f7012aab2c947d22` |
| full native log | `fd01d07d47960a665234e6b99d26a3d735b50064ef82009abf0d3a6b1e1ad08f` |
| build log | `dc11b8cef65d8c6c2eb405a29918e0c3b90764278f6022f2e44e2da774c70c5c` |

Full fault-enabled native, full sanitizer, mounted POSIX and performance,
RDMA/direct-I/O and ORCD scale qualification are NOTRUN for this combined
source. Some native gates include individual fault/sanitizer controls; those
are not full configuration qualification. Historical branch passes do not
transfer automatically. Documentation-only follow-up commits do not change
these tested product/test inputs. No binary is deployed.

AMD can also run the remaining full fault/sanitizer suites, isolated mounted
ordinary-file `.efs` behavior, smaller quota/footprint regressions, concurrent
mutation/query, clean unmount/restart, exact payload integrity, GC and
small-file/snapshot latency investigations. Intel is another Linux development
target. Reserve ORCD for final native hardware RDMA/direct-I/O, full catalogue,
seven-client concurrency, complete 400 GiB cold verification and real scale
latency/resource gates. This integration started no ORCD test or service action.

Reproduce on an isolated Linux source checkout:

```sh
make -j2 all
EFS_EXPIRY=0 EFS_PACKED_WRITES=0 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 make -j2 test
```

Individual tests deliberately enable feature-specific synthetic cases; these
unit tests do not enable the running cluster's deferred features. Raw build,
sweep and native logs are outside Git under
`/data1/efs/devel-integration-code-20261013` on AMD; local review diffs/archives
remain under `/private/tmp`. Test fixtures use private per-process directories
and perform their own cleanup; retained failed logs are not acceptance PASS.

## Commit-purpose inventory

Each row records the original commit's stated purpose and changed surface.
Disposition is integration in its reviewed branch context, including later
superseding fixes/reverts; it does not mean every intermediate experiment is
retained or every documented gate passes on the combined source.

### release-v0.3.2-pre-alpha

| Commit | Purpose | Surface |
| --- | --- | --- |
| `ac8cf2d7` | Remove orphan helpers and share profiler launch code | Makefile, results, src, tests |
| `5516ea4e` | Record Mac live POSIX failures and durability contention evidence | docs |
| `aec679e7` | Reduce durable append completion and background close retirement rounds | Makefile, docs, src, tests |
| `670c6853` | Avoid GC mutex fsync stalls and redundant settled lease quorum reads | Makefile, docs, src, tests |
| `abf6b7fc` | Use exact settled CREATE entry for retained submission proof | docs, src, tests |
| `f980d003` | Avoid empty deferred close cleanup with conservative atomic proof | docs, src, tests |
| `cc79b2f2` | Own proposal log holds before acquiring the host mutex | Makefile, docs, src, tests |
| `1ba5a465` | Bound retained lease and CREATE recovery mutex waits | docs, src, tests |
| `464b1ed0` | Allow guarded read votes without consuming queued replication | docs, src, tests |
| `47fd0e07` | Separate fresh ReadIndex votes from outstanding write suffixes | docs, src, tests |
| `832c0ac6` | Apply bounded FUSE worker defaults before library option parsing | Makefile, docs, src, tests |
| `a4679e14` | Rotate retained lease retries without dropping failed owners | docs, src, tests |
| `0fc79023` | Record complete Mac native verdict and isolated durability profiles | docs |
| `cf51a0b2` | Note retained fresh read probes before queued follower batch I/O | Makefile, docs, src, tests |
| `d3f6b110` | Avoid redundant directory REPORT ownership after committed mkdir | Makefile, docs, src, tests |
| `a12d7773` | Keep CREATE data publication ownership off committed parent directories | docs, src, tests |
| `caa016af` | Add captured KV WAL durability frontiers for off-pump persistence | include, src, tests |
| `a03981ae` | Reduce FUSE small-file session and snapshot lock overhead | src, tests |
| `0661d7e4` | Keep metadata progress off KV barriers and bound background GC interference | Makefile, src, tests |
| `32f71f27` | Keep nested directory allocation within its parent metadata group | include, src, tests |
| `b2e6cf06` | Document Mac small-file fixes, open native gates and undeployed ancestry proposal | docs |

### devel-container

| Commit | Purpose | Surface |
| --- | --- | --- |
| `8efa76c1` | docs: scope authorized Intel container investigation | docs |
| `3173be61` | docs: clarify scratch expiry feature is disabled for container sprint | docs |
| `4b905a48` | test: allow direct-only container correctness qualification | tests |
| `a2fc43d4` | test: add isolated two-host Intel container fixture controls | scripts |
| `3b23e3f7` | test: reproduce isolated Intel container POSIX and lifecycle gates | scripts |
| `19f6e3d5` | test: preserve container build identity and complete lifecycle phases | scripts |
| `59180db0` | fix(containers): bound new segment directory syncs to storage root | src, tests |
| `e2bfb186` | test(intel): investigate bounded 64-frame Raft receive slices | src, tests |
| `5aa4dafb` | test: isolate container evidence and add tiny-file cold gates | scripts |
| `e6757b3c` | fix(test): detect disconnected FUSE mounts and phase cluster restarts | scripts |
| `92ef337f` | revert: retain 32-frame Raft slices after failed Intel experiment | src |
| `fc2d2015` | fix(fuse): preserve authority errors from readdir pages | src, tests |
| `4d21f19c` | test: require bounded physical container reclamation on both Intel hosts | scripts |
| `ba7acebe` | fix(test): wait for Intel listeners and reject live reclamation fixtures | scripts |
| `27dca9c4` | Record Intel container qualification and retain unresolved load gates | docs, scripts |
| `73b1f915` | Cache pinned durable ancestry in the experimental container backend | src, tests |
| `eed87ad9` | Batch container inode retirement with retryable durable mappings | src, tests |
| `acee1609` | Make Intel container writer concurrency explicit for load qualification | scripts |
| `e25b6fce` | Use bounded progressing-backlog pacing in the private container fixture | scripts |
| `b4333b54` | Expose existing metadata counters for container diagnostics | scripts |
| `76bb4376` | Route experimental container metadata reads to known dual-group leaders | src, tests |
| `13567b2e` | Record fresh container reclamation pass and unresolved directory load gates | docs |
| `7761affa` | Batch container segment barriers and retain shared durable ancestry | src, tests |
| `b2379df2` | Retire container fsync cohorts after loaded timeout regression | src, tests |
| `df29de98` | Let container fresh read probes note votes before queued apply work | src, tests |
| `291ad470` | Document container load regressions and fresh-probe qualification candidate | docs |
| `5ce632dd` | fix(containers): preserve distinct Raft read and progress replies | src, tests |
| `ae5a1778` | fix(containers): avoid redundant durable GC work | Makefile, src, tests |
| `17138338` | docs: record container ACK and GC qualification checkpoints | docs |
| `c3bcffd1` | fix(containers): note queued fresh read votes before durable receive | Makefile, src, tests |
| `2682e8c5` | docs: record complete container GC and cold qualification | docs |
| `be506447` | intel containers: balance private metadata leadership across hosts | docs, scripts |
| `27fca85b` | containers: send bounded fresh read votes alongside replication | Makefile, docs, src, tests |
| `209c1f8b` | containers: retain distinct request proof prefixes in outbox | src, tests |
| `ec4e8498` | docs: record balanced container loaded verdict and pending fix | docs |
| `ab90a83d` | docs: finish balanced container qualification checkpoint | docs |
| `b0ba3819` | containers: prefer dual-group voters for metadata leader placement | docs, src, tests |
| `dc82cce3` | docs: complete priority-probe container qualification evidence | docs |
| `c23f9687` | containers: bound paired read proof helpers separately from files | docs, src, tests |
| `6d1b957f` | docs: finish dual-group container qualification checkpoint | docs |
| `5d454a9d` | Colocate container metadata proofs and bound timeout cleanup | Makefile, docs, scripts, src, tests |
| `1eb09a8c` | Route container paths and overlap independent physical fences | Makefile, docs, src, tests |
| `f28d44ca` | Make container fence controls clean under GCC warnings | docs, tests |
| `c4c4bb02` | Disperse container inode guards without weakening death exclusion | Makefile, docs, src, tests |
| `5f8c6717` | Record container guard physical pass and remaining directory failures | docs |
| `2065f216` | Admit container reads before waiting for the Raft mutex | Makefile, docs, src, tests |
| `5aa743b1` | fix(container): capture paired read floors before mutex wait | docs, src, tests |
| `0f851fcc` | fix(container): tolerate graceful daemon exit races in Intel fixture | docs, scripts |
| `ffa35558` | docs(container): record paired-floor Intel qualification failures and passes | docs |
| `db72bd58` | fix(container): service admitted read votes outside pump durability tail | docs, src, tests |
| `e9fbb5bd` | test(container): adapt flow harness for queued read-worker wakeups | docs, tests |
| `35ac542e` | fix(container): wake admitted reads only on useful proof transitions | Makefile, docs, src, tests |
| `cb39a4f6` | Bound obsolete container read-vote replies without folding replication progress | Makefile, docs, src, tests |
| `7cf827a0` | Record complete Intel container read-vote qualification and open directory gates | docs |
| `a0ecf931` | Hand off applied container read rounds to already registered demand | Makefile, docs, src, tests |
| `fab16632` | Retire container read handoff and identify slow apply and flush diagnostics | Makefile, docs, src, tests |
| `2f8d4b7e` | Defer held container metadata flushes to the off-mutex WAL release | Makefile, docs, include, src, tests |
| `034c04ee` | Bound SST fixture names for strict deferred-flush regression builds | docs, tests |
| `98bc99b3` | Record complete deferred-flush Intel container qualification | docs |
| `206a0548` | Bound superseded canonical container read requests as well as votes | docs, src, tests |
| `3d8f5e1b` | Record exact Intel request-queue qualification and open GC deadline | docs |
| `17f7db68` | Bound parallel container fragment deletion and join before metadata ACKs | Makefile, docs, src, tests |
| `68502a5f` | Record parallel fragment deletion safety and Intel loaded reclamation pass | docs |
| `326da5be` | Retire fully handled container read-only probes without duplicate FIFO receive | docs, src, tests |
| `13c7ca5b` | Record exact pure-probe qualification and reproducible deep-path diagnostics | docs, scripts |
| `e8f04e27` | Measure long client stripe waits during Intel container diagnostics | docs, src |
| `fb654a08` | Bound identical container inbox probes behind an explicit candidate flag | Makefile, docs, include, scripts, src, tests |
| `9610602a` | Fence duplicate inbox folding at every same-group control change | docs, src, tests |
| `290efa61` | Record completed lock diagnostic source qualification and next candidate identity | docs |
| `015b55c3` | Include actual duplicate inbox dependency in host flow safety adapter | docs, tests |
| `12ad2cf6` | Record corrected duplicate inbox native checks and exact private deployment | docs |
| `fafad9be` | Measure memo suppression without changing container read authority | docs, src, tests |
| `3213688b` | Distinguish suppressed memo insertion from expiry and cache misses | src |
| `b248b78a` | Record complete duplicate inbox qualification and next memo diagnostic gate | docs |
| `06b4624b` | Preserve newer container replication windows across older prefix acknowledgements | Makefile, docs, include, src, tests |
| `53db7b7f` | Add bounded opt-in deep path callback diagnostics for container investigation | docs, scripts, src |
| `f3d2b0ab` | Record complete older acknowledgement Intel qualification and callback diagnostic build | docs |
| `e14925c8` | Trace remaining deep-path callback service without changing authority | docs, src |
| `85177b6b` | Include bounded ACCESS timings in container path diagnosis | docs, src |
| `39464077` | Expose explicit private FUSE worker control for bounded queue diagnosis | docs, scripts |
| `9414a457` | Record ACCESS baseline and controlled Intel worker comparison | docs |
| `a879fac0` | Measure reply delivery after safe owned callback sampling | docs, src |
| `53999e1f` | Record complete normal Intel 64-worker container qualification | docs |
| `4990669e` | Observe canonical container read sender queue ages without changing scheduling | docs, include, src, tests |
| `11e9e7cd` | Keep derived read probe controls compatible with queue observations | tests |
| `f0af8482` | Restrict derived probe queue observations to canonical control frames | docs, src, tests |
| `92a144f8` | Make Intel deep-directory diagnostics reproducible | docs, scripts |
| `42316531` | Keep extra container read probes free of commit work | docs, src, tests |
| `b559c6e4` | Record complete Intel container candidate qualification | docs |
| `0d8ae026` | Measure container read authority wait phases | docs, scripts, src |
| `31abdc36` | Use monotonic clocks for authority phase durations | docs, src |
| `4a6d1543` | Use shared log cohorts for container background proposals | docs, src, tests |
| `24243a74` | Keep deep-call diagnostic emission nonblocking | docs, scripts |
| `d9002ff4` | Record complete container qualification and preserve syscall failure traces | docs, tests |
| `e664d218` | Retry transient unlock admission failures within existing RPC bounds | docs, src, tests |
| `a39cce7c` | Complete the traced container qualification checkpoint | docs |
| `aa496a7b` | Observe canonical read-control wire batch latency | docs, include, src |
| `4f8d4ad7` | Keep flock ownership until final open-description release | Makefile, docs, src, tests |
| `e13c4c79` | Record flock ownership qualification and remaining namespace blockers | docs |
| `5ee027fe` | container: test a larger bounded receive prefix to amortize KV release | docs, src, tests |
| `5cfdeee0` | container: record full receive-batch trial and restore prior scheduling | docs, src, tests |
| `1af4c703` | container: observe canonical TCP read frames inside mixed sender batches | docs, include, src, tests |
| `81aff433` | docs: checkpoint exact Intel container qualification and TCP timing | docs |
| `d2818564` | container: measure accepted canonical control age without changing FIFO | docs, include, src, tests |
| `7f0c611c` | container: separate entry read votes and enable validated overlap receive | docs, src, tests |
| `e960ce19` | docs: record complete Intel receive-overlap candidate qualification | docs |
| `56e898ef` | container: retire fully handled queued canonical read probes without flow | docs, include, src, tests |
| `2a56c060` | container: separate covered entry contexts and batch validated duplicates | docs, src, tests |
| `3cb226f2` | container: distinguish flock admission refusals from lock conflicts | docs, include, src, tests |
| `5b88e8cc` | container: authorize directory listings from fresh metadata | Makefile, docs, src, tests |
| `9145b7a3` | container: handle late read votes before pump durability tail | docs, src, tests |
| `e1123f40` | docs: record Intel late-read qualification and remaining two timeouts | docs |
| `1c86ac81` | container: preserve descriptor cleanup capabilities in phase observer | Makefile, docs, scripts, src, tests |
| `43b6176d` | docs: checkpoint committed container fixes and pending Intel validation | docs |

### devel-expiry

| Commit | Purpose | Surface |
| --- | --- | --- |
| `e2b2c361` | docs: reserve W90 for expiry and renumber churn benchmark W113 | docs |
| `70ba0424` | expiry: add durable deadline index and conditional namespace apply primitives | Makefile, docs, include, src, tests |
| `d867751a` | expiry: stage guarded inherited births and versioned policy controls | docs, include, src, tests |
| `31e1cbcb` | expiry: integrate clock-gated lifecycle worker and cross-group renewal | docs, include, src, tests |
| `54b193b3` | expiry: refresh follower clock health and preserve cleared file lifetime | docs, include, src, tests |
| `a5ac58a7` | test: prove expiry replay safety and disable-enable catch-up | tests |
| `9e292f84` | expiry: safely enable deadlines on existing scratch files | docs, include, src, tests |
| `d471b901` | test: select real expiry fragment holders and cross-group policy gates | docs, tests |
| `539290f6` | expiry: expose authoritative FileIDs and reject stale inspection | docs, include, src, tests |
| `0c68c032` | expiry: redirect unhosted policy and FileID controls to group peers | docs, src |
| `a0e88757` | test: require expiry FileID CLI inspection through both group redirects | tests |
| `0e975936` | expiry: fail over bounded deletion work from an offline coordinator | docs, src, tests |
| `973a8c27` | docs: close W90 with final NUC expiry and fault gate evidence | docs |
| `42abd400` | tests: expose expiry lifecycle gates through the POSIX runner | docs, tests |
| `5b41b8d3` | tests: create private expiry roots before invoking the fixture | docs, tests |
| `d075ab57` | docs: record passing NUC POSIX expiry runner qualification | docs |
| `1a0fc2a9` | expiry: add readable path inspection, worker telemetry and two-mount soak | docs, include, src, tests |
| `3b873839` | expiry: collect lane-derived logical size without blocking safe deletion | docs, include, src |
| `c14a952d` | tests: verify expiry status compatibility and document inspection and soak | docs, tests |
| `2da7a70a` | expiry: distinguish initial clock sampling from latched uncertainty | src, tests |
| `64967046` | docs: record passing expiry inspection, telemetry and NUC sustained soak | docs |
| `bd6570a9` | expiry: add readable policy durations and large backlog recovery gates | include, src, tests |
| `6b3f85ed` | expiry: distinguish duration parsing from inspection formatting | src |
| `d7bd5b1d` | expiry: restart with a partially drained large deadline queue | tests |
| `ad8d25fa` | expiry: explain duration and renewal semantics in standard command help | include, tests |
| `656fce08` | expiry: bound large backlog gates for the default cautious worker cadence | tests |
| `5bd5a95c` | expiry: clarify current policy limits for later renewals in help | include, tests |
| `bfdd7908` | docs: qualify expiry duration controls and 30,000-file NUC backlogs | docs |
| `565cc5cf` | expiry: bound retained Raft caches and shorten progressing backlog idle | include, src, tests |
| `e85b1b9c` | raft: certify retained-log indices from the actual window base | src |
| `a40f5d0a` | raft: test index capacity failure before durable append | src, tests |
| `afbce294` | raft: release mapped index residency after suffix removal | src |
| `bc7b6563` | docs: qualify expiry RSS and backlog cleanup fixes on NUC | docs |
| `e98ac96f` | expiry: keep fast backlog passes separate from full GC cadence | src, tests |
| `d5b23c38` | expiry: use monotonic deadlines for the split GC yield | src, tests |
| `99f6ef0c` | expiry: spread deletion bursts while keeping physical GC fast | src, tests |
| `e83a7db4` | docs: qualify foreground expiry fairness on NUC | docs |
