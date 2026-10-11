# Mac query delta investigation — W109 / W92

**Branch:** `devel-mac`, from devel `48a2719d`. **Cluster:** `mac` (three
Fedora aarch64 storage VMs and gateway on this Mac), buffered I/O/TCP.
This is neither native-IB acceptance nor an ORCD workload rerun.

## Confirmed cause and changes

The production disk query builder reproduces the exact 1,024-change cliff on
the Mac seed. A private KV fixture has 6,000 existing regular files and two
directories. Editing existing inode rows isolates changed-ID cardinality from
creation/parent updates. Every result is compared with an independent RAM
catalog, including all file stats and directory aggregates.

| Unique edited files | Baseline rows refolded | Fixed rows refolded |
| --- | --- | --- |
| 1,023 | 1,023 | 1,023 |
| 1,024 | 1,024 | 1,024 |
| 1,025 | 6,002 | 1,025 |
| 4,096 | 6,002 | 4,096 |

With 60,000 files, the baseline refolds all 60,002 rows at 1,025 and 4,096
changes; the fix refolds only the edited rows. This establishes the code-path
cause independently of ORCD's aggregate logs. It does not establish that every
ORCD rebuild takes this path or that it causes every foreground timeout.

- `7c3b48d2`: replace the namespace-limited changed-ID array with a transient
  candidate journal, using 256-ID buffers. Account journal bytes against disk
  admission, check partial I/O and cancellation, remove it before publication,
  and preserve errors instead of converting them to full rebuilds. IDs are
  visited once in the candidate stream and once only for old-only removals,
  so no namespace-sized deduplication allocation is required.
- `dadbb2ef`: compare sorted old/candidate inode streams with sequential merges,
  removing repeated binary lookups during revised-partition and removal scans.
  Current and retained generation isolation remains unchanged.
- `ff5f1c87`: collect removed IDs in the first merge pass rather than scanning again.
- `408043b6`: prove unchanged regular-file stat inputs by comparing exact active-lane, APPEND_CUR and conservative PVER values, instead of invalidating every file on an unrelated partition edit. Lane-only mutation/removal still invalidates correctly.
- `f02c4fae`: skip exact lookups when validated old/new optional stat families are both empty; an absent active lane still refuses reuse.
- New regressions cover the boundary, 4,096 updates, 2,048 creations and deletes,
  stat/rollup equality and journal short-write/ENOSPC refusal. Existing rename,
  hard-link, append-frontier, cancellation, cold restore and corruption/fault
  gates remain required and pass in the targeted seed run.

**Work reduction proven, latency improvement limited:** the final lane-free
60,000-file individual samples refold 1,025/4,096 rows in approximately 73/149 ms,
versus full-baseline 60,002-row refolds in 80/79 ms. The 4,096-change incremental
case can remain slower on this simple fixture. These are individual samples
with other VM activity, not a controlled performance verdict.

A second invalidation cause was reproduced with 6,000 lane-bearing files:
editing 1,023 / 1,024 / 1,025 rows refolded all 6,000 files even below the former
changed-ID limit. The exact-dependency fix refolds only the edited files.
Lane-only EOF/allocated-chunk changes and removal each refold exactly one file
and agree with the independent RAM catalog. Representative sustained live data
and repeated samples remain necessary before claiming lower foreground latency.

The 1,024 **dirty-directory** bound and missing-directory coverage fallback
remain. This repair removes the changed-inode cardinality cliff, not all full
rebuild causes or the complete W109 contention package.

## Deployment and retained-data blocker

The journal and first sequential-merge changes were built on the Mac seed;
full `make test` passed and those binaries were shipped to all three storage
VMs and gateway. Later focused query fixes passed the targeted seed oracles;
a fresh full gate passed on the seed using data-disk fixture space, and the latest
query code is deployed to all Mac service nodes (see the checkpoint below). The cluster uses
`NODE_QUOTA=10G`: its former explicit 15G exceeds the current safety estimate
(12.09 GiB on node 1). Cluster-script commit `386c710` permits a per-run quota
override. Existing stores were preserved; no wipe or forced write discard.
Build stamps from these rsync deployments say `unknown` because their tree lacks
Git metadata; running executable byte checks and source hashes establish the
actual deployed build, not that stamp.

The retained Mac namespace has an independent query bootstrap failure:
`query-disk: unlinked directory ino=4091 parent=2098 nlink=2`, followed by
`query-refresh: rc=-1 initial=1`. It also appears in October 10 logs, before
this repair. Its historical build attempts take roughly 231–248 seconds.
It prevents an installed-cluster incremental-query comparison until diagnosed
and repaired with authoritative namespace evidence. Do not delete the inode,
invent a dentry or suppress catalog validation to make it publish.

Fresh private fixture gates on the Mac seed validate query/RPC/two-FUSE
semantics; those use four local daemons and do not imply four physical hosts.
The installed three-VM native/peer POSIX gates are recorded separately.

## Evidence and checkpoint

[Boundary baseline](../../results/measure/20261011-mac-query-delta/baseline-6000.txt),
[60,000-file baseline/journal comparison](../../results/measure/20261011-mac-query-delta/journal-60000.txt),
[sequential merge and existing query oracles](../../results/measure/20261011-mac-query-delta/merge-oracles.txt),
[large creation/deletion oracle](../../results/measure/20261011-mac-query-delta/create-delete-oracle.txt).

## Live verdicts and retained evidence

Fresh private four-daemon/TCP fixture run1 passed initial catalogs, quiet metadata
refresh, partial-host refusal, RPC/JSON predicates, two FUSE mounts, permissions,
pages and streaming exports. Optional virtual-interface checks were 7 pass /
3 fail: FD capacity/recovery expected at least 20 but observed 3; deep indexed
name-limit convergence failed. These failures remain open. Failed private runs
have been unmounted and their owned processes stopped; stores/logs are retained.

The 1,536 payload-file run2 and 1,025 metadata-only run3 each exceeded the
compound fixture's 300-second deadline during creation (roughly 2–3 files/sec).
Neither is a passed large live delta gate. The deterministic 6,000/60,000-file
oracles are separate from this end-to-end liveness obligation.

Installed native POSIX initially reported 47/216 pass, 6 fail and 163 NOTRUN;
peer gates were not reached. The isolated October 11 12:53 UTC rerun reported **136/216 pass, 17 fail,
63 NOTRUN** in 254.2 seconds. Failures include directory/name/many-file timeouts
and concurrent append worker survival; subsequent worker checks show no tester
process remains. Its retained tree is `/mnt/efs/posix-efsgw-uapieh9s`.
Earlier apparent progress was baseline output, not a proof that EFS passed.

Read-only management inspection at 12:53 UTC confirms inode 4091 still has
mode 040755/nlink 2. Parent 2098 lists `level_4`, but authoritative lookup maps
that name to inode 8187, also a live directory. Inode 4091 lists no children.
This establishes a retained linkage inconsistency; it does not identify the
historical mutation that created it. No installed row/dentry was modified.
The installed query worker retries the same invalid namespace, with failed
builds of 236,526 and 248,576 ms in the 12:51–12:55 UTC window. Contention
causality and the expensive phase still need attribution.

## Two-hour continuation

Authorized Mac-only work runs from 2026-10-11 12:52:50 to 14:52:50 UTC. Default
remains buffered/TCP. No other cluster, merge or push is authorized by this run.
The tested empty-family optimization is committed; fallback diagnostics and a
dirty-directory boundary oracle are being validated. Installed service remains
up while isolated POSIX runs; deploy/restart follows clean fixture termination.

W109 remains open for dirty-directory coverage, representative performance,
retained Mac namespace diagnosis and original ORCD load/fault gates; W110 is
unchanged. See [ORCD investigation](orcd-communication-hypotheses-20261011.md).

### 13:00 UTC checkpoint

- `1533fb50` adds explicit `dirty-directory-budget` versus `missing-directory`
  fallback logs and a production-builder/RAM oracle in the default unit gate.
  On the Mac seed: 1,023 changed directories + root stay incremental (1,023
  refolds); 1,024/1,025 + root exceed the 1,024 closure budget and refold all
  7,027 rows. All directory stats and root aggregates match the RAM oracle.
  The bound remains an open scaling limitation; this commit does not remove it.
- Separate 15-second installed perf capture contains only **20 CPU samples**;
  it is insufficient to rank query hot paths. The subsequent syscall summary
  overlaps graceful shutdown, including 13,477 unlinkat calls, so it cannot
  attribute normal build latency. Preserve it as an inconclusive capture, not
  a repair justification. Raw perf stays on Mac efs2 under
  `/data1/mac-query-profile-20261011`.
- Installed Mac drained normally with no forced discard. Full latest build /
  unit gate / shipping is running. A focused fresh query smoke will run before
  service restoration, then native and peer gates run separately.

### Capacity-fixture repair

Latest deployment's full unit gate stopped in `test_storage_capacity.py`:
Mac seed `/tmp` is a 1.9 GiB tmpfs with 929 MiB available, below production's
1 GiB reserve. The refusal is correct; the test assumed arbitrary temporary
filesystems must admit a quota. Controlled inventory now tests duplicate roots,
shared usage, two devices and reserve refusal deterministically; real filesystem
capacity/failure checks remain. No production quota policy was weakened.
Successful `test_query_delta` fixtures now clean their own exact mkdtemp roots
with physical traversal; failures remain for diagnosis. Both revised tests pass
on Mac. The full unit retry is running, service still cleanly stopped.

### Full gate and shipping

Full Mac seed `TMPDIR=/data1/mac-unit-fixtures-20261011 make test` passed after
the fixture correction. Mac tooling commit `53d44ee` makes future deployments
use `/data1/efs-unit-tests` for live startup fixtures and defaults to the already
validated 10G quota. Directory setup/remote identity quoting and shell syntax
checks passed. All latest EFS daemon/FUSE binary hashes match on efs1/2/3/efsgw.
The focused 64-file metadata-only smoke is running in fresh private run4, with
existing RPC/Raft diagnostics. This is explicitly below the changed-ID boundary
and cannot replace the outstanding 1,025-file live gate. Service restoration
follows fixture cleanup; no forced discard, installed metadata edit or wipe.

### Live smoke and cleanup diagnosis

Run4's metadata update phase failed a new helper's early assertion: file counts
were unchanged while an older published snapshot reported transient refresh BUSY.
The helper now waits within its original 120-second deadline for complete,
error-free, exact counts, and still independently verifies every indexed mtime
and size against the requested mutation and peer syscall state. Run5 passed
64-file create/update/delete/rename/hard-link checks, normal RPC/command/page
gates and cold catalog restore, with root oracle 277 files / 7 dirs / 2,451 bytes.
No large-delta gate is inferred from this smoke.

Run4 also exposed a pre-existing fixture lifecycle error: Popen tracked FUSE's
daemonizing parent, so waiting on it did not prove the real client had drained
before servers were stopped. One unmounted client subsequently logged metadata
flush/data-loss warnings; it finally exited, and its fixture evidence is kept.
This does not prove payload loss (run4's new payloads are empty), and its cleanup
is not a clean gate. The harness now launches foreground clients and uses normal
stop-control drain while servers remain available; refusal preserves the private
servers/clients instead of killing an unresolved client. Fresh run6 validates
this lifecycle change. Installed Mac service was restored before run6, buffered
TCP; no private process from run4/run5 remained at its start.

A production-builder 64-parent/lane-bearing fixture passed before any alias
optimization: 1,023/1,025 updates took individual stats-phase samples of 128/126
ms; 4,096 took 463 ms. These samples are not controlled end-to-end latency
measurements. The existing per-inode/per-shard alias lookup order may churn the
small FD cache across many parent shards; the next step is a matched syscall
comparison before changing that traversal.

### Foreground/drain acceptance

Run6 passed the 16-file smoke, RPC/command/options/pages gates and cold catalog
restore (253 files / 7 dirs / 2,451 bytes) using foreground clients and normal
stop-control drain. This validates tracking the actual FUSE processes and
waiting for their teardown before fixture-server shutdown. No force discard is
used. Native/peer installed gates are next, separately, after query unit work.

### Alias FD-cache repair

`c09ef7f8` processes each fixed 256-ID journal batch by alias shard, rather than
reopening every alias shard for every changed inode. The 32-descriptor bound,
complete alias coverage, candidate isolation and cancellation remain. In a
controlled 64-shard/256-ID production-helper gate, identical lookup semantics
use 64 opens / 128 reads instead of 16,384 / 32,768. Immediate cancellation
opens nothing; a truncated alias record fails IO rather than requesting FULL.

All 6,000-file per-inode/RAM oracle checks and 64-parent aggregates pass, as do
creation/deletion, lane-only mutation/removal, dirty-directory boundary, rename,
retained snapshot, short-write/ENOSPC and fsync-refusal gates. Individual matched
logical-fixture stats samples for 1,023 / 1,024 / 1,025 / 4,096 edits are
128.292 / 117.518 / 126.129 / 463.091 ms before, versus
26.910 / 21.493 / 21.347 / 52.383 ms after. This demonstrates reduced cache work
and better fixture timings, not live ORCD or installed-Mac foreground latency.
The actual efsd is relinked with this code; controlled Mac rollout is running.

`04ba852d` commits the live mutation helper and foreground/drain lifecycle fix;
run5 and run6 acceptance are distinct from the original failed large live gates.


### 13:30 UTC Mac checkpoint: failed-build phase attribution

`e679adb7` adds diagnostic-only phase timing to failed query disk builds. The
Mac targeted query delta/disk/catalog/IO gates pass. The actual efsd is relinked
and shipped; installed service is restored with buffered/TCP and opt-in existing
Raft/client RPC counters. No other cluster is touched.

The first measured installed failure reports **index 261,430 ms / stats 345 ms /
persist 0 ms**, 40,939 inode rows; the same retained orphan directory rejects
publication. This rules out inode stat refolding as the expensive phase for this
attempt. It does not yet separate raw/index fsync cost from snapshot scans and
name sorting; that needs separate syscall/profile captures. RPC samples show
reply waits dominate pool checkout and sends. Observed Raft persist maxima are
roughly 69–92 ms; these are intervals, not proof of a causal mechanism.

The isolated native rerun is **137/216 PASS, 16 FAIL, 63 NOTRUN**, 244.4 seconds,
with an unchanged local-filesystem baseline of 216/216 PASS. Original deadlines
remain, and append worker survival aborts later cases. The initial peer attempt
was NOTRUN: its fixed `/mnt/efs/posix-2c` directory has historical permissions
incompatible with the test user. The cluster wrapper now uses a unique parent
for local peer mounts as it already did for remote peers. A fresh full peer run
is in progress; this fixture repair is not a passed EFS correctness gate.


### Full installed Mac peer acceptance, before raw-sync batching

The unique-parent local two-mount peer gate completed **67/67 PASS, 0 FAIL**
in 1,534.8 seconds. All 800 concurrent append records were present, complete and
unique. Append versus truncate, reverse extension completion, rename/unlink
source and destination races, hardlinks and range locks pass. The harness exited
and normally drained the secondary mount; no forced discard or killed client.
This is two independent FUSE mounts on one gateway against three Mac storage VMs,
not two physical client hosts. It does not turn the native deadline failures into
passes. A separate syscall/profile capture follows this correctness run.


### Raw-cache writeback repair (`a39edfa2`)

Separate syscall capture after peer teardown measured 330 query raw/index fsyncs
with mean 30.112 ms and median 27.748 ms. GC data/death-fence syncing is also
substantial (164 / 97 calls); Raft log, WAL and applied-checkpoint syncing remain.
These barriers protect durable ACKs and were not removed. The 30-second CPU
profile has only 58 task-clock samples, insufficient to rank CPU hot paths;
flat, thread and caller reports are retained.

A matched 128-file probe checks every byte after its timing interval. Merely
holding 16 writing descriptors before fsync showed no improvement and that
candidate was discarded. Initiating asynchronous Linux writeback for the bounded
batch, then fsyncing **every original writing handle**, shows:

| Sample | Sequential fsync (ms) | Writeback hint + checked fsyncs (ms) |
| --- | ---: | ---: |
| 1 | 4,008.6 | 885.6 |
| 2 | 4,463.3 | 884.4 |
| 3 | 3,912.3 | 735.1 |

`a39edfa2` adds that ordering to the actual query disk builder. A fixed 16-handle
queue is charged to working admission; the existing 32-entry read FD cache stays
bounded. Unsupported hints fall back to the same fsync barriers. Hint I/O errors,
fsync errors and cancellation abort the candidate and close all writing handles.
No durable generation is published before checked writer barriers, digests,
manifest and directory sync. Disk format and authoritative metadata are unchanged.

Mac production-builder gates exercise 64 historical inactive-lane partitions,
reach exactly 16 retained writing handles, and leave none open on success/failure.
Fallback, cancellation, hint EIO, every persisted-file fsync failure, partial
ENOSPC, retained checkpoint, cold restore, 6,000/60,000-file exact RAM oracles,
lane-only changes/removal and dirty-directory boundary gates pass.

The first broader unit run hit the unmodified benchmark direct/split/punch
30-second deadline under installed-service I/O. The same four-case test passed
in 4.750 seconds after clean service stop, and the **complete unchanged `make
test` then passed** with data-disk fixtures and service interference removed.
The failure is preserved; benchmark code/deadlines remain untouched. This isolates
installed-service interference collectively, not query-versus-GC causality.
The committed optimization is now being deployed to Mac, buffered/TCP; installed
native and peer gates are repeated separately. The retained orphan remains open.

### Installed writeback result and remaining foreground lead

With `a39edfa2` deployed, failed initial-build indexing measured 28,487 / 29,288
ms before the native workload and 34,067–51,231 ms during native/peer activity,
versus the prior 251,658–265,382 ms observations. Exact-stat folding remains
roughly 0.3–0.5 seconds in most samples. These are repeated phase observations
on retained, changing data, not matched end-to-end healthy publications: orphan
directory 4091 still prevents publication. No metadata repair was guessed.

The unchanged native suite after the writeback repair is **137/216 PASS,
16 FAIL, 63 NOTRUN**, 240.9 seconds. Its local-filesystem baseline is 216/216
PASS in 10.2 seconds. This is essentially unchanged from the prior 137/216,
244.4-second run. The query indexing improvement does not close foreground
liveness. The independent full peer rerun is pending.

Read profiling at 14:19:28 UTC reports 9,075 leader ReadIndex calls averaging
41,797 microseconds; lookup's parent ReadIndex averages 44,240 microseconds,
versus 15 microseconds for the local KV lookup. Lookup's lane authority work
adds 25,314 microseconds on average. Later cumulative averages depend on the
workload mix and must not be treated as paired latency samples.

The pump publishes/wakes satisfied views before its off-lock durability tail,
but remains the sole pump during WAL release and applied-checkpoint syncing.
An incoming heartbeat reply therefore may await that tail before the next
inbox drain. This is a code-supported hypothesis for remaining ReadIndex waits,
not proven timing causality. The next discriminating gate should correlate
request admission, outbound probe, reply enqueue/drain and view publication
with existing KV-release/applied-persistence phase timings. Delay each barrier
independently in a private preserved-byte fixture before proposing any pump
split. Keep the KV-before-checkpoint ordering, bounded replay tail and all
acknowledged-byte recovery gates. Raising native deadlines would hide the issue.

A 186–188-second whole-host Mac TCP window during the peer run records:

| VM | Sent segments | Retransmitted segments | Failed connects / resets / input errors |
| --- | ---: | ---: | --- |
| efs1 | 413,524 | 0 | 0 / 0 / 0 |
| efs2 | 148,951 | 2 | 0 / 0 / 0 |
| efs3 | 143,487 | 0 | 0 / 0 / 0 |
| efsgw | 201,124 | 2 | 0 / 0 / 0 |

SSH is included. These counters do not identify lost EFS RPCs or rule out
intermittent stalls. Mac buffered/TCP results do not close ORCD IB/RDMA gates.

A separate, tracer-free 20-second `/proc` observation during the peer workload
adds a storage-layer constraint:

| VM | Data write MiB/s | Data-device busy | Flushes/s | Full I/O pressure time |
| --- | ---: | ---: | ---: | ---: |
| efs1 | 3.00 | 96.9% | 294.8 | 31.5% |
| efs2 | 1.90 | 98.7% | 255.8 | 41.4% |
| efs3 | 2.12 | 86.5% | 158.8 | 37.0% |

Counters are whole-VM `vdb` and pressure deltas, including background work;
device busy is not physical-device utilization or an EFS-only percentage.
CPU pressure averages are near zero. EFS kernel wait samples include
`submit_bio_wait`, `xlog_wait_on_iclog` and `__flush_workqueue`. Per-cycle Raft
durability-tail maxima in the same interval are approximately 45–61 ms,
while pump-held work is usually under one millisecond. These observations
support durability waiting rather than CPU/network bandwidth as the next lead;
they do not distinguish query, GC and foreground causality. GC reports 1,223
removed files / 80,189,664 file bytes since service start, with pending work and
47 reap errors; classify those outcomes before calling them communication loss.

All four Mac nodes have matching query source and binary hashes. Every running
server and both gateway FUSE executables match the installed binary byte for
byte. Build identities and raw counter observations are retained with evidence.


### Final deadline checkpoint

The two-hour authorization ended at 14:52:50 UTC. No new implementation or
tests were started by the later heartbeat. The heartbeat is disabled.

The persistent cluster result for the latest peer run records **all 67 cases
PASS**, including all 800 concurrent append records. It has no final summary
or captured successful process/teardown exit, so this is case-level acceptance,
not a confirmed clean full-suite completion. The earlier independent peer run
has a complete 67/67 summary and verified normal mount drain. Latest native
acceptance remains 137/216 PASS, 16 FAIL and 63 NOTRUN. Full unchanged unit
acceptance passed with installed-service interference removed.

At the later heartbeat, /private/tmp checkouts and raw local test output were
missing. Commits and the persistent cluster case results survived. Tart reports
all four Mac VMs stopped; gateway SSH times out. Current mount health and
teardown cannot be verified, and no VM was restarted after the deadline.
The last verified running deployment was a39edfa2, buffered/TCP, with matching
source/binary hashes and running executables. All implementation and earlier
evidence commits remain on devel-mac. No other cluster, push or merge was used.

Remaining blockers: retained orphan directory 4091 prevents query publication;
native ReadIndex/durability latency still causes deadline failures; the dirty-
directory fallback bound remains. Mac evidence does not close ORCD hardware
IB/RDMA or representative million-row liveness gates.

### Resumed Mac ReadIndex/durability investigation

Work resumed at the user's request, on a persistent `devel-mac` checkout under
`/Users/mike/Documents/Codex/2026-10-05/how-x20/work/devel-mac`. Mac VMs are
owned foreground Tart sessions; detached launcher children stopped when their
exec session ended. The clean deployment full unit gate passed before service
startup. Installed stores were preserved, using buffered/TCP and 10G quotas.

The default checkpoint cadence reproduces **138/216 PASS, 15 FAIL, 63 NOTRUN**
in 249.9 seconds; the unchanged local baseline is 216/216 in 9.7 seconds.
The seed's 135-second counter window records 20,672 pump cycles: KV release
averages 2,341 microseconds across all cycles (including empty cycles), and
applied-marker persistence averages 1,717 microseconds. Aggregate sequential
pump time is about 48.4 / 35.5 seconds respectively. ReadIndex's cumulative
leader average is 41,164 microseconds. This motivates the existing bounded
100 ms checkpoint experiment, without altering WAL barriers or the 1,024-entry
replay-tail cap; its native gate is pending. No timing conclusion is inferred
from zero-work cycles alone. Captures now exclude old process incarnations in
the append-only log; earlier pre-run read-profile entries may be historical.

A demonstrated scheduling bug was also repaired: a failed query build's retry
interval was anchored to its start, so a build lasting longer than two seconds
could immediately launch another full rebuild. Failure backoff now starts at
completion. Successful start-to-start coalescing, pending-age reporting and
all authority/durability gates remain unchanged. Scheduler boundary checks
pass; installed validation of this separate change is pending.

The isolated 100 ms checkpoint run still failed: **138/216 PASS, 15 FAIL,
63 NOTRUN**, 224.5 seconds. A diagnostic run temporarily disabling query
publication reached **203/216 PASS, 13 FAIL, 0 NOTRUN**, 550.4 seconds; directory,
append and repeated-write deadlines still failed. Query contention contributes
but does not explain the entire failure. The policy was restored to mask 3,
version 2 before the next normal drain; this disabled-policy run is not an
acceptance gate. Evidence is outside the repository in
`/Users/mike/Documents/Codex/2026-10-05/how-x20/results/mac-readindex-resume/`.

Operation-local authority reuse now avoids reissuing a successful parent/name
shard ReadIndex while resolving the same LOOKUP's inode and lanes. SETATTR and
UTIMENS likewise reuse their own fenced inode group after successful mutation
settlement. Other newly observed lane/directory groups remain freshly fenced;
transaction uncertainty, leader routing and missing-inode retries are preserved.
No authority mask survives an RPC. The operation helper and captured-inode
replacement tests pass on the Mac Linux seed; Raft, query scheduler, transaction
read authority, follower redirects, readdir cursors, checkpoint intervals and
retirement gates also pass. The local standalone helper passes; the linked
replacement test needs the Linux-built libefs.a, absent in the macOS checkout.
The unchanged native suite is running with features restored and the existing
bounded 1,000 ms checkpoint setting; no default was changed. Full native
acceptance remains pending.

The operation-local native run with 1,000 ms checkpoints completed cleanly:
**203/216 PASS, 13 FAIL, 0 SKIP, 0 NOTRUN**, 622.0 seconds including cleanup.
All failures are the directory/name/concurrent append/repeated-write deadlines
listed in `native-covered-1000ms.txt`; it is not native acceptance.

A follow-up separates pure fresh read control from the pump's off-lock
durability tail. It only answers an empty probe from the known same-term,
same-boot leader when its matched prefix and commit are already applied, in a
stable configuration. Successful current-round replies can contribute read
authority before normal queued replication bookkeeping. A trylock and empty
inbox prerequisite preserve serialization and pending message order. New
boots, terms, log bytes, uncertain prefixes, unapplied work, storage failures
and joint configurations retain ordinary processing. No WAL/checkpoint barrier
is removed. Opt-in batch counters record handled requests and early replies.
The full Raft tests, including fresh rounds, parked apply and pending joint
configuration, pass locally with address/undefined-behavior sanitizers. Host
queue-order/trylock tests and admission fallback tests pass locally; Linux
full-unit and native validation are pending. The next live cycle restores the
original 10 ms checkpoint default, to isolate this fix from tuning.

The complete Linux `make test` passed for the read-control candidate with the
Mac service normally drained, including asynchronous sender/admission, durable
Raft disk, crash recovery, query corruption/fault and host checkpoint gates.
The candidate was shipped to all three storage nodes and the gateway. No
benchmark source or harness was changed. Installed native validation follows.

Pure read control is confirmed active by per-process counters on all three
Mac replicas, but the default-cadence native deep-directory probe still failed
both original 20-second deadlines (0/2, clean completion in 32.6 seconds).
No full-suite acceptance is inferred. Another demonstrated amplification is
batched LOOKUP_PATH: alternating ancestors repeatedly fenced already-covered
name groups within one RPC. It now carries its own successful group fences
through the bounded ancestor walk, explicitly fencing a newly encountered child
inode group before reading it. Final lane/directory validation records all
additional successful fences. Each independent RPC starts with an empty mask.
The existing 64-component protocol bound and client fallback are unchanged.
A production-handler regression walks 64 alternating-group ancestors with two
fresh fences, verifies permission ancestors, repeats with independent fences,
and checks missing groups, IO errors, absence and the existing depth boundary.
It and the operation/LOOKUP/transaction/control/sender/architecture gates pass
on the Mac Linux seed. Native deep-directory retesting is pending.

The batched path-authority fix also passed its Linux gates, but the unchanged
native deep-directory pair still failed (0/2, 33.4 seconds). A separate,
normally cleaned 60-level mkdir diagnostic with client RPC profiling showed
that this low-level FUSE path issued no LOOKUP_PATH calls. Its 2,158 LOOKUPs
spent 2.497 seconds receiving replies, whereas 61 creates spent 18.002 seconds
(~295 ms each). The path fix therefore does not establish a native-suite
latency improvement. Durable namespace writes are the remaining measured lead.

A bounded prepare collector now groups at most eight ordinary PREP entries,
preserving order and every exact index/term verdict. Only contiguous locally
led nonsolo groups share the existing checked log fsync barrier. Packing
failures discard unsubmitted commands, leadership changes fail locally instead
of forwarding inside a hold, and persistence failure overrides admission
failure. Every hold closes before remote calls or apply waits; COMMIT still
requires all original PREP verdicts. MKDIR and RMDIR use the collector. There is
no new transaction opcode or relaxed durability rule. Unit cases exercise the
bound, ordering, individual identities, role/fsync errors, remote boundaries
and preexisting shared holds. The initial full Linux gate stopped because the
REPORT coverage fixture mistook a forward declaration for a function body;
fixing its definition extraction preserved its durability assertions. The
remaining full-unit recipes, including Raft, transaction/session, persistence,
query and client drain gates, then passed. Live acceptance is pending.
The prepare-cohort live deep-directory pair also completed normally with 0/2
PASS (two unchanged 20-second deadline failures, 33.4 seconds including
cleanup). It is not acceptance and no native improvement is established yet.

Whole MKDIR/RMDIR requests now return a parent-group leader hint before
session admission or transaction effects when committed membership proves
that leader hosts both scratch groups. Unknown/self/single-group/removed
leaders keep normal forwarding; this avoids redirect loops on four-node
placements. Client retries retain the same operation identity. Routing,
namespace fencing, transaction resolution, follower redirection, metadata
retry-budget and shared-log coverage gates pass on the Mac Linux seed. The
unchanged live deep pair still failed 0/2 (33.1 seconds, normal cleanup).
This routing correction is not sufficient for native acceptance.

New-shard namespace admission now reads fresh session-owner authority before
logging CREATE: existing ACTIVE sessions skip the no-op CREATE, and an already
registered participant skips repeated REGISTER. Absence still creates and
registers; owner read errors, stale epochs and fencing never admit a write.
ESTABLISH retains its fresh global check, durable local floor and final local
acceptance. Local/remote owner, missing/error/fenced/revoked-state, registration
and final-check failure cases pass, as do the Linux session admission,
namespace cold-WAL/fencing, transaction and cohort gates. The full build is
clean. Opt-in EFS_READ_PROF now samples monotonic MKDIR phases at most once per
second per process; production defaults emit none.

The normally cleaned 60-level diagnostic with this candidate shows sampled
cross-group MKDIRs spending 70–130 ms on session admission and 116–150 ms on
transaction commit/resolution, versus 16–40 ms on prepare submission (one
74 ms outlier). Cleanup takes 11.70 seconds. Resolution currently proposes
ordinary per-participant entries separately, each paying a log durability
barrier. This is the next measured lead; no native pass is claimed. Slowing
markers to the existing 1,000 ms setting alone also failed the unchanged deep
pair (0/2, 31.4 seconds), so cadence alone is insufficient.

The collector now covers bounded locally led nonsolo prefixes across groups
sharing the physical Raft log, and COMMIT participant resolution uses it only
after the coordinator's durable decision succeeds. Every original resolution
entry and exact verdict remains independent; failed/partial submission,
failed fsync or a missing verdict retains the finisher and propagates failure.
The production collector/commit regression covers all 144 participants,
ordering across groups, undecided refusal, ambiguous decision, partial batch,
failed barrier and failed verdict. Linux Raft disk/core, transaction recovery,
apply retry, namespace fencing and packing-capacity gates pass. The live deep
pair still failed 0/2 in 37.4 seconds, normally cleaned. Phase samples still
show 106–188 ms transaction commit and 88–214 ms session admission under this
load. This is not evidence of a latency improvement. Repeated failed query
index builds take 24–37 seconds on each replica; a normally restored disabled-
query diagnostic will distinguish their contribution from consensus costs.

The follow-up query-disabled diagnostic also failed the unchanged deep pair
(0/2, 34.9 seconds, normal cleanup). Policy was restored to mask 3, version 4.
Failed catalog rebuilds amplify the IO load, but they do not explain the
entire remaining failure.

An uncommitted candidate moves applied-marker persistence to a worker with
one captured in-flight prefix and no growing queue. The pump still completes
its KV barrier before enqueueing; no log, quorum, apply or ReadIndex rule is
relaxed. The marker helper uses the published host view rather than racing an
underlying core pointer. A blocked previous job supplies backpressure before
accepting another; a failed KV barrier cannot enqueue a marker, and failed
marker persistence remains retryable. Thread creation failure retains the
synchronous path. Shutdown joins the accepted marker job before its existing
forced KV-before-marker checkpoint and file close. Actual worker persistence
cost remains in the opt-in batch histogram. Captured-prefix, bounded admission,
KV failure, marker failure/repair, fallback and blocked shutdown tests pass,
along with original checkpoint/retirement/cadence tests and local ASAN/UBSAN.
The full Mac Linux unit gate is running with service normally drained; native
validation and acceptance remain pending. Stores and VM services are preserved.
The checkpoint-worker candidate's full clean Linux `make test` passed and it
was shipped to all Mac replicas/gateway. The unchanged native deep pair still
failed 0/2 (34.9 seconds, normal completion); the worker does not establish a
native latency win. Its sampled MKDIRs still spend 84–178 ms admitting cold
namespace shards and 100–138 ms deciding/resolving cross-group transactions.
The current admission helper starts several independent ReadIndex rounds
within the same operation, including immediately after its own successful
REGISTER/ESTABLISH settlement. Request-scoped authority reuse is the next
lead; independent RPCs and new groups must still obtain fresh authority.

Namespace session admission now reuses only authority already obtained within
the same MKDIR/RMDIR RPC. It still reads the current session row and validates
ACTIVE state, epoch, registration and the durably applied local floor. Remote
owner checks and independent RPCs keep fresh ReadIndex rounds. The original
protocol wrappers supply no inherited coverage. Scope/error/fencing tests and
focused Linux session, cold-WAL, transaction, resolve and authority gates pass.
The unchanged native deep pair still fails 0/2 in 35.6 seconds, with normal
cleanup (`results/mac-readindex-resume/native-session-authority-deep.txt`).
Sampled admission phases often fall to 35–55 ms, but outliers remain and no
paired improvement or native pass is claimed. LOOKUP measurements show 2,891
calls averaging 5.59 ms: queued already-accounted read replies may hold later
fresh rounds behind the durability tail. This requires a conservative exact-
prefix control-path test, preserving replication bookkeeping whenever needed.

Read control now fully consumes an already-accounted successful empty ACK
only at the exact committed/applied full-log prefix, with matching recorded
peer match/next state. An ordinary ACK also requires no outstanding entry
batch; a probe preserves that batch. Boot, term, group, voter/config and inbox
ordering checks remain mandatory. Late rounds update peer liveness but never
current read authority; progress-bearing ACKs retain normal recv bookkeeping.
Sequential fresh/old-round, outstanding suffix, parked apply, failure and
configuration tests pass under ASAN/UBSAN and on the Mac Linux seed, together
with Raft disk, transaction, resolve, cohort and checkpoint regression gates.
Both the initial probe-only and ordinary-empty-ACK candidates still fail the
unchanged native deep pair (0/2, normal cleanup; evidence `native-accounted-
control-deep.txt` and `native-accounted-heartbeat-deep.txt`). The latest
LOOKUP average remains 5.45 ms. This is no native speedup proof. Log fsync
averages 19.35 ms; wire RTT averages 0.36–0.38 ms. Duplicate applied-marker
dispatch still waits behind an accepted same-prefix job and is the next lead.

The bounded marker dispatcher now skips duplicate requests for an already
accepted/completed exact prefix when no transport credits need retirement.
Newer prefixes retain backpressure; a failed completed marker is retried and
failed KV release still cannot schedule a marker. Actual worker tests cover a
blocked identical-prefix request, credit-bearing duplicate, new-prefix wait,
failed same-prefix repair and shutdown drain; ASAN/UBSAN and Mac Linux
checkpoint/retirement/control gates pass. Native deep pair still fails 0/2
(33.6 seconds, normal cleanup). Its LOOKUP average is 4.12 ms, versus 5.45 ms
in the preceding candidate run; these are consecutive diagnostic runs, not
a paired performance proof or native acceptance. Evidence: `native-checkpoint-
duplicate-deep.txt`, `checkpoint-duplicate-sample-efs1.json`.

Known locally led group reads now overlap their independent fresh rounds.
Both admissions precede either send; pending earlier rounds remain ineligible.
Different leaders retain scalar routing. MKDIR overlaps known parent/child
groups; LOOKUP uses unfenced bytes only as scheduling hints and rereads every
reply row after authority, fencing newly discovered groups. Stale speculative
route/timeout failures fall back to authoritative discovery; storage failures
remain errors. Pre-send admission, pending/current rounds, stepdown/routing,
overflow, hint corruption, independent-RPC and original replacement/transaction
authority gates pass on Mac Linux. Native deep pair remains 0/2 (37.6 seconds,
normal cleanup). A separate bounded strace -k directory diagnostic completed
and cleaned normally; its timing is not acceptance evidence. It proves 28
KV fdatasync stacks through handler-side host_apply, both after proposer log
sync and cohort release. One shown KV sync costs 12.4 ms while the handler
holds h->mu. These callbacks currently bypass the pump's KV sync hold.
`results/mac-readindex-resume/kv-apply-stacks.txt` records the evidence.
Separating the synced-log watermark from pump-owned apply is the next fix.

Handler-side log-sync completion now records its checked durable watermark
without calling commit/apply. Disk-backed proposer, background and cohort
release paths wake the pump, which retains commit/apply inside its original
KV hold. The existing synchronous API remains available for core/memory
callers. Own votes still require the actual synced-log prefix; command waiters
still require exact applied verdicts. A two-voter regression receives the
peer ACK before local sync, notes the monotonic watermark without commit or
apply, then verifies the pump commits/applies exactly once. Core ASAN/UBSAN,
Mac Linux Raft disk/transaction/resolve, host dispatch/log coverage, session
fencing, cohort, checkpoint and read authority gates pass. The live deep pair
remains 0/2 (36.3 seconds, normal cleanup); no closure is claimed. The full
unchanged native suite and local baseline are now running with one outer
fixture at a time; intrinsic multiprocess cases and original deadlines are
retained. Its deep-60 and 300-file cases have still exceeded their deadlines.
Live read samples: LOOKUP 3.71 ms average, KV body 7 us, lanes 3 us.

The one-outer-fixture native run completed incomplete: 144/216 PASS, 8 FAIL,
64 NOTRUN in 538.5 seconds. The surviving timed-out process group was in
concurrent_create_unlink_two_proc (not concurrent_appends_two_proc). Both
append cases also timed out. Workers later exited. Normal tree cleanup
finished; a stop attempted during cleanup refused safely and retained servers.
After cleanup the next stop drained normally. No force discard was used.

Fresh matched-prefix follower read probes now acknowledge before KV apply,
while requests owing commit/apply still enter ordinary receive. Only known
boot/current term/stable voters and previously persisted matching log prefixes
qualify. Leader applied-prefix authority remains mandatory. Zero-context
empty traffic contributes liveness only, never a fresh read vote. Parked
follower apply, prefix mismatch, deferred storage, fresh/old contexts and
normal queued apply regressions pass, including core ASAN/UBSAN and Mac Linux.

The checkpoint worker can defer a newer no-credit hint while its accepted job
is blocked, strictly below the original 1,024-entry cap measured from COMPLETED
marker floors. Captured work is not overwritten; pending credits and cap
boundaries still backpressure. Stop drains accepted work and retains the
existing forced latest checkpoint. Blocked cap/credit, captured-prefix, error
repair and shutdown tests pass under sanitizers and Mac Linux. Live deep pair
remains 0/2 (35.9 seconds including normal cleanup); neither fix closes the
native gate. Evidence: native-bounded-marker-deep.txt and
bounded-marker-sample-efs1.json under results/mac-readindex-resume.

APPEND reservations now collect independent captured image PREPs in the
existing eight-entry physical-log cohort, preserving stable refs and all exact
verdicts before DECIDE COMMIT. A flush/verdict failure before DECIDE drives
durable ABORT; an already-submitted ambiguous COMMIT is never converted.
Validation/no-op/error paths clear request-local capture. Generic resize/
append RESOLVE participants also share bounded barriers after their decision.
Captured-plan, owned-session, 69-image bound/overflow, pending verdict, sync
failure, abort failure, ambiguous COMMIT, nested-capture refusal and TLS cleanup
regressions pass under sanitizers. Mac Linux append transaction/recovery,
receipt, transaction fault, session and cohort gates pass; build has no warnings.
The isolated native multiprocess append still times out at its original
30-second budget (31.1 seconds including normal cleanup), without a surviving
worker. Its sampled reserve RPC average is 223 ms (54 calls), while REPORT
averages 600 ms (24 calls). Different runs/partial timed-out workloads are
diagnostic only, not paired performance or correctness acceptance. REPORT's
append completion still prepares images individually and is the next lead.
Evidence: append-cohort-unit.txt and native-append-cohort.txt.

Normal append completion and pruning now use the same bounded captured-image
PREP collector. Registered completion retains the exact local session guard;
last-close cleanup keeps its synchronous first PREP before checking leases.
No-op/capture-error exits clear TLS, and failures retain the same pre-decision
ABORT/ambiguous COMMIT handling. Extended sanitizer and Mac Linux completion,
prune, owned-session, transaction recovery and receipt gates pass. Live native
multiprocess append still misses its original budget: 0/1, 31.4 seconds with
normal cleanup. Partial RPC diagnostics show reserve 170 ms (65 calls) and
REPORT 408 ms (22 calls); not a paired throughput comparison or acceptance.
Evidence: append-completion-unit.txt and native-append-completion.txt.

Consecutive failed derived query builds now retry after 2, 4, 8, 16 and at
most 30 seconds, measured from completion. Success resets the delay; query
errors and pending age remain visible, and no namespace bytes are skipped or
repaired. This reduces repeated failed scratch rebuild pressure; it does not
resolve the retained orphan or promise native acceptance. Boundary, huge-count,
overflow and success-reset scheduling tests pass locally and on Mac Linux.
The full Linux make test passed on the Mac seed after append completion and
this retry change (latest-linux-unit.txt).

Ordinary inode/lane reads now overlap fresh locally led groups predicted
from unfenced row hints. The helper rereads transactionally afterward and
fences new authoritative groups; hints never supply reply data or authority.
Stale/missing hints and speculative BUSY/routing failures fall back to scalar
fencing; storage failures remain failures. Independent-RPC, changed lanes,
directory shard, pair admission, replacement absence and coordinator failure
gates pass on Mac Linux (inode-overlap-unit.txt); no build warnings.
A new full native gate is next, with original assertions/deadlines.
