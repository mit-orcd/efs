# Intel container investigation

User explicitly reopened container-specific work on devel-container based on release-v0.3.2-pre-alpha, base3a41493591dcede820700db7d934da9dc10f07ee. This authorizes container investigation on this branch; the experimental backend remains excluded from unrelated release qualification and is not production-approved.

Scope: Intel intel1_efs/intel2_efs, four daemon topology (two per physical host), containers engine, TCP/direct I/O, EFS_COMMANDS=0 and EFS_COMMANDS_SERVER=0, EFS_PACKED_WRITES=0. Use fresh private container roots and ports, never switch existing files roots in place. Required quota/fixture sizing follows available capacity, recorded explicitly before tests.

User clarification: disable the scratch-file expiry-date feature (W90), including expiry dates, renewal, expiry-based namespace deletion and its background lifecycle. This does not disable operational RPC/test deadlines or unrelated cache/session bounds. The release baseline documents expiry as unavailable and rejects expiry arguments; no expiry activation or new expiry implementation is in scope. Keep this inactive state on Intel and verify it before container tests. Do not invent an expiry environment flag: no runtime activation control has been established. Required durability and fencing remain unchanged.

Initial work: review existing backend gaps, run focused container storage/durability/quota/GC tests before live deployment, then actual two-host mounted POSIX/concurrency, overwrite/truncate, cold restart and physical allocation/reclamation checks. Separate correctness qualification from matched performance comparisons. No small-file packing or .efs query matrix; shared metadata and ordinary writes still exercised. Live qualification is underway; the seven-client original-budget gate is FAIL. See the dated checkpoint below. No defect is marked fixed by branch creation. Record concise reproducible summaries per AGENTS.md; raw artifacts stay outside Git. Commit fixes and affected verification on devel-container; no push or promotion implied.


## October 12 checkpoint: TCP/direct container qualification

Purpose: dedicated experimental container investigation, with remaining Intel
load failures tracked under [W100](../backlog/work-items.md#w100). This is not
release acceptance or evidence that containers outperform files.

The first fixed runtime source was **59180db0f6f906ae0d96572b98b0020630ee477e**, based on release
**3a41493591dcede820700db7d934da9dc10f07ee**. It bounds new segment directory
publication syncs to the initialized storage root. Startup still seals the full
root ancestry; segment bytes, the container directory and storage root retain
required syncs. A new regression failed on the original implementation and
passed after the fix. This does not establish a remedy for full-load timeouts.

Four private daemons: nodes1/2 on intel1 (10.0.0.207), nodes3/4 on intel2
(10.0.0.209), ports27432/27433, containers/direct, writers2 and **4G quota each**.
Seven FUSE clients are split4+3; each seeds from the opposite physical host.
The private base is `/data1/efs/bench/container-3173be61-20261012`. Roots n1/n3
are under that base; n2/n4 are under the same basename in `/data2/efs/bench`.
**/data2/efs resolves to /home/efs/additional-work-dir on a separate874G
/home XFS filesystem** (732G free on intel1,739G on intel2 at final inspection).
It is not on the70G root filesystem. /data1 is a separate954G XFS/NVMe
filesystem (742G/759G free). Existing production files daemons on17432/17433 and mounts at
/data1/efs/mnt are separate.

Actual process records verify `EFS_TRANSPORT=tcp`, `EFS_MD_RAFT_N=4`,
`EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0` and daemon
`--storage-engine containers --direct-io`. `.efs` is absent from both private
mounts. Scratch expiry dates remain unavailable in this baseline and cannot be
activated through disabled commands. Operational deadlines remain enabled.
Both peer ports are reachable in both directions after the permanent firewall
rules were corrected and reloaded.

The filtered build manifest verifies all780 regular build/test/script inputs on
both hosts. Each build explicitly sets EFS_GIT_ID and EFS_GIT_DESCRIBE to the
full runtime SHA; status reports4/4 identical versions. Earlier archive builds
had unknown human-facing versions and produced a status mismatch; explicit
metadata fixes that deployment issue without changing storage semantics.

| Earlier59180db0 binary | intel1 SHA256 | intel2 SHA256 |
| --- | --- | --- |
| efsd | f7eedec088189ddb4d8a50b3e55e643d83c06a2c392958f2ef3a81fd6fba6a28 | 4bf4576bba282b10c172f689abf8a4189ca33c1f6ade132d606bc6a568e5aec8 |
| efs-fuse | f78460c549e91ff20f24340fd2cd10cb229ef2875d9c66c3a7eb3bdee370e1dc | f4e100c2188c0a3494d2e15dcbbfa5ca8b477eecb45acd450a8efffce1566c10 |
| efs-mgmt | 7118f09909385e966a6c67c904c87392e1607f6df3c38da27e2a553d669fdb50 | bd7c74c3077f77def18dd4ad0e0ca6e01b856fe8ce26fb3d26529907555a0358 |
| efs-query | e63493ed93a199001bf8b8991d8235ec4ec15240203471bd63b8dfb2a89ed008 | 09b9c03727005ef8ea97de822aec61497eacb139adce33842a6370fd4453c3ac |

### Results and limitations

Native `test_store_containers --direct-only` and its complete default control
(buffered/direct) PASS on both hosts. Live daemons use direct I/O exclusively.
The real backend harness covers multiroot identity/locks, quota, large/uncached
segments, concurrent same-range operations, conditional/inode GC, restart,
write/map crash boundaries, sync/punch failure and corrupt mappings/headers/
payloads. Shared storage durability/GC/overwrite native tests PASS on both hosts.
Intel1 ASan/UBSan direct container harness PASS with leak detection and halt-on-
error; the included backend/store code is instrumented, linked common library
and writer objects are the normal build, so this is not whole-program sanitizer
coverage.

Initial runtime4b905a48 mounted full core:216/216 PASS per host. Isolated peer
suite:67/67 PASS per host. An initial peer invocation shared a parent across
hosts and failed with conflicting fixtures; those results are confounded and
not attributed to the backend. The committed controller uses distinct parents.
Mmap-private PASS on both hosts; shared writable mmap SKIP on both with ENODEV,
a FUSE capability limitation. `.efs`, expiry and packing matrices are excluded.

Initial seven-client/jobs16 core:1484/1512 PASS,28 FAIL,0 SKIP/NOTRUN. All failures
were original30-second deadlines: deep60 and deep100 and crazy directories on
all7, crazy file names on4 and repeated-mtime writes on3. Cases with75/180-second
decorator budgets retain those budgets. Do not raise budgets or count reduced-
load results as full-load acceptance. Runtime59180db0 repeated full-load result:1490/1512 PASS,22 FAIL,0 SKIP/NOTRUN. Deep60/deep100/crazy directories failed on all7; crazy filenames on intel1 client2. The segment sync fix does not close these timeouts. Quiet final core216/216 and peer67/67 PASS per host. Two-host jobs1 deep60/deep100 diagnostic passes at unchanged30-second budgets (7.65/7.90s including cleanup). This establishes load dependence, not a causal fix. A bounded64-frame Raft slice experiment is pending on runtimee6757b3c.

Lifecycle:256 files of512KiB, four workers, then30 seconds of complete hash
checking reads and30 seconds of durable128KiB overwrites. Create/read/churn and
complete warm verification PASS. Read completed4683 operations at77.981MiB/s;
churn2473 operations at10.293MiB/s; errors0. All256 hashes verified after daemon
restart on59180db0. These are observations, not a matched files/container
comparison. Two-host tiny churn also PASS:100 files and300 durable generations per host, sizes1/17/128/512/65536 bytes, four workers per host. After the corrected full stop/rebuild/restart, all200 deterministic payloads verified and deleted PASS on e6757b3c; the256-file lifecycle hashes also verified again. Final lifecycle deletion/reclamation remains pending.

### Reproduction and evidence

Use `scripts/intel_container_node.py` on each host for manifest-checked build,
start, mount, run, inventory and graceful stop. Build accepts only a full source
SHA and matching `devel-container-inputs.json`. Produce the archive from tracked
regular files under include/src/deps/tests/scripts plus Makefile and AGENTS.md
from a clean checkout of that SHA; the manifest is a JSON dictionary mapping
each relative file path to its SHA256. Extract the same archive on both hosts
and copy that manifest to each private base. This is reproducible from Git,
without any retained run artifacts. Initialize a fresh private export
once; never mkfs a retained fixture for a restart. Use
`scripts/intel_container_qualification.py GATE --output /absolute/outside-git`
for core, peer, mmap, concurrent, lifecycle-create/read/churn/verify/cold/delete,
and tiny-create/cold. Each run now uses a unique evidence/namespace suffix.
The two-host directory diagnostic uses jobs1 and the original30-second budget.
Native commands are `./tests/test_store_containers --direct-only`,
`./tests/test_store_containers`, `./tests/test_store_durability`,
`./tests/test_store_gc` and `./tests/test_store_overwrite`; native outer deadlines
are180 seconds for containers and90 for each shared test.

Cold lifecycle sequence: create/read/churn/verify; the controller restart gate
(which drains both hosts before stopping either daemon pair); lifecycle-cold then
lifecycle-delete. Tiny sequence: tiny-create on both, the controller restart gate,
then tiny-cold on both (which verifies deterministic bytes and deletes its own
fixtures). Inventory excludes reserved metadata table fragments. Require zero
ordinary data records and zero allocated container blocks after all owned test
namespace cleanup; preserve any failed reclamation finding.

Raw evidence is outside Git at
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/container-20261012`, and
`/data1/efs/bench/container-3173be61-20261012/artifacts` on each host. Local
controller summaries retain the initial run outcomes; the initial remote
concurrent filenames were reused by the final rerun, so initial per-case TSVs
are not claimed immutable. New controller suffixes prevent future collisions.
Reproduction does not depend on private captures. No raw logs or payloads are
committed.


Private restart-control defect: `os.path.ismount` could miss FUSE mounts when
root stat failed. This left Intel2 clients running across the first rebuild.
Actual PID/executable checks caught them; they were explicitly drained and
remounted before59180db0 qualification. A later two-host stop/build attempt
stopped one host before the other host drained, again exposing disconnected
mount detection. Previous compatible private daemons were restored, all retained
clients drained successfully, and only then were both daemon pairs stopped.
No forced-discard was used. The helper now reads `/proc/self/mountinfo`, refuses
to reuse a mounted old executable, and refuses local daemon stop with owned
mounts present. The controller's `restart` gate waits for BOTH hosts at every
phase before proceeding. Earlier shorthand claims of all-client graceful stop
are superseded by these verified drains. Cold bytes passed after the corrected
first restart; tiny-file cold verification is pending after the second.


Bounded64-frame experiment e6757b3c: normal11 selected Raft/read/flow/durable
native extractors PASS on Intel1; affected5 ASan/UBSan extractors PASS. Expanded
inbox regression begins with200 queued frames and appends to256, exercising
retained FIFO tails even with larger slices; durability credit accounting is
unchanged. Direct native container harness PASS on both hosts. Full seven-client
original-budget acceptance remains running; no tuning acceptance is claimed.


64-frame result:1491/1512 PASS,21 FAIL (20 original30-second timeouts and one
concurrent-write/readdir EIO on intel2 client0),0 SKIP/NOTRUN. Cluster remained4
up with matching versions and usage below4G/node; no quota exhaustion or daemon
corruption/sync error was established. The experiment was reverted by92ef337f;
32-frame scheduling remains. No causal attribution of EIO to batching is made.

A separate READDIR page error-classification defect was reproduced: every
non-NOT_FOUND listing-page failure was returned as EIO, including BUSY,
NOT_PRIMARY, NOMEM and ACCES. **fc2d20153002e8a822089f790a2d4aac5b0731b2** uses
the same errno mapping as the preceding parent-stat/OPENDIR checks. Genuine
IO/NET/NO_QUORUM failures still return EIO. It does not retry an expired proof,
serve stale rows, or turn a failed operation into success. The production
callback regression fails before and passes after normally and under ASan/
UBSan. This establishes the classifier bug, not that BUSY caused the observed
loaded EIO. Full-load acceptance still requires successful operations.

Final runtime fc2d2015 preserves the segment fix, strengthened inbox regression,
correct mount detection and cluster-wide restart barriers;32-frame Raft policy
is restored. Manifest780 inputs MATCH on both hosts. Native container/direct,
default buffered/direct control, shared durability/GC/overwrite PASS on both;
Intel1 selected15 authority/Raft/readdir native extractors and2 affected
sanitizers PASS. Final direct container ASan/UBSan PASS. Final original-budget seven-client core:1491/1512 PASS,21 FAIL,0 SKIP/NOTRUN,207–226 seconds including cleanup. All failures are30-second timeouts for deep60, deep100 and crazy directories, on all7 clients. Concurrent write/readdir passed on all7; no EIO or EBUSY was observed in this final run. The earlier EIO remains unexplained, and these results do not close W100. Final quiet core/peer, mmap, lifecycle and physical-GC results follow below.


| Finalfc2d2015 binary | intel1 SHA256 | intel2 SHA256 |
| --- | --- | --- |
| efsd | dbfdd7268acf868d45a48e7b56db991e3f323dedc12999547cdf17bcc9f43f7a | b123f064f1d8489d5f719dc566b7f71d941a643c879053a92a6bc51776aad1f8 |
| efs-fuse | 9e6efd2d43198a460183ab81403dfaf6de2ba866befbdfc5703cf53e088c3751 | f1371c4dc3a99851739b272d1e7b5b7b71e96a534937b33f9e02c02ba975be86 |
| efs-mgmt | ef503fecdc8ab794b2433e9a507dbef86d17530fa286d75c7a058c993c48231d | 62f244fd508e25ffa0919a7a86e382883d13fd4c7905c1493b060149276bc70d |
| efs-query | c8f303c590f8fde415d94cacbd7d0d9590ae747ce53921294c8eae2fa0df5de8 | 494eb984a2da0dfae7591dbc985cf5d4fccdd9f7062c93d54b278238c433537a |


Final quietfc2d2015:216/216 core per host PASS (83.56/84.74s),67/67 peer per
host PASS (45.84/47.44s). Mmap-private PASS per host, shared writable mmap ENODEV
SKIP per host. Retained old lifecycle hashes and deletion PASS. Fresh final-
source create/read30s/churn30s/warm hashes PASS, then100 tiny files/300 durable
generations per host PASS. All workload flags remain unchanged.

The first global reclaim observer was ABORTED (exit143), because a fresh
workload was started before it finished; no180-second verdict is claimed from
that run. Pre-overlap samples showed substantial ordinary GC backlog while the
private namespace was empty. The controller now rejects a reclaim setup unless
the private export is kernel-mounted and empty on both hosts, and checks that
precondition at every sample. Final acceptance is the later quiescent oracle.

The first controller cold remount failed setup on both hosts: clients launched
before recovery opened the daemon listeners, reporting no bootstrap discovery.
Daemons remained healthy; no byte-integrity failure was established. Helper
ba7acebe waits up to60s for both local listeners, and up to40s for kernel mount
plus serving stat; restart start-stage timeout is90s. It writes owned PID
records before readiness waits so failure retains cleanup ownership. These are
setup bounds; POSIX case budgets were not increased. Corrected complete restart
and cold hash results are recorded below. Runtime binaries remainfc2d2015;
subsequent commits change qualification controls and this report only.


Corrected controller restart: all8 two-host phase actions PASS, including both
local-listener waits and7 serving mounts. Final tiny cold hashes/deletion PASS
on both hosts (200 files total), lifecycle256-file cold hashes and deletion
PASS. Native/cold corruption checks found no data-integrity failure.

**Final physical reclamation gate FAIL:** original180-second budget, observer
returned181.56s after its last sweep. Last ordinary mapping counts n1=10586,
n2=10428,n3=10445,n4=10560; allocated container bytes737218560,724721664,
727416832,733618176. The private namespace was mounted and empty at every
sample. These are real EFSCMAP1 ordinary inode records, not reserved metadata
misclassification. GC counters report progress and no delete/mismatch/sweep/
reap/ack/scan errors. This is an unresolved reclamation latency gate; do not
label later convergence as satisfying180 seconds or bypass authority to clear
raw files. No GC pacing or expiry semantics were changed.

Existing daemon observation lines show REAP dominates quiescent leader passes:
about1.70s of1.80s, including1.15s physical inode fencing,0.40s lane sweeps,
0.15s completion proposals. The implementation already pipelines independent
node fences, so adding parallel fanout without reading it would not fix this.
These aggregate phase samples are diagnostic, not a proven root cause of all
namespace timeouts or a fixed GC defect. Follow-up must preserve durable inode-
death certificates, fragment checksums and fresh metadata authority.

Both hosts are Intel Core Ultra7 256V,8 CPUs, with two approximately954G NVMe
devices each. intel1 /data1 uses nvme0n1, /home uses an LVM volume on nvme1n1;
intel2 reverses those device names. /data1 and /home are XFS; the70G root LV
shares the system NVMe but is not the /data2/efs backing filesystem.

Final deployment remains the isolated4-daemonfc2d2015 container cluster on
27432/27433,4G/node, with one cross-host private mnt0 per host; test-only extra
clients are gracefully retired. Background GC continues on the retained owned
stores. Existing production processes, ports and /data1/efs/mnt are checked
separately. No push, merge, tag or production promotion. **Qualification FAIL**
until W100's21 original-budget namespace timeouts and the physical-GC deadline
failure have verified fixes. Containers remain experimental.

Final audit: bidirectional peer TCP connections to both27432/27433 PASS;
all780 deployed build inputs match the frozen manifest on each host. Actual
/proc executable hashes match the recorded final binaries. All four private
daemons and both retained clients use TCP with commands/server commands and
packed writes set to0; `.efs` is absent from both serving private mounts.
Production daemon PIDs5945/5953 and10086/10094, clients6602/10331,
400G quotas,17432/17433 and `/data1/efs/mnt` remain in place. Only one
private cross-host client mount remains per host.


## Resumed container-only investigation — candidate 73b1f915

The user authorized continued repair of the directory and physical reclamation
failures with the same Intel container/TCP/direct configuration and disabled
commands, scratch expiry dates and packing. Candidate
`73b1f915f52088e7ebc57e478b564cd4d664ea8f` adds a bounded physical-directory
ancestry cache scoped to container deployments, including their buffered
metadata fragments. A cache entry retains a directory FD to prevent inode
reuse; only a successful full ancestry barrier admits it. Each operation still
syncs its containing directory. Removal/recreation cannot reuse the pinned
identity, failed barriers do not admit hints, and eviction falls back to full
barriers. Files-engine behavior, RPC budgets, admission and authority are
unchanged. No cache hit is used as proof of namespace or payload authority.

The extended actual-backend native regression PASS on both Intel hosts,
including failed directory-sync retries, removal/recreation, multi-root,
concurrent same-range writes, quota, cold recovery, inode GC and corruption.
The previous fc2d2015 implementation aborts the new repeated-write barrier
assertion (negative-control exit134). Intel1 ASan/UBSan direct backend regression
PASS, leak detection enabled; linked common/writer objects are not all
instrumented. Both hosts' native default direct/buffered control plus shared
store durability/GC/overwrite controls PASS. Live runtime remains direct only.

A repeated previous-source seven-client baseline retained14 failures (depth60
and crazy directories on all7 clients); depth100 passed this repeat. This is
not a clean performance comparison: staging compilation/native checks overlapped
part of that baseline. Original21 failures and all original30-second budgets
remain retained. One private candidate start raced the previous Intel2 graceful
stop and was refused by the exclusive storage lock. The pending stop finished;
startup was retried only after source proof, and all4 nodes then reported
73b1f915. The committed readiness-aware helper is installed on both hosts.
Candidate live acceptance and fresh physical reclamation are pending below.

Retained-backlog diagnostic on73b1f915 reached zero ordinary mapping records
and zero allocated container bytes on all4 nodes at160.48s (observer return
160.49s), within the original180s. Initial observed records per node were
19820,19685,19624,19528. This is a retained-backlog diagnostic, not fresh
qualification: recovery and some draining preceded the observer, and native
controls overlapped part of it. Fresh seven-client workload and subsequent
quiescent physical gate are required before closing the prior failure.


Cache-only73b1f915 fresh full gate:1472/1512 PASS,40 FAIL,0 SKIP/NOTRUN. Additional timeout cases appeared; the ancestry cache is
not an established latency remedy. Its later post-suite observer reached zero
at97.02s, but started several minutes after cleanup while the next candidate
was being prepared, with overlapping staging/native work. This delayed,
confounded observation does not close the physical deadline gate.

Candidate `eed87ad926dfcbdb3daf38ceab962f2d16c364c4` adds bounded container-only
inode retirement batches under the existing exclusive inode guard and durable
death fence. The existing128-object budget is retained. A snapshot avoids
readdir/rename double visits. Each validated mapping is renamed without
replacement to `.tmp.gc`; the containing directory is made durable before
punching. All touched segments receive durable barriers before retirement names
are removed and synced; only then are ranges reusable and quota/counters
updated. In-process punch/barrier failures retain retryable mapping identities.
Startup reconciles non-authoritative temporaries as before. Failed final
unlink/barrier outcomes conservatively quarantine allocation until startup.
No expiry, age-based reclamation, authority relaxation or deadline changes.
The ancestry hint budget is now128 descriptors across the fixed target set.

Both Intel hosts' direct native tests and default direct/buffered controls plus
shared durability/GC/overwrite controls PASS. Intel1 ASan/UBSan direct PASS
with the same linked-object instrumentation limit. Two preceding-code native
negative controls abort:70-fragment collection requires70 segment barriers
instead of1, and failed segment-barrier retry cannot fully reconcile quota.
Both pass on the candidate. Native fault tests cover segment/namespace/punch
failure and in-process retry; crash boundaries after retirement rename,
after punching, and after retirement unlink recover safely with persistent
inode death and no late PUT admission. The native same-process restart helper
invalidates its test-only thread-local death epoch to model an actual new
process; production lifetime/epoch behavior is unchanged.

The four-node candidate was cleanly deployed with two-host drain/stop/start/
mount phase barriers;780 inputs per host match the manifest and all live
versions agree. Fresh seven-client qualification is underway with no staging
build or native workload overlap. Quiescent physical observation must follow
that fresh cleanup before any additional workload or restart.

The two-writer eed87ad9 full gate also FAILS:1462/1512 PASS,50 timeouts,
0 SKIP/NOTRUN. Ten seconds of syscall tracing on node2 occurred only after
failures were already reported; this diagnostic overlap prevents a clean
throughput comparison, and no passing acceptance is claimed. Its unchanged
physical gate starts immediately after all7 controllers finish cleanup, with
no subsequent workload, build, native test or daemon restart overlapping.
Initial records24999,25093,24949,25022/node, allocated bytes1740857344,
1746780160,1737371648,1741901824. Verdict follows below.

The storage trace retains2037 completed segment writes in10s, with2037 segment
barriers and4107 mapping-file/directory barriers. Mean segment and mapping
barrier durations were about2ms; traced latency is instrumented diagnostic
context. Uninstrumented cumulative node2 PUT duration around60ms versus disk
service around8ms suggests queuing/other handoff work, not device service
alone. This is not proof that every directory timeout has the same cause.
Helper acee1609 makes writer slots explicit, with8 per private daemon for the
next isolated configuration trial instead of the fixture's hard-coded2. Core
case concurrency, all original30/75/180s budgets, quotas, transport, I/O and
feature flags are unchanged. Runtime source and binary hashes remain eed87ad9.

Fresh eed87ad9 two-writer/1000ms physical gate FAIL: observer180.009s,
last179.920s, ordinary records287,319,321,283 and allocated container bytes
19918848,22077440,22290432,19570688. No deadline extension or later-convergence
PASS is claimed. This is materially different workload history from the earlier
10k residual; the50 namespace timeouts prevent a matched throughput conclusion.

Private fixture e25b6fce sets the existing `EFS_GC_BACKLOG_IDLE_MS=20` option
and8 writer slots per daemon. The progress selector requires a capped pass and
an increased physical-removal counter, retains the128-object/32-marker/200ms
work budgets and20ms fair yield, and keeps1000ms idle/blocked backoff. This is
explicit container-fixture configuration, not a files-engine default or new
expiry/reclamation-authority rule. A fresh full workload and immediate180s
physical gate must pass in the same configuration. Reproduce the previous
configuration with helper start `--writers 2 --gc-backlog-idle-ms 1000`, using
the same cluster-wide drain/stop/start/mount phase barriers.


### Fresh eight-writer/20ms backlog qualification and diagnostics

Runtime `eed87ad926dfcbdb3daf38ceab962f2d16c364c4`, fixture `e25b6fce`,
seven concurrent exports at jobs16: **1448/1512 PASS,64 FAIL,0 SKIP**.
The same nine cases failed on all seven clients: deep60/deep100, many-files,
crazy roundtrip/directories, concurrent create/append/create-unlink, and
mtime-many-writes; write/readdir failed on one. Original30/75/180s case
budgets remain unchanged. These failures do not close the original21-timeout
blocker and show that writer/backlog configuration alone is insufficient.

The controller started the unchanged180s physical observer immediately after
all seven workloads and cleanup exited, with no competing build, native test,
restart or workload during observation. Initial ordinary mappings were21366,
19792,21289,19703 on n1..n4. **PASS at103.741s:** every node reached zero
ordinary mappings and zero allocated container body bytes. All observed
private exports were mounted and empty. This is a fresh, in-limit physical
reclamation result, unlike delayed/confounded earlier observations.

Diagnostic-only `b4333b54` enables existing RPC/read/Raft counters. Its loaded
run and bounded20s CPU recording are not acceptance/performance comparators.
Counters locate most LOOKUP latency in fresh parent ReadIndex authority:
n2 about17.0ms versus roughly0.1ms KV access; leader proofs about7–8ms,
remote proofs about16.5ms. Client pool checkout is negligible and measured
transport round trips are submillisecond. No cache expiry, stale proof reuse,
authority shortcut or test deadline change is justified by those counters.

Existing opt-in bounded `EFS_RAFT_GROUP_DRAIN=1` was tried with the same
runtime/eight writers/20ms configuration after native inbox ordering,
receive-overlap and publication controls passed. Full mounted run:
**1447/1512 PASS,65 FAIL** (same nine cases all seven; write/readdir two).
The trial is rejected as a timeout remedy and the temporary helper switch was
removed; the subsequent candidate restores default group-drain configuration.

Container-only routing candidate `76bb437657204dd6d2c63c379687e472aa6aaf7c`
redirects LOOKUP/GETATTR from a follower to a known inode/parent leader only
when committed membership proves that leader hosts both metadata groups.
Unknown, single-group, removed and self leaders preserve existing paths;
ordinary files remain unchanged. Every receiving RPC still establishes its
own fresh ReadIndex and discovers/rechecks dentry, inode and lane authority.
Native membership/self/unknown/files controls and per-RPC freshness checks
PASS. This is a candidate, not a loaded acceptance claim.


### Routing candidate mounted result (76bb4376)

The exact780-input manifest and correct full-SHA build metadata match on both
hosts; native complete buffered/direct container, shared durability/GC/
overwrite, routing and per-RPC freshness controls PASS. The initial build used
a non-repository make variable and reported an unknown version; it was discarded
and clean-rebuilt with `EFS_GIT_ID`, `EFS_GIT_DESCRIBE`, and `EFS_GIT_BRANCH`
before any deployment. Only those corrected binaries were launched.

Fresh seven-client full gate: **1491/1512 PASS,21 FAIL,0 SKIP/NOTRUN**:
all seven deep60, deep100 and crazy-directory cases exceed unchanged30s.
This is the original blocker set, not closure. Observed full-run exit times
including cleanup131–144s are source/history-specific, not a matched
performance or throughput claim. Bounded group drain and diagnostic flags
are off; eight writers and20ms capped-progress GC yield remain explicit.

Immediate physical gate **FAIL at180.010s**. Initial ordinary mappings:
28075,28095,28220,28129; final179.168s:1395,1360,1420,1401, with roughly
95–99MB container allocation per node. The earlier103.741s pass on eed87ad9
remains valid for its smaller21366/19792/21289/19703 backlog; it does not
qualify this source or larger workload. No deadline extension, delayed observer,
restart or concurrent build/test was used to relabel this failed gate.


### Shared barriers and pinned shared ancestry (7761affa candidate)

`7761affaed630fb96c81cde5961af1111d039144` changes only the experimental
container backend and its actual-backend native regression. Runtime segment
barriers detach a bounded-by-caller cohort after completed writes/punches and
before starting fsync. Later arrivals need their next barrier; there is no
assembly timer, background task, ACK shortcut or cross-request read coverage.
Each stack waiter receives its own cohort result before returning. A failed
cohort cannot inherit later success, while a new retry remains permitted.
Uncached segments, mapping-file publication, directory barriers, busy-bit
retirement and startup reconciliation retain their prior durability rules.

The ancestry cache now retains all successfully sealed ancestors (at most16
transient pins per call) and uses a fixed LRU descriptor budget of128 across
the target set. New siblings can stop after syncing an already-sealed shared
ancestor; their containing/newly changed directories still sync. Successful
full ancestry barriers are required before inserting any pins. Failure,
removed/recreated directories and inode reuse cannot create a false hit.
Ordinary files do not use this container cache or segment barrier path.

Native complete buffered/direct container harness PASS on both Intel hosts.
Deterministic16-caller barrier test observes exactly two fsyncs: one captured
before the15 later completed writes, then one covering those15; verifies
independent failed/late-success results, full-cohort failure, new retry and
actual bytes. Sibling/cache-churn regression verifies3 versus repeated-root
barriers, a hot shared parent across more leaves than cache capacity, and
bounded retained descriptors. Existing quota, GC, crash and corruption checks
remain intact. Independent-fsync negative control aborts134 at the bounded
pending-cohort assertion; single-leaf-cache negative control aborts134 at the
sibling ancestry assertion. Intel1 direct ASan/UBSan with leak detection and
halt-on-error PASS (backend/store instrumented, linked common/writer normal).
Native success is not mounted-load or physical-deadline acceptance; exact
committed candidate builds and live reruns follow.


### Loaded cohort experiment retired; fresh-probe candidate (df29de98)

Exact `7761affa` seven-client core: **1449/1512 PASS,63 FAIL**. All seven
clients timeout on the same nine cases (deep60/deep100, many-files, crazy
roundtrip/directories, concurrent create/append/create-unlink and many-write
mtime); write/readdir passes. Immediate physical reclamation **PASS106.287s**
(last zero snapshot106.263s), initial mappings22388/22353/22394/22435,
all four nodes zero ordinary mappings and zero allocated body bytes within the
unchanged180s observer, with mounted empty exports and no overlapping work.
The observed load regression is not a successful directory fix. Source/workload
histories differ, so this does not isolate a causal performance attribution.

`b2379df2` retires the new segment-fsync cohort implementation and its feature-
specific native tests. Ordinary independent segment barriers are restored;
per-inode multi-fragment GC still retains its verified single-segment barrier
batch. Shared pinned ancestry/LRU correction and sibling/churn/fault/bounds
regressions remain. Complete native container buffered/direct and shared
storage durability/GC/overwrite controls PASS on both hosts after restoration.
The experiment and its passed/failed evidence remain in Git history above.

`df29de986661fbc81243cf015285499a297e748c` changes only containers' fresh-
context empty read-probe host scheduling. If the existing Raft mutex is
available, the unchanged core may send a current-term heartbeat vote while
queued work exists. Its known-leader, durable-prefix, boot, term, membership,
storage-failure and unfinished-log filters all remain. Such a request ALWAYS
returns to the ordered inbox when work is pending, even if the core reported
it fully accounted; no commit/apply, entry durability, transport credits,
configuration or replication bookkeeping can be consumed/reordered early.
Zero-context heartbeats and ordinary files retain their former pending-work
path. Fresh RPC admission/round and local applied-prefix barriers remain.
A queued higher-term/config frame has made no durable promise; processing it
still invalidates later old-term proofs normally. No age-based authority is used.

Actual Raft/disk/simulator tests and actual host helper, admitted-completion,
publication and pair regressions PASS on both hosts. Host negative control
using the former blocking fails at the new pending-probe assertion; Intel1
ASan/UBSan actual host extractor PASS. Exact-source live qualification is still
required; this is a candidate, not closure of the21 directory timeouts.


### Fresh-probe loaded result and distinct-ACK correction

Exact `df29de986661fbc81243cf015285499a297e748c`, unchanged private
4-node Intel containers/TCP/direct configuration, 8 writers/node, GC progress
yield20ms, four plus three opposite-host mounts, 4G/node, commands/`.efs`,
expiry-date feature and packed writes disabled: seven-client core **1451/1512
PASS,61 FAIL**. Each client runs216 core cases at jobs16 with original
case deadlines; use `intel_container_qualification.py concurrent --run-id
df29-full --output <external-dir>`. Deep60/deep100, many-files, crazy
roundtrip/directories, concurrent creates and appends fail on all7; mtime6,
create/unlink4 and write/readdir2 also fail. Subsequent two-client core
**430/432 PASS,2 FAIL**, both deep60 timeouts. Controller stops at this failed
gate; peer/mmap and later lifecycle/tiny/cold phases were NOTRUN on this SHA.

Immediate unchanged180s physical observer **PASS135.966s**: initial ordinary
mappings23139/22628/23251/22525 and body allocation
1589133312/1571684352/1597583360/1565351936 bytes become zero on all four
nodes while exports stay mounted and empty; no competing workload/build
overlap. Raw evidence remains outside Git at local
`artifacts/container-20261012/df29-qualification`, and private host fixture
`artifacts/`. Binary identities are those of the exact df29 build recorded
in the external proof; this result does not qualify another source.

`5ce632dd9f1dca3fa8c06b3ce7af70913414df99` corrects container outbox
coalescing: read-probe votes cannot overwrite entry-retirement ACKs, and
different term/boot/read context/status/matched prefix retain separate frames
in the existing bounded queue. Only semantically identical frames coalesce.
Ordinary-file scheduling and vote-at-capacity eviction remain unchanged.
Actual production-helper regression verifies both request/reply classes,
old/new contexts, prefix/status/boot/term, byte bounds and buffer ownership.
Old-coalescer negative control aborts at the probe-overwrites-progress
assertion; local ASan/UBSan PASS. Both Intel host helper/outbox/inbox/flow
and actual Raft/disk/simulator controls PASS. Exact780-input build and live
qualification follow; native proof alone does not close directory timeouts.


### Distinct-ACK qualification and redundant-GC correction

Exact `5ce632dd9f1dca3fa8c06b3ce7af70913414df99`, same four-node
containers/TCP/direct fixture and seven-client original-deadline core:
**1478/1512 PASS,34 FAIL**. Deep60/deep100/crazy-directories fail on all7;
crazy roundtrip6, many-files4 and concurrent creates3 also fail. Full runs
finish153.879–172.228s. Immediate physical180s observer **FAIL**: initial
ordinary mappings28033/27938/28026/28023, last179.691s
3422/3434/3426/3427 with238071808/238907392/238538752/238481408 allocated
body bytes. No competing build/workload overlapped this observer. It is not
closed by df29's earlier smaller-backlog pass. The controller stops here;
quiet/peer/mmap/lifecycle/tiny/cold gates are NOTRUN on this SHA. Source780
manifest and both host binary/version proofs are retained externally at
`artifacts/container-20261012/outbox5ce-deploy`; raw qualification at
`artifacts/container-20261012/outbox5ce-qualification`.

After the backlog drained, an explicitly diagnostic single-mount jobs1
`--filter dir_deep_nesting` run passes both original30s cases in5.0s; existing
nonblocking phase markers show deep60 setup/create/leaf/read/recursive
cleanup/absence all completing. This is not seven-client acceptance.

`ae5a177831b26290fd34e63b4e5daccf9b4eabe0` removes two sources of
container-only redundant GC work. Death-page publication uses the pinned
ancestry proof, with every completed/failed barrier still going through GC
syscall accounting. A known sealed containing directory still syncs on every
retry; newly created/unknown ancestors all sync before cache admission. The
ordinary-file three-barrier path remains unchanged. Native regression
requires one accounted repeat-fence barrier rather than three, rejects a
failed callback without caching ancestry, and verifies retry/root sealing.
The old path aborts134 at the repeat-fence assertion.

A partial reap no longer proposes a durable empty-lane deletion when a fresh
request-local group ReadIndex proves both the lane row absent and the source
chunk prefix empty. An unfenced absence hint cannot authorize skipping:
both checks repeat after the proof, and any error/new row/remaining chunks
retain normal work. Coverage is reset for each inode reap attempt, never
shared across requests/time; physical all-node inode fencing and final reap
retirement remain mandatory. Ordinary files retain original processing.
The production-helper regression tests failed authority, newly visible rows,
remaining chunks, independent group coverage and fresh subsequent attempts;
ignoring a failed proof aborts at its refusal assertion.

Complete container buffered/direct, metadata apply, GC verdict propagation
and storage controls PASS on both Intel hosts. Intel1 direct ASan/UBSan with
leak detection/halt-on-error PASS (backend/store instrumented, common/writer
normal); empty-lane host extractor sanitizer PASS locally. Exact781-input
builds and loaded qualification follow. No successful native/diagnostic run
closes the outstanding full directory or physical deadline gates.


### Redundant-GC loaded qualification and queued-vote candidate

Exact `ae5a177831b26290fd34e63b4e5daccf9b4eabe0`, same preserved private
Intel fixture, containers/TCP/direct,8 writers/node, GC progress yield20ms,
4G/node, four plus three opposite-host mounts, disabled commands/`.efs`,
expiry-date feature and packed writes, unchanged test deadlines:

| Gate | Result |
| --- | --- |
| Seven clients,216 core cases each,jobs16 | **1450/1512 PASS,62 FAIL** |
| Immediate physical reclamation,180s | **PASS100.630s**; zero snapshot100.619s |
| Two clients core216 each,jobs16 | **430/432 PASS,2 FAIL**, both deep60 |
| Peer coherence,67 cases/host | **134/134 PASS** |
| mmap extras | Private2 PASS; shared writable2 ENODEV SKIP |
| Lifecycle256 files,512KiB,4 workers;30s read and30s durable churn | Create/read/churn/warm verify **PASS** |
| Tiny files100/host,300 total durable generations/host,4 workers | **PASS** both hosts,8.970/9.004s |
| Cluster-wide graceful cold restart; lifecycle and tiny hashes | **PASS**; both tiny sets verified/deleted |
| Lifecycle deletion; immediate final physical observer | **PASS11.462s**, all ordinary mappings/body allocation zero |

Full-load failures: deep60/deep100, many-files, crazy roundtrip/directories,
concurrent creates and appends on all7; create/unlink6, mtime6, write/readdir1.
Initial immediate-observer mappings23064/22359/23152/22431 and allocated
body bytes1573298176/1542881280/1579315200/1548599296 become zero. No
competing build/workload overlaps either physical observer. Different failed
full-run workloads produce different backlog sizes; this does not establish
a causal performance ratio or close the previously larger backlog by proxy.

Reproduce with `intel_container_qualification.py <gate> --run-id <unique>
--output <external-dir>` in this order: concurrent,reclaim,core,peer,mmap,
lifecycle-create/read/churn/verify,tiny-create,restart,lifecycle-cold,
tiny-cold,lifecycle-delete,reclaim. For this evidence controller, independent
concurrent/reclaim/core/peer/mmap failures are recorded while later functional
gates continue; dependent lifecycle/tiny/restart failures stop progression.
Raw output is external `artifacts/container-20261012/gcae5-qualification`;
source781 and version/hash proofs at `gcae5-deploy`. The preserved stores,
metadata history and original deadlines are retained; no reset/deadline
extension changes acceptance. Both hosts build with exact `EFS_GIT_ID`,
`EFS_GIT_DESCRIBE` and `EFS_GIT_BRANCH=devel-container` as recorded earlier.

| Host | efsd SHA256 | efs-fuse SHA256 |
| --- | --- | --- |
| intel1 | `4706dd1408df70ee31fc84bf6ee366a31aeba903253a5d0f46d662a96bf1792d` | `07cbca82073fe086a19dc6444d3c7b5d1d1e68f2a9ec592c3221cebf1a6b6065` |
| intel2 | `bedcf1721b55a39c491b6138125d56ec79b5394de560a8970709b89707ec8e0c` | `7f4b2a454744221a3ec9b87fb06bf18250e522454ac50566609d71378462e189` |

`c3bcffd1a91c9c5dac5f4f730d2f5b03f6ebcc78` addresses eligible queued
container read votes whose network trylock previously failed. The sole pump
snapshots at most256 existing80-byte fresh-context request/reply headers
before durable inbox work and invokes the unchanged core term/boot/voter/
known-leader/persisted-prefix filters. Every frame still enters normal receive
in original order, with credits and commit/apply/replication work intact. A
per-frame note prevents repeatedly granting the same early vote; enqueue
resets it, FIFO movement preserves it, and failed core checks retry normally.
No new queue, assembly delay, stale proof reuse or clock authority is added.
Ordinary files and zero-context/data/config/vote frames retain prior handling.
The empty-lane GC hint now runs before normal selection to avoid scanning
already-cleared lanes twice; its fresh proof and post-proof checks remain.

Actual helper, bounded headers, refused/retried votes, note reset, FIFO and
credit ownership regressions PASS on both Intel hosts, along with actual
Raft/disk/simulator and host publication/admission/pair controls. Old no-op
pump negative control aborts at the accepted-vote assertion; local extractor
ASan/UBSan PASS. Exact782-input native builds and live qualification follow.
The directory gate is still open; ae5a's cold/physical successes do not qualify
this new runtime.


### Queued read-vote qualification and placement follow-up (2026-10-12)

Exact source `c3bcffd1a91c9c5dac5f4f730d2f5b03f6ebcc78`, 782 matching
tracked build inputs, private preserved four-node Intel stores, TCP/direct,
4G/node, eight writers, 20ms progressing-GC yield; `.efs`, expiry dates and
packing disabled. Seven simultaneous 16-job clients retain all original
30-second directory budgets: **1493/1512 PASS, 19 FAIL**. Deep60 and crazy
directories timed out on all seven clients; deep100 timed out on five.
Immediate 180-second physical reclamation **FAIL** at 180.003s; last sample
179.072s retained 460/461/477/492 mappings and
32038912/32108544/33222656/34271232 allocated container bytes on nodes 1–4.
This does not close either requested loaded gate.

Two-host quiet core **432/432 PASS**; peer **134/134 PASS**; private mmap
PASS, shared-writable mmap ENODEV remains the documented skip. Lifecycle
256 files, 512KiB, four workers, read/churn/warm hashes PASS; tiny-file
300 durable generations per host PASS, followed by globally phased graceful
restart, cold lifecycle and both-host tiny hashes/deletes PASS. Final physical
observer is still running; its verdict will be recorded separately.

Read-only post-restart Raft status shows both metadata groups led by Raft
peer 2 (server node 3, Intel2), with committed voter masks group0=0x7 and
group2=0xe. The next private-fixture candidate enables the existing opt-in
`EFS_METADATA_BALANCE=1`: its actual-voter preference selects server node1
for group0 and node3 for group2, spreading leadership across physical hosts.
`--metadata-balance 0` preserves the previous diagnostic fixture configuration.
The existing two-second settling grace, target durable catch-up refusal,
30-second cross-term backoff and quorum checks remain unchanged. Native
production balancing-policy extraction PASS locally. No production service,
ordinary-file default, test concurrency, deadline or preserved store is changed.
Placement is a hypothesis until fresh loaded and immediate physical tests pass.


### Bounded container read-probe candidate

Balanced exact `be506447328c0b0c8d92af39fbfbfe24aa867b8b` has matching
782 tracked inputs and both-host native backend (buffered/direct), durability,
GC, overwrite, metadata apply, Raft/store/simulation and balancing controls
PASS. Build uses `make -j4 EFS_GIT_ID=<SHA> EFS_GIT_DESCRIBE=<SHA>
EFS_GIT_BRANCH=devel-container`. The initial command used an incorrect target
path and then an unsupported extra backend argument; corrected commands
passed before deployment. No result from either setup error is acceptance.
Verified group0 leader node1/Intel1 and group2 node3/Intel2 before load;
unchanged concurrent gate **1448/1512 PASS,64 FAIL**: eight cases all seven
(deep60/deep100/manyfiles/crazy roundtrip/crazy dirs/concurrent creates/appends/
create-unlink), write-readdir five, mtime three. This placement is not a fix.
Immediate physical and remaining functional gates are running. Prior c3 final
quiet physical observer completed **PASS65.388s**; it does not replace its
failed immediate full-load physical observer.

The next product candidate adds one bounded priority prefix-zero read vote per
peer/group/fresh context alongside a successfully queued original container
AE frame. It uses the existing escape-probe wire class and unchanged core
term/boot/current voter/leader/config guards, request admission and leader
applied ReadIndex barrier. The original frame remains queued, and the vote
class cannot retire a data window or advance replication progress. Queue
count/byte caps and allocation refusal keep the additional probe optional;
context/term/boot tracking suppresses repeats. Ordinary-file outboxes are
unchanged. Actual helper regression, outbox and host-flow controls PASS
locally; helper ASan/UBSan PASS. Removing the helper produces the expected
assertion/SIGABRT negative control. Both-host compilation/native and exact
live qualification remain required before any closure claim.


Balanced be506447 qualification completed: quiet core **430/432 PASS**,
**2 deep60 timeouts**, peer134/134 PASS, private mmap PASS/shared-write
ENODEV skips, lifecycle create/read/churn/warm/cold hashes PASS, both-host
tiny warm/cold hashes and delete PASS; global graceful restart PASS; final
physical reclamation **PASS10.145s**. Immediate post-full reclamation
**PASS114.138s**, all four zero at114.131s, initial mappings
24545/24773/24544/24825 and allocated bodies
1698246656/1720942592/1698455552/1724784640 bytes. This does not
qualify the next source and does not compensate for64 full-load failures.
Both-host exact binary hashes are retained outside Git in
`artifacts/container-20261012/balancebe-deploy/*-proof.log`; Intel1 efsd
SHA256 `36062853bb8f4b6e2b1e22bc29331f0c4a6b7d35f680b7ed28a998f727b9d86d`,
FUSE `b555b6da79b32625561dcb003190a64c7c4a9ca5423bcaaf31cd05813938168c`.

Priority read-probe exact candidate is
`209c1f8b26a22947a90c9b6de53583b20e1a978e` (783 tracked build inputs).
Companion outbox retention includes AE_REQ prev-index/term in bytes44–59,
so a same-context matched-tail probe cannot overwrite a prefix-zero escape
probe. AE_REP match-prefix retention, class/context/term/boot checks and
ordinary-file behavior remain unchanged. Native extraction distinguishes
both request prefix fields and retains vote eviction at capacity. Exact
both-host native compilation/controls are in progress; no live acceptance
claim is made yet. Configuration remains balanced to isolate this product
change against the immediately preceding placement result.


### Dual-group container leader preference candidate

Exact priority-probe source209c1f8b passes both-host affected native controls:
backend buffered/direct, durability/GC/overwrite, metadata apply, Raft/store/
simulation, new probe, outbox, actual Linux sender, inbox, pending votes, read
control/admission/publication/completion/pair and empty-lane proof controls.
783 matching tracked inputs, unchanged private preserved stores/configuration
and split placement node1/group0, node3/group2 were verified before load.
Intel1 efsd SHA256
`b68353e3824e892ec70f7e31453fed5dd9b6ed6e9f762e31488479632f6d4ef2`,
FUSE `24df6f88d69829c88d8e4479249fb6c20162c09bfe2b51dfe954a7c60d3a77d3`.
Unchanged full gate **1447/1512 PASS,65 FAIL**: the same eight cases on
all seven clients, mtime five, write-readdir four. The probe alone does not
close directory performance. Its immediate physical and remaining gates are
still running. Raw evidence stays in external
`artifacts/container-20261012/priority209-{deploy,qualification}`.

The next container-only optional balancing preference intersects the actual
committed voter masks of both metadata groups, then applies the existing
ordinal voter selection. The Intel masks0x7 and0xe intersect0x6: group0
selects node2/Intel1, group2 selects node3/Intel2. Both targets host both
groups, satisfying the existing LOOKUP/GETATTR/MKDIR routing requirement;
node1 was single-group and could not accept that routing shortcut. No
membership, replication quorum, authority proof, two-second grace, durable
target catch-up guard or 30-second handoff backoff is changed. Unknown group
membership postpones optional handoff; disjoint groups use the original
actual-voter preference. Ordinary files keep the original policy. Native
actual helper/policy extraction and ASan/UBSan PASS locally, including
unhosted-but-known masks, unknown/disjoint/all-four masks, ordinary-file
scoping and lag refusal. Original ordinal-only preference fails the new
dual-group assertion/SIGABRT negative control. Both-host native build and
fresh live gates remain pending; no closure claim.


Priority209 qualification completed: immediate full-load physical reclamation
**PASS112.619s**, all four ordinary maps/bodies zero at112.612s; initial
mappings25281/24922/25245/25111 and allocated bodies
1740349440/1725296640/1746358272/1729781760 bytes. Quiet core
**430/432 PASS,2 deep60 timeouts**. Peer134/134 PASS; mmap private
PASS/shared-write ENODEV skips; lifecycle256×512KiB four workers and tiny
300 durable generations per host warm/cold hashes PASS; global graceful
restart and deletes PASS. Final physical observer **PASS10.240s**. Full65
failures remain; this is not directory acceptance or physical evidence for
a newer SHA.

Exact dual-group placement candidate is
`b0ba38194586b853760ea253a97f5d33961e5bb7`, 783 tracked build inputs.
Native build/affected controls are underway on both Intel hosts, and live
validation follows only after both exit successfully. Every preceding
private store and metadata history remains preserved.


### Bounded container paired-proof capacity candidate

Exact dual-group sourceb0ba3819 compiled and passed both-host affected
native controls; 783 matching inputs and node2/group0, node3/group2 dual
leaders verified before unchanged seven-client load. Full **1459/1512 PASS,
53 FAIL**: deep60/deep100/crazy roundtrip/crazy dirs/concurrent creates all
seven, manyfiles five, create-unlink five, mtime five, appends two, write-
readdir one. Immediate ordinary physical reclamation **PASS86.035s**,
all four maps/allocated bodies zero at86.028s. Quiet core FAIL; peer/mmap,
lifecycle warm gates PASS; tiny/cold/final cleanup are continuing.
Intel1 efsd SHA256
`e752c5fd819e32fb433e35487660ed0e6918649f93d7f984aa65c0cd42c04559`,
FUSE `7166b0f89f9ac1eab2c5c872dc94f7fa30ffa9f1dddc4d45848f6ae485e880ba`;
Intel2 efsd `771f922c354e06a1929615ac75d37e5dd1f889b5f5886e7f071ee8d1323006f2`,
FUSE `e2bf145025fe51575a498ddf4e32fdaeae0e44d674865bd4f2b3d4d24f96f724`.
Existing GC timing samples show32-inode reaper passes around746–908ms,
mostly547–653ms physical fencing; they are diagnostic samples, not evidence
that directory failure is caused by a particular GC stage.

The next scoped candidate keeps the existing persistent paired-read design
but initializes32 bounded128KiB helpers for containers (4MiB stack budget),
and keeps eight for ordinary files (1MiB). A saturated eight-slot pool falls
back to serial independently fresh proofs; increasing this resource bound
is a performance hypothesis, not yet a demonstrated cause of all timeouts.
No queue, per-RPC thread creation, proof reuse, new deadline or concurrency
change to the POSIX workload is introduced. Partial/failed initialization
and saturation retain scalar fallback; both proofs finish before a handler
can release its host or stack. Existing EFS_READ_PROF now reports parallel
and scalar counts for future diagnostics; profiling remains off for acceptance.
Native production helper/header controls PASS locally and under ASan/UBSan:
eight ordinary-file and32 container slots, partial/zero initialization, every
real slot simultaneously occupied by a distinct caller/deadline, overflow
fallback, repeated reuse, hard refusal and joined lifetimes. Original eight
container slots fail the capacity assertion/SIGABRT negative control. Local
fresh-round paired admission controls also PASS. Both-host native/live
validation is still required, and earlier physical passes do not qualify it.


Dualb0 qualification completed: quiet core **430/432 PASS,2 deep60
timeouts**; peer134/134 PASS; private mmap PASS/shared-write ENODEV
skips; lifecycle256×512KiB four workers and both-host tiny300 durable
generations warm/cold hashes, global graceful restart and deletes PASS.
Final physical observer **PASS11.326s**. Initial immediate full-load
mappings24726/25158/24666/25145 and bodies
1716420608/1748897792/1714409472/1747206144 bytes drained to all-zero
at86.028s (observer PASS86.035s). Full53 failures still leave directory
acceptance open.

Exact capacity candidate is
`c23f9687deb87d2ee232ede9d59bbeb3fd58a561`, 783 tracked inputs.
Native builds/affected controls are now running on both Intel hosts after
the preceding qualification completed. Profiling remains off; no overlapping
remote native work will run during live qualification or physical observers.


Capacity c23 qualification completed incompletely: **1451 PASS,54 FAIL,
7 NOTRUN /1512**. One runner stopped after it could not confirm worker-group
termination; its owned test namespace remained. Immediate physical gate was
NOTRUN because the private export was not empty, not a reclamation pass.
Quiet core430/432 PASS with two deep60 timeouts. Remaining peer/mmap/lifecycle/
tiny/restart/cold gates were cancelled; no success from an older SHA qualifies
this candidate. All four private daemons stayed alive and Raft progressed.
After workers exited, only the explicitly owned leftover test directory was
removed; stores and metadata history were retained. Production remains separate.

Normal POSIX timeout cleanup now uses the existing bounded shutdown state
inspection: zombie-only groups are stopped; live, D-state and unknown groups
remain failures. Original case deadlines and TERM/KILL grace are unchanged.
Focused actual-helper positive/negative controls and full local harness self-test
PASS. The observed live failure was not proven zombie-only. Reclamation now
reports an invalid empty-export precondition as NOTRUN/exit2, so controllers
can stop rather than continue with an invalid fixture.

Next container placement candidate colocates both metadata leaders on the
same committed dual-group voter when such an intersection exists. Intel
intersection nodes2/3 prefers node3; disjoint/unknown membership retains
fallback/refusal, and files retain their original policy. Existing settling,
catch-up, backoff, quorum and independently fresh read checks remain mandatory.
This is a hypothesis based on earlier colocated quiet432/432 versus split
quiet430/432 results; native and exact-source live validation are required.


Colocated `5d454a9db211a0cd8118c9e4bea60b3a95e62295` (784 inputs):
both-host affected native/storage/Raft controls and harness self-tests PASS.
Seven-client **1497/1512 PASS,15 FAIL**, no NOTRUN: deep60 and crazy
directories all seven; deep100 one. Full clients113.695–133.912s.
Immediate physical **FAIL180.057s**: final ordinary mapping counts
45/46/42/41; allocated bodies3137536/3207168/2928640/2859008 bytes.
Original180-second requirement is retained. Quiet core **432/432 PASS**
(56.885/58.182s), peer134/134 PASS, private mmap PASS with existing
shared-write ENODEV skips. Lifecycle/tiny/cold continuation still running.
No newer source is deployed during this qualification.

Next scoped changes: batched LOOKUP_PATH uses the same container-only
committed dual-group leader routing hint as LOOKUP/GETATTR before reading
rows. This is routing, never authority: fresh encountered-group proofs
still run at the destination. Actual handler tests confirm redirect has no
metadata reads; ancestors, fresh next RPC, storage refusal, bounds and
missing names remain controlled. Native/sanitizer PASS; old handler fails
the redirect assertion/SIGABRT negative control.

Container reaper now prepares lanes serially, overlaps at most four
independent all-member physical inode fences, joins every helper, then
serially selects/proposes REAP only for successful inode results. Files and
repair experiment retain serial fencing. Failed lane prep, failed/busy
physical fences, shutdown and thread creation failure retain ledger work;
creation failure only reduces concurrency. No timers, expiry authority or
per-RPC proof reuse is added. Helpers own distinct result slots and exact
inode/generation; the existing inode guards, membership completeness, death
record durability, punch/body barriers and mapping retirement remain in
host_gc_inode_nodes/server_gc_inode. Native/sanitizer controls PASS for
four bounded participants, zero/partial thread creation, joined lifetimes,
per-inode hard/BUSY outcomes and stop/empty batches. A masked-failure
negative control aborts. Both-host native and fresh live qualification are
required before claiming either failure gate closed.


Colocate5d continuation completed: lifecycle/tiny warm/cold verification,
graceful restart and deletes PASS; final physical PASS68.954s. Full15
directory failures and immediate physical180 failure remain the verdict.
Next1eb both Linux product builds succeeded but the new native fence test
failed to compile under GCC -Werror=misleading-indentation (two test-only
statements). No1eb deployment/live run occurred. Split the test statements
and require fresh complete both-host native builds of the corrected SHA.


Corrected `f28d44ca091bb44727e2ed60674133059cb21307` (785 tracked
inputs) passed both-host complete affected native/storage/Raft/cleanup
checks and was deployed with all input hashes matching. Full **1498/1512
PASS,14 FAIL**, no NOTRUN: deep60 and crazy directories all seven.
Clients113.471–131.876s. Immediate physical **FAIL180.077s**; final
counts625/722/698/641, allocated bodies43393024/50286592/48545792/
44646400 bytes. Four independent workers do not by themselves close the
physical requirement; quiet/lifecycle/cold continuation still running.

A concrete contention defect remains in the old inode guard hash: low12
shard bits plus ino>>32 map distinct same-shard ordinals below2^20 to one
RW guard. The next container-only selector includes ino>>EFS_KV_SHARD_BITS,
so independent inodes can fence concurrently rather than blocking each
other and unrelated PUTs. Every container GC/read/PUT storage path uses the
same selector; exact-inode read versus exclusive death exclusion remains
mandatory. Files/null-server callers retain the old mapping. Focused native
and ASan/UBSan controls PASS:1024 same-shard ordinals dispersed, exact-inode
read/write exclusion and files unchanged. Old-hash negative control aborts.
No stronger live performance or physical pass is claimed yet; require both
hosts' full affected storage/race controls and exact-source live rerun.


Guard candidate additionally extends real test_store_containers: four
same-shard inode GC calls must simultaneously reach an authority callback
while each owns its exclusive exact-inode guard. A bounded3-second
rendezvous fails rather than hanging if all guards alias. The callbacks
then run real death certificates, physical punch/barriers and retirement;
a live sibling retains its verified bytes, dead siblings return NOT_FOUND
and refuse delayed PUTs as STALE. Runs in buffered and direct modes on
Linux. Test hook counters are atomic for concurrent actors. This new real
storage case requires Linux native validation; local selector/sanitizer
controls alone do not establish successful physical storage concurrency.


Exact guard `c4c4bb024b2a0fd817e129b25a688cffe2de3173` (786 inputs)
passed both-host affected native controls including real same-shard GC in
buffered/direct and file-mode guard race controls. External runner initially
passed the file-control pathname as a meta_apply argument; corrected and
explicitly ran the file-control binary on both hosts, both PASS before deploy.
All source inputs match at deployment. Intel1 efsd
`3cd00044ceddccf2496f9d315326fc6b6c7c3d329d23a9db8805b581510b3de0`,
FUSE `49bd326c3145faa7c1b8de51213c2a974044c6a1e6f2fade90d3fd8198608993`;
Intel2 efsd `9d54ea7a46bf495ae085a52840a90996bb39fa8c4e56edf49d94212c50b3ce53`,
FUSE `56e758f621be19dd54c4e77ec87ca812acde7467377d3150c0117a5c98c1c6bc`.

Seven-client full **1497/1512 PASS,15 FAIL**, no NOTRUN: deep60 and crazy
directories all seven, deep100 one;110.677–132.791s. Immediate physical
**PASS164.183s**, all-zero at164.158, original180 deadline. Initial ordinary
mappings27852/27764/27808/27847; allocated bodies1937412096/1932292096/
1933938688/1938317312 bytes. This closes that physical gate for this
failed-directory full load; it does not establish a completely passing full
workload or qualify a later source. Quiet core432/432 PASS (56.569/57.601s),
peer134/134 PASS (39.466/42.766s), private mmap PASS/shared-write ENODEV
skips. Lifecycle/tiny/restart/cold/final observer still running. Directory
acceptance remains open; next work is targeted diagnostics without changing
case deadlines, client count, fresh authority or namespace cache coherence.


C4 remaining acceptance completed: lifecycle256×512KiB four-worker
create/read/churn and both-host tiny300 durable generations, warm/cold
hashes, graceful global restart and deletes PASS. Final physical
PASS30.887s. Immediate full-load physical164.183 PASS remains distinct.

Separate C4 diagnostic configuration enabled existing RPC/read/Raft
profiling, not acceptance: full1498/1512 PASS,14 deep60/crazy-dir timeouts.
All metadata leaders are local to the serving dual-group node; counters
show no remote paired/scalar proof calls there. Lookup averages about6.8ms,
about6.5ms in fresh authority versus0.2ms metadata lookup. Diagnostic
phase collector produced empty files: the harness intentionally os._exit's
its frontend, discarding that collector's buffered writes. No phase timing
conclusion is supported by those files. Completed diagnostic workload
physically drained without resets; profiling results do not replace any
acceptance result.

Next container-only admission candidate registers a bounded monotonic
ticket before h->mu acquisition. Each tracked ReadIndex begin captures its
watermark BEFORE calling the unchanged core/send. A delayed reader may use
that round only if its ticket was registered before probes; arrivals during
probe send or later require another round. Round/term identity must match;
untracked/unknown and saturating counters conservatively use the original
next-round rule. This avoids requiring an extra quorum solely because a
request waited for the Raft mutex. Both paired admissions precede this
caller's begins; independently eligible group proofs remain mandatory.
No lease, clock/expiry authority, queue, thread, proof-index reduction or
quorum/apply/configuration predicate changes. Files retain their original
admission policy.

Actual-helper native and ASan/UBSan controls PASS: mutex-blocked request
admitted before probes, late-in-send exclusion, old completed/pending
rounds, term/untracked round and failure refusal, files unchanged, counter
saturation fallback and1024 concurrent unique tickets. Both restoring the
extra-round rule and capturing the watermark AFTER probe send fail/SIGABRT
negative controls. Existing actual paired admissions, publication and
completed-proof term/config/role/storage/own-apply controls PASS locally.
Both-host native and fresh complete live validation of the exact corrected
source remain required; C4's physical pass cannot qualify this newer source.


### Pre-mutex admission 2065f216 — completed, directory gate FAIL

Exact source2065f21664ac04e00e4ebde67b40be914381c296; both Intel hosts
built with EFS_GIT_ID/EFS_GIT_DESCRIBE set to that SHA, branch devel-container.
All787 frozen source input hashes match deployed proofs. Same retained
four-node4G/node fixture, containers/TCP/direct, seven cross-host mounts,
commands/.efs/expiry-date/packing disabled,8writers and20ms progressing GC.
Both-host affected native/storage/Raft/self-test controls PASS before deployment.
Reproduce via intel_container_qualification.py concurrent then immediate reclaim,
followed by core/peer/mmap/lifecycle-create/read/churn/verify/tiny-create/restart/
lifecycle-cold/tiny-cold/lifecycle-delete/reclaim, with unique --run-id and
--output outside Git. Original seven clients,16jobs/client,216cases/client
and case deadlines unchanged; physical deadline180s, no reset/history removal.
Concurrent1498/1512 PASS,14FAIL: deep60 and crazy-directory cases on all
seven clients,120.410–133.336s. Immediate physical PASS178.336s, all four
ordinary maps and allocated container bodies zero at178.326s. Quiet core
432/432 and peer134/134 PASS; private mmap PASS, shared ENODEV skips.
Lifecycle256x512KiB/4workers/read30/churn30, tiny300 durable generations/host,
warm/cold hash checks, global graceful restart and deletion PASS. Final
physical PASS14.263s. Narrow physical margin and failed-directory load do
not establish reclamation under a fully passing concurrent workload.
Raw logs/proofs/TSVs retained outside Git under container-20261012/
admit2065-deploy and admit2065-qualification. Directory gate stays open.

Next container candidate captures coherent hosted-leader commit floors
before mutex waiting, paired with the already pre-send admission tickets.
Only matching leader/term views are used; missing/follower/changed-term
views fall back to the locked current floor, same-term commit regression
refuses BUSY. The unchanged core captures its full proof index and own
apply must reach that actual index; later concurrent writes do not become
extra minimum dependencies. Files retain original admission floors.
Actual paired helper controls, unknown/follower/term fallbacks, regression
refusal, ASan/UBSan and restoring the late-floor negative PASS locally.
Both-host native and fresh live qualification remain required for this
candidate;2065 results cannot qualify it.


Paired-floor5aa743b175db0ef651cf63759a7aeb43e51c4136: both-host
complete affected native/storage/Raft/self-test controls PASS. Initial
private deployment NOTRUN after shutdown-helper ESRCH race on Intel2 n3:
process exited during /proc/stat read; no copy/start/mount phase occurred.
Production processes remain alive. Follow-up helper treats ProcessLookupError
as exited during inspection/open/signal/poll, closes pidfd in finally and
rechecks argv/start after pinning before signalling. Actual helper mock
controls exercise every ESRCH stage, reused-PID refusal, zombie return and
FD cleanup PASS locally. No forced daemon kill or private-store reset.
Exact corrected source will be rebuilt/proved before renewed live testing.


### Paired-floor and safe fixture0f851fcc — completed, directory gate FAIL

Exact0f851fcce3154bc1f090893cd317553bd2f61bff, both-host native and
787input deployed proofs PASS. Identical retained four-node4G/node
containers/TCP/direct,8writers,20ms progressingGC, seven cross-host mounts;
commands/.efs/expiry-date/packing disabled; no profiling/reset/history removal.
Same reproduction gates and original case/concurrency/deadline limits as2065.
Full1497/1512PASS15FAIL: deep60/crazy directories all7,deep100one;
138.439–140.159s. Immediate strict physical PASS146.014s, zero145.994.
Quiet core432/432PASS56.600/57.135s;peer134/134PASS39.335/39.673s;
private mmap PASS/shared ENODEV skips. Lifecycle and tiny warm/cold hashes,
global graceful restart/deletion PASS; finalphysical20.060sPASS,zero20.047.
Intel1 efsd sha2560c7c8d69321383138a95e0bfa097f912cb8868ad40fe65f02ac3ab5a5f570b8c,
fuse83c7a5356befe4f1cf6af13984504d076fd74684075de17e24be6dbcd3672047;
Intel2 efsd3ed4ee6215d12eab584c722ecb191ad061bd996e9ba7eccbb504565383bed1cf,
fuse30d17887e0cba3277633727fa2aeb765335a33ec0b80f787b3448e9f7aa782fe.
Raw retained outsideGit safe0f85-deploy/safe0f85-qualification. Paired
floor change is safety-tested but has not improved the loaded directory
verdict. Physical passes apply to this failed-directory workload only.
Separate same-source diagnostic next enables existing read/Raft/batch
counters and flushes phase observations; it cannot replace acceptance.


Same0f85 diagnostic (read/Raft/batch counters ON, not acceptance) full
1496/1512PASS16FAIL: deep60/crazy all7,deep100two. Flushed deep markers
show every worker enters creation after0.17–0.38s setup; none reaches
leaf-write or recursive cleanup before its original30s deadline. This
supports creation-phase delay, not recursive cleanup as the failing phase.
Leader lookup average5.51ms, fresh authority5.17ms, KV0.237ms; no remote
proof routing. Cumulative all-frame inbox age averages n1=5.31ms,n2=47.19ms,
n3=8.49ms,n4=3.22ms; these are not read-only latency histograms and cannot
prove an individual vote delay. Diagnostic physical cleanup PASS126.557s;
no reset. Raw diagnostic slices/phases retained outside Git safe0f85-diagnostic.

Next bounded container vote worker services already admitted80-byte fresh
read-context headers while the pump is in its off-lock durability tail.
One event-driven helper per container daemon, no new timer or queue; h->mu
still serializes unchanged core eligibility and pins snapshot identities.
FIFO receive, bytes and deferred durability credits stay owned by the pump;
no entry apply, persistence, term/config/boot/quorum/own-apply change. Files
start no helper. Eventfd/thread failure retains existing pump fallback;
shutdown atomically stops/wakes/joins before freeing host/core/inbox state.
Local pending-read/probe/inbox/pair/completion/operation authority controls
PASS. Actual Linux eventfd lifecycle control NOTRUN on macOS; local txn
control NOTRUN because libefs.a is absent. Both require fresh Linux native
validation before deployment; no performance or acceptance claim yet.


Workerdb72bd5843daf71cb93e09ec6f54cd056b74bbc0 product builds PASS on
both Intel hosts. Full affected native stopped at flow adapter compile:
missing rd64be/worker-kick mock hooks; no deployment. Actual Linux worker
control separately PASS both hosts: real eventfd/thread, pump-independent
votes, refusal/retry, unchanged FIFO/credits, bounded coalesced wakeups,
files/eventfd/thread-failure fallback, stopped-before-core and join cleanup;
ASan/UBSan and worker-disabled timeout/SIGABRT negative PASS. Corrected
mock adapter receives those hooks; complete corrected-source native remains
required. This is native progress evidence, not loaded performance closure.


### Extra vote workere9fbb5bd — completed, REJECTED for loaded regression

Exacte9fbb5bd819556fcfb77b757ce63dd297a8b04ff, both-host complete
native controls and788 deployed input proofs PASS. Same retained Intel
four-node4G/node containers/TCP/direct fixture and unchanged seven-client
16jobs/216case/deadline limits,8writers/20msGC; commands/.efs/expiry-date/
packing/profiling OFF. Full1490/1512PASS22FAIL: deep60/deep100/crazy dirs
all7 plus one crazy roundtrip;128.019–139.480s. Immediate physical
PASS175.345s,zero175.313; quietcore432/432PASS60.560/61.372s;
peer134/134PASS40.095/42.464s. Private mmap PASS/shared ENODEV skips;
lifecycle/tiny/warm/cold/global restart/deletion PASS; finalphysical
PASS17.258s,zero17.255. Reproduce same intel_container_qualification.py
gates/order and exact-source build procedure as prior attempts. Raw
vote2e9fb-deploy/vote2e9fb-qualification retained outside Git. Physical
pass is only this failed-directory load; extra worker does not improve
loaded acceptance and is retired, along with its current-only adapter/test.
Its controls remain reproducible frome9fbb5bd history.

Next container-only candidate filters wake selection using each admitted
waiter's actual wanted floor/round/term. Unrelated commit/stamp updates do
not signal a pending read. Fresh applied quorum proof, term/role/missing
view changes, or completion of an old round requiring a new begin wake
promptly. The outer waiter retains every existing authority predicate and
original deadline; condition registration and publish/wake still share
cv_mu. Ordinary stamp/index waits and files policy unchanged; saturated
slots retain original shared-condition fallback. No helper/thread/queue,
lease, read-index reduction or placement change. Actual registration/no-
lost-wake,1000 unrelated publications, fresh proof/full own apply, old
round/wrong floor refusal, next-round progress, term/role/missing views,
files/index controls and ASan/UBSan PASS locally. Restoring stamp wakeups
fails negative control. Both-host native and exact-source live remain
required; this is a candidate, not directory closure.


### Wake selection35ac542e — completed, rejected

Exact35ac542e1cd76eba4ca5218d064a6ff88d882435, both-host native and
788-input deployment proofs PASS. Same retained four-node4G/node Intel
containers/TCP/direct, seven clients/jobs16/original deadlines,8writers/
20msGC, commands/.efs/expiry-date/packing/profiling OFF. Full1498/1512
PASS14FAIL, deep60 and crazy directories on all7;115.526–132.075s.
Immediate physical FAIL180.524s, last180.515s: maps1914/1915/1903/1949
and allocated bodies132128768/133386240/131919872/135401472 bytes.
Quiet core432/432 PASS56.855/58.488s,peer134/134 PASS39.421/40.324s;
private mmap PASS/shared ENODEV skips. Lifecycle/tiny/warm/cold/global
restart/deletion PASS; finalphysical PASS22.957s,zero22.954s. This final
pass does not replace the failed immediate deadline. Raw wake35ac-deploy
and wake35ac-qualification stay outside Git. Wake filter/test retired;
original waiter publication policy restored. No resets or production change.

Next candidate coalesces only canonical80-byte successful prefix-zero
READ-vote replies in the container priority outbox. Same group/peer/term/
boot and nonzero monotonic read context; all44 progress bytes must be
zero and vote_granted=1. Newer context replaces obsolete queued context;
late older arrival is consumed without erasing the newest vote. Ordinary
replication ACKs, progress-bearing replies, changed identities, failures
and files retain existing policy. Core still requires exact current-round
quorum and full own apply; no authority cache, deadline or receive-credit
change. Leader restart requires a new term; read contexts never wrap.
Actual outbox controls over1000rounds, every header guard, UINT64_MAX,
late-context negative and ASan/UBSan PASS locally. Real core test adds
obsolete-zero-prefix refusal and current fresh quorum without commit/
apply progress; both-host native and exact-source live remain pending.


### Canonical read-vote coalescingcb39a4f6 — all gates completed

Exactcb39a4f6b2cab06be3497cff3d0b416ef2466189. Both Intel complete
affected native builds/controls PASS, including actual core current-vs-
obsolete zero-prefix replies, Linux flow and outbox ASan/UBSan/negative.
All787 tracked product/test inputs match both deployments; binaries built
with full EFS_GIT_ID/EFS_GIT_DESCRIBE and devel-container branch. Intel1
efsd SHA2565cc83102ab2f1d6667929d9c93ce8824e259adc6c882c1e0ddb667ae88098fd8;
FUSEa7ca50b94a0ade13b68f4ffb4f55e3ce909ba21be3a542f60f79cd0be14e78e8.
Same retained four-node4G/node containers/TCP/direct Intel fixture, seven
clients/jobs16/original deadlines,8writers/20msGC; commands/.efs/expiry-
date/packing/profiling OFF. No reset or production change. Full1498/1512
PASS14FAIL: deep60 and crazy directories on all7;123.990–130.929s.
Immediate physical PASS176.539s,zero176.536s. Quiet core432/432
PASS56.647/59.054s;peer134/134 PASS39.726/41.636s;private mmap PASS/
shared ENODEV skips. Lifecycle/tiny/warm/cold/global restart/deletion
PASS;finalphysical PASS27.876s,zero27.871s. Reproduce existing committed
intel_container_node.py deployment/proof and intel_container_qualification.py
gates in their established order. Raw votequeuecb39a4f6-deploy and
votequeuecb39a4f6-qualification stay outside Git. Physical passes cover
only this failed-directory workload, not hypothetical all-passing load.
No directory closure or release acceptance; narrow queue bound retained,
its performance benefit remains unproven. Next investigate the directory
creation/authority latency with separately labelled diagnostics.


Queuecb39a4f6 separate profiling diagnostic (not acceptance): full1498
PASS14FAIL,119.031–131.184s; all7 deep60 workers enter directory creation
within0.084–0.434s of spawn and never emit leaf-write/recursive-cleanup
before their original30s deadline. Latest sampled leader LOOKUP6.296ms,
parent fresh authority5.913ms,KV0.259ms,lanes0.050ms,getattr0.072ms;
ReadIndex leader4.376ms average. This identifies the expensive authority
path, not the cause of an individual timeout. Raw bounded slices/phases
are outside Git votequeuecb39a4f6-diagnostic. Diagnostic cleanup PASS178.460s,zero178.447s (profiling enabled, not acceptance).

Next candidate hands off a completed tracked read round in the pump before
its off-lock durability tail, only when container callers registered after
the pre-send watermark. Requires unchanged current role/term/context,
nonpending quorum completion and own apply through the actual proof index.
The next begin captures its watermark before sends; late send arrivals
remain eligible only for a later round. Existing core retains the settled
proof for previously admitted readers; a fresh pending round does not
inherit its quorum. No timer/worker/queue/lease or new permission to read;
no pending-round resets, files change or deadline change. Failure preserves
the tracked watermark and normal caller fallback. Actual helper guards,
late arrivals, demand bound, ASan/UBSan and incomplete-apply negative PASS
locally; real core retained-proof controls and both-host native/live pending.


### Demand handoffa0ecf931 — completed, rejected

Exacta0ecf931a5970a5da59ea45bb061a8251ffecda9 both-host complete
native controls and788-input deployment proofs PASS. Same retained Intel
four-node4G/node containers/TCP/direct, seven clients/jobs16/original
deadlines,8writers/20msGC, commands/.efs/expiry-date/packing/profiling OFF.
Full1495/1512PASS17FAIL: deep60/crazy directories all7 plus deep100
Intel1 clients1/2 and Intel2client1;122.770–135.752s. Immediate physical
PASS168.246s,zero168.236s. Quiet core432/432PASS53.945/55.230s;
peer134/134PASS39.635/40.731s,private mmap PASS/shared ENODEV skips.
Lifecycle/tiny/warm/cold/global restart/deletion PASS;finalphysical
PASS33.408s,zero33.391s. Raw handoffa0ecf931-deploy and qualification
stay outside Git; reproduce established committed gates/order. Handoff
regresses loaded directory acceptance and is retired, including current
test/Make entry. Its native safety controls remain reproducible in history.

Review of queuecb39a4f6 diagnostic identifies individual slow applies,
not just large batches: one leader cycle apply768.853ms over2entries
(pump hold768.890ms); another463.641ms over2entries. These sampled
cycles do not identify command type or prove flush causality. Next source
restores queue-bound product behavior and adds observational logs only:
container apply command/group/index/elapsed for existing EFS_RAFT_OBS
runs over20ms, plus existing KV flush elapsed/bytes/L0 under that same
explicit profiling flag. Normal configuration emits neither diagnostic.
No apply budget, memtable size, authority, durability or placement change.
Both-host native including actual KV LSM and diagnostic investigation
remain required; no new performance fix/acceptance claim.


### Slow apply/flush diagnosisfab16632 and off-mutex candidate

Exactfab16632a1d55b71e60ebc22b12383b409eab0ff both-host affected
native (including actual KV LSM/snapshot-tail/segment-index) and787-input
deployment proofs PASS. Separate profiling run, same retained Intel
containers/TCP/direct/4G/node/seven clients/jobs16/original deadlines,
8writers/20msGC; commands/.efs/expiry-date/packing OFF; profiling ON.
Full1498PASS14FAIL127.028–133.538s (diagnostic, not acceptance).
Diagnostic cleanup PASS91.083s,zero91.063s; no reset. Bounded raw slices
outside Git slowapplyfab16632-diagnostic. Inline4MiB automatic flushes
match individual apply stalls at the same timestamp: leader295/247ms;
later leader460ms flush/465ms apply, follower541ms flush/541.110ms apply.
This establishes that those inline flushes occupy the Raft apply mutex;
it does not prove they account for every directory timeout.

Next container-only opt-in defers automatic flush while the existing WAL
sync hold is open, then runs the unchanged flush at the outer release
AFTER the WAL barrier and OFF the Raft mutex. Explicit maintenance flush
retains policy; ordinary stores retain the old fast hold/release path.
No new worker/queue, memtable size, placement, apply cap or authority change.
Effects stay in the ordered WAL/memtable; the existing reset guard retains
newer unapplied WAL records. Nested releases cannot flush early. Failed
barrier/flush fails release and fences further mutation; captured applied
checkpoint retirement receives the failure. Container startup enables only
this scheduling option; current proof/own-apply and physical GC guards stay
unchanged. Actual native controls cover default inline policy, nested holds,
visible ordered effects, outer flush/cold bytes, real crash with newer
unapplied tail, and real SST-open failure with retained cold WAL effects.
Old-inline negative plus ASan/UBSan instrument all actual KV modules. These
controls and both-host native/live remain NOTRUN before the fresh freeze;
local structural/order review PASS. No directory fix or acceptance claim.


Deferred-flush2f8d4b7edf8898e39934b82b4d1c244aba12c528 product builds
PASS both hosts; native run stops at negative-harness compilation because
the existing sst_names helper's64-byte snprintf triggers GCC format-
truncation under Werror. No deployment. Bounded63-character precision
retains the helper's original truncation behavior and fixes that adapter
warning; full corrected-source native and live remain required.


### Deferred-held flush034c04ee — all gates completed

Exact034c04ee5857642a183bc185f51bc3407e4adede both-host complete
affected native, actual KV ASan/UBSan/old-inline negative, nested holds/
failed SST publication/real crash preserving newer unapplied WAL tail
controls PASS. All788 deployed inputs match; full version/build identity
set as prior procedure. Intel2 efsd SHA256191be4eae35866e55db229568045c3353b8b690e6015bb35b23bb0600ab50edf,
FUSEa139884c35055937d5f49c7124065a7df2e9910b551001faa0c990655649d65d.
Same retained Intel four-node4G/node containers/TCP/direct, seven clients/
jobs16/original deadlines,8writers/20msGC; commands/.efs/expiry-date/
packing/profiling OFF. Full1498/1512PASS14FAIL: deep60 and crazy directories
on all7;129.569–133.110s. Immediate physical PASS158.318s,zero158.283.
Quiet core432/432PASS56.510/57.431s;peer134/134PASS41.528/42.489s;
private mmap PASS/shared ENODEV skips. Lifecycle/tiny/warm/cold/global
restart/deletion PASS;finalphysical PASS40.851s,zero40.849. All15 gates
recorded in established order; raw flush2034c04ee-deploy/qualification
remain outside Git. No reset or production change. Safe flush scheduling
retained, directory closure remains unproven. Physical results qualify
only this named failed-directory workload; no release acceptance.


Next bounded-outbox candidate extends canonical coalescing from replies to
zero-prefix escape requests as well. Exact80bytes, vote class1, request
status0/reply success1, nonzero monotonic context and all44 progress bytes
zero; only same direction/group/peer/term/boot can replace. The leader has
already started a newer independent round before producing its request,
so obsolete queued probes cannot contribute to current quorum. Delayed
older arrivals retire without erasing the latest context. Original entry
frames and ordinary heartbeat/ACK prefixes stay separate; files policy,
FIFO receive/credits, core term/config/boot/quorum/own-apply unchanged.
Actual outbox1000-round request/reply bounds, header guards, no late
overwrite, original progress traffic and ASan/UBSan PASS locally. Both
late-context and request-bound-disabled negatives fail as expected. Actual
core control adds latest-only zero-prefix request echo and unchanged
follower apply before proving exactly its current context; Linux native
and fresh complete live remain required. No performance/closure claim.

### 2026-10-13 — canonical request queue complete run

Exact source206a0548c217c5be4e6675157b424fa3868bc596: both-host complete
affected native, KV sanitizer/fault/cold and outbox negative controls PASS.
All788 deployed inputs match, version/build identity is the full SHA on
devel-container. Intel2 efsd SHA25689681bc040721273d9fe5bc95d614060ce5be272bc53f926b9fc7e9af50680e8;
FUSE2d25b7e816bdd11dc25a1ce3f62e10d6758f614d1ca07ded1d437e130b435d6d.
Same retained private four-node4G/node Intel containers/TCP/direct fixture,
8writers/20msGC, seven clients/jobs16/original deadlines; commands/.efs,
expiry-date, packing and profiling OFF. Reproduce the15 gates in the
previous procedure with intel_container_qualification.py, unique run IDs.
Full1498/1512PASS14FAIL: deep60/crazy directories on all7 clients,
118.537–132.286s. Immediate physical FAIL180.273s: records5559/5392/5561/5424,
allocated bodies386056192/375132160/386199552/377139200 bytes. Quiet core
432/432PASS52.149/53.927s;peer134/134PASS38.876/40.227s;private mmap PASS,
shared ENODEV skips. Lifecycle/tiny/warm/cold/global restart/deletion PASS.
Final physical PASS72.263s,zero72.256. All15 gates finished; final cleanup
is not a replacement for the failed immediate gate. Raw external evidence:
artifacts/container-20261012/probequeue206a0548-{deploy,qualification}
below the workspace artifact root. No reset, production change, release
acceptance or directory/physical closure. Queue capacity correctness is
verified; performance benefit and causation of the reclamation difference
are not established by these unmatched runs.

Existing GC logs after restart show fragment deletion consuming1067–3415ms
of2102–4426ms passes, with reap619–1099ms. This points to serial fragment
delete requests as the next bounded scheduling investigation, not proof
that it explains every directory timeout. Preserve each generation/checksum
delete and durable reply; join all helpers before serial metadata ACKs.

### 2026-10-13 — bounded fragment deletion candidate

The same original per-fragment generation/checksum deletion runs in at
most four participants over32-record slices of the existing256-record
scan. Private per-record ACK buffers prevent shared append races. All
helpers join before serial GC_ACK submission, record fragments remain in
one ACK command, and failed deletes remain unacknowledged. Thread creation
failure reduces concurrency; shutdown prevents new deletion work. Export/
scan lifetimes span every join; existing cursor, scan/ACK budgets, physical
durability, fences, and retry semantics remain. Only containers without
repair use this path; files/repair retain serial deletion. Actual dispatcher
ownership/identity/partial refusal/creation fallback/shutdown controls and
ASan/UBSan PASS locally; disabled-parallel negative refuses as expected.
Both-host native and full original-budget live qualification remain
required. This scheduling hypothesis does not close either failed gate.

### 2026-10-13 — parallel fragment deletion full qualification

Exact17f7db68e5999e005ec9aded168fe8bba85b748a both-host complete affected
native, actual dispatcher ASan/UBSan/disabled-parallel negative, durable
late-DATA/death/checksum and KV failure/cold controls PASS. All789 frozen
inputs match on both hosts; full source SHA is build/version identity,
branch devel-container. Intel2 efsd SHA2569b82aefd0ab158f15c43e1896f2d59de745f97823399e6602778f58e891db6be.
Same retained four-node4G/node private Intel containers/TCP/direct,
8writers/20msGC/seven clients/jobs16/original case deadlines and15-gate
sequence. Commands/.efs/expiry-date/packing/profiling OFF. Raw external
artifacts/container-20261012/fragparallel17f7db68-{deploy,qualification}.
Full1496/1512PASS16FAIL: deep60/crazy all7, deep100 two clients;
133.001–139.341s. Immediate physical PASS101.715s,zero101.707; quiet core
432/432PASS58.810/59.540s,peer134/134PASS43.890/45.474s;private mmap PASS,
shared ENODEV skips. Lifecycle/tiny/warm/cold/global restart/deletion PASS;
final physical PASS9.009s,zero8.996. All15 gates finished without reset.
Physical pass applies only to this failed-directory load; no all-passing
load or release acceptance. Bounded fragment parallelism is retained on
this experimental branch with its exact-source safety evidence and
reclamation pass. It does not fix directory latency. Different retained
metadata history and failed case counts mean these sequential runs do not
establish a matched performance comparison or cause for the two extra
deep100 timeouts. Next diagnostic uses this same source with bounded
existing counters and phase markers, separately from profiling-OFF verdict.

### 2026-10-13 — current-source traffic diagnostic and pure request retirement

Diagnostic source17f7db68e5999e005ec9aded168fe8bba85b748a, same retained
fixture/original workload/deadlines, explicit bounded existing RPC/read/
Raft/batch counters ON, commands/.efs/expiry-date/packing OFF. Full1495PASS
17FAIL: deep60/crazy all7, one deep100, one flock/shared-exclusive and one
byte-range-lock timeout. Profiling-OFF1496/1512 verdict above remains
separate. Deep60 phase markers enter CREATE promptly, with no leaf-write
before deadline on any client. Leader LOOKUP6.025ms average, parent fresh
proof5.522ms; RI4.055ms over264266 leader calls; KV.380ms. Peak apply24.831ms,
pump-hold39.665ms, lock-wait73.965ms. Six214–344ms sampled flushes are
now off the Raft apply mutex; the former hundreds-of-ms apply/flush
correlation is absent in this sample. Leader partial replies243854 versus
covered22314; wire sends946436. Followers' early request votes115016/
214071/136491, with inbox drops1601/48413/462. These counters describe
traffic/pressure, not proof of the directory cause. Diagnostic strict
physical PASS57.667s,zero57.649; raw external
artifacts/container-20261012/fragparallel17f7db68-diagnostic.

A proposed pure-vote replication-send diagnosis was NOT reproduced: real
updated test_raft against deployed17f7db68 archive PASS because the existing
in-flight guard suppresses the tested resend. Core replication logic is
unchanged; retain the explicit boundary control to prevent repeating that
unsupported diagnosis. The narrower host defect is forced FIFO retention
after a read-only request already completed its control work. For exact
container80-byte prefix-zero class1/nonzero-context requests only, consume
a successful core result1 even with pending inbox. The core already sent
the fresh vote and reports no commit/apply bookkeeping owed. Every result2,
progress field, ordinary frame and reply remains ordered. No authority,
boot/term/config/quorum, own-apply, data durability or credit rule changes.
Local host positive/header/refusal/pending-order controls and ASan/UBSan
PASS; forcing old FIFO retention fails the new positive control as expected.
A first negative-control build hit a Clang tautological-expression warning;
its adapter was corrected before any freeze/deployment. Both-host fresh
native and full profiling-OFF live qualification remain required.

### 2026-10-13 — pure request retirement complete profiling-OFF run

Exact326da5be9d418075bc8f3d957793848cd671168e both-host complete affected
native, host-control ASan/UBSan/old-retention negative, physical late-DATA/
death/checksum and KV failure/cold controls PASS. All789 frozen inputs
match; full source SHA is build/version identity, branch devel-container.
Intel2 efsd SHA256706f12426437af2ec2a9edcf990455a30fb571ceea55ef635c0491322fc2a50c.
Same retained private four-node4G/node Intel containers/TCP/direct fixture,
8writers/20msGC/seven clients/jobs16/original deadlines and15-gate sequence;
commands/.efs/expiry-date/packing/profiling OFF. Raw external
artifacts/container-20261012/pureconsume326da5be-{deploy,qualification}.
Full1498/1512PASS14FAIL: deep60/crazy all7;117.435–137.150s. Immediate
physical PASS115.146s,zero115.144. Quiet core432/432PASS55.548/58.143s;
peer134/134PASS44.340/45.849s;private mmap PASS/shared ENODEV skips.
Lifecycle/tiny/warm/cold/global restart/deletion PASS; final physical
PASS10.644s,zero10.641. All15 gates finished, no reset. Runtime checks
confirmed2 daemons/host,4+3 private mounts and zero feature/profiling flags.
Production PIDs remain5945/5953/6602 Intel1 and10086/10094/10331 Intel2;
no payload immutability claim without a prior baseline. Both tested
parallel-fragment candidates pass immediate physical cleanup on their
failed-directory load. Directory/all-passing loaded gate remains FAIL;
no release acceptance. Read-only retirement correctness controls pass,
but its workload speed benefit is not established by unmatched runs.

Next diagnostic observes original stat/mkdir calls without substituting
a different path algorithm. The POSIX harness launches fresh Python
workers; parent-only monkeypatches do not propagate. Use committed
scripts/container_deepsteps_sitecustomize.py copied as sitecustomize.py
into BASE/artifacts/deepsteps on both hosts. Prepend that directory to
PYTHONPATH only in the existing phase-socket wrapper before it launches
workers. Same TCP/direct/features/deadlines/concurrency; explicit existing
profiling counters ON, separate from acceptance. Record offsets before
launch, retain at most12MB per daemon and bounded deep60 phase/step markers
externally, then run the strict reclaim gate with the diagnostic run ID.
Local fresh-child original os.makedirs socket smoke PASS after ordinary
sandbox denied Unix-socket bind; escalated check succeeded. This observer
is a diagnostic-only addition after the tested product freeze; no product
input or binary change is claimed for326da5be.


### 2026-10-13 — original deep-path diagnostic and local wait instrumentation

Diagnostic product326da5be with fresh-child observer13c7ca5b, same retained
Intel four-node4G/node containers/TCP/direct/seven clients/jobs16 and original
30-second deadlines; .efs/expiry-date/packing OFF, explicit profiling ON.
Full1491/1512PASS21FAIL: deep60, crazy directories and deep100 on all7.
127.402–138.417s; immediate strict physical PASS58.731s, zero58.710.
Raw external artifacts/container-20261012/deepsteps326da5be-diagnostic;
reproduction uses the preceding committed sitecustomize/phase-socket steps.
This separate instrumented result does not replace profiling-OFF acceptance.
Deep60 completes only37–40 mkdir levels by deadline; recursive initial stat
cost is0.354–1.242s, completed mkdir calls26.888–28.441s. Late full-path mkdir
calls1.293–2.137s. Client0 first30-second LOOKUP means7.2–7.5ms; RPC checkout
is approximately1us, not a demonstrated bottleneck. No report-pressure event
was observed in that interval. FUSE already configures32 active workers and
8 idle workers; a presumed ten-worker default is ruled out by actual code.

Next diagnostic adds EFS_RPC_PROF-only DIRLOCK-WAIT records for local stripe
waits >=20ms, including single/pair/all-stripe acquisitions, retaining identical
mutex order, ownership, deadline and workload. Profiling-OFF behavior has no
clock/logging calls. This instrumentation is not a performance fix; build/native
and exact diagnostic deployment remain required. Directory gate stays FAIL;
physical success still covers only the named failed-directory workload.


### 2026-10-13 — lock-wait findings and default-OFF duplicate inbox candidate

Exacte8f04e2766e416f42d7378ea455e190e3911dd20 both-host complete native
and790 frozen-input proofs PASS. Diagnostic same retained four-node4G/node
Intel containers/TCP/direct/seven clients/jobs16/original deadlines, features
OFF/profiling ON:1498/1512PASS14FAIL (deep60/crazy all7),131.589–138.965s.
No DIRLOCK-WAIT >=20ms on any of seven clients; RPC profiling was active on
all7 (64–79 samples each). Immediate physical PASS102.856s,zero102.849.
Raw external artifacts/container-20261012/lockwaite8f04e27-diagnostic;
original path observer reproduction as above. First approximately30seconds
leader batch counters show inbox age mean13.999ms/199.157ms maximum,
KV release mean2.192ms/197.480ms maximum. These are diagnostic correlations,
not proof that a particular queue change fixes directory deadlines.
Normal configuration restored with all features/profiling OFF and no reset.
Its separate full qualification currently1494/1512PASS18FAIL: deep60/crazy
all7 and deep100 on4;125.591–136.967s. Immediate physical PASS96.342s,
zero96.325. Remaining quiet/peer/mmap/lifecycle/tiny/restart gates running;
no all-passing loaded acceptance or performance improvement claim.

A new default-OFF EFS_CONTAINER_INBOX_FOLD candidate considers only exact
80-byte canonical read-only requests/successful probe replies. It folds only
byte-for-byte duplicates within the pure-control suffix; ordinary, progress,
configuration and different-context traffic cannot be replaced. Remove the
older duplicate and append the newest arrival at the normal tail. Every
retained message keeps FIFO order; no newer read context is substituted.
Only containers with flow admission OFF are eligible, so no work/durability
credits are discarded. Allocate first: failure preserves the older frame.
No Raft authority, term/boot/config/quorum, own-apply or expiry rule changes.
--inbox-fold1 explicitly activates the private helper and survives controlled
qualification restart through its matching argument; both default0.
Native actual-enqueue controls cover flags/files/flow refusal, all80 header
bytes, fences/different contexts, tail order, full capacity and allocation
failure; normal and ASan/UBSan PASS, disabled-fold negative fails as expected.
Real core duplicate successful zero-prefix votes with an outstanding suffix
preserve current-round readiness, commit/apply and send counts; minimal native
and ASan/UBSan PASS. Generic macOS Makefile target could not compile existing
Linux fopencookie logging; direct unchanged real-core build avoids that
platform-only dependency. Full Linux native and live qualification are still
required before activation; this is a bounded queue candidate, not an
established directory-timeout fix. New profiling-only inbox_read_duplicate
counter measures actual folding, never admission or authority.

Candidate review tightened the fence further before any Linux freeze/build:
a same-group nonidentical pure frame is also a boundary, including a different
context, peer, boot or term. Only independent-group pure frames may intervene
between identical duplicates. This prevents folding around a possible
same-group role/liveness transition; the context fence control was strengthened.


### 2026-10-13 — lock diagnostic source, complete normal qualification

Exacte8f04e2766e416f42d7378ea455e190e3911dd20 all15 profiling-OFF gates
finished on retained Intel fixture with preceding identical configuration,
features OFF. Full1494/1512PASS18FAIL as recorded above; immediate physical
96.342s PASS. Quiet core432/432PASS52.924/56.652s; peer134/134PASS43.477/
45.077s; private mmap PASS/shared ENODEV skips. Lifecycle/tiny create, warm
verify, global controlled restart, cold hashes and deletion PASS. Final physical
9.188s PASS,zero9.175. All stores/history retained; no release acceptance.
Raw external artifacts/container-20261012/lockwaite8f04e27-qualification.
The duplicate inbox candidate's actual next frozen build is
9610602abd688f26b7f041a56323a320f83b2c5b with791 tracked inputs, explicit
--inbox-fold1. Reproduce affected new safety controls using
python3 tests/test_container_inbox_duplicate.py; repeat with EFS_TEST_CFLAGS=
'-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'. Real tests/test_raft
and all existing container/host/authority/physical/fault/cold controls remain
required on both Linux hosts. Default-OFF flag control is retained; no live
candidate result is claimed before its own deploy/proof and15-gate sequence.

Frozen9610602a built binaries on both hosts but native sequence FAIL in the
existing test_host_flow source-extraction adapter: it lacked the new
host_inbox_duplicate_read dependency. NO DEPLOY/activation. The adapter now
extracts the actual helper and classifier, supplies an ordinary/default-OFF
fixture and preserves its existing credit and async Linux sender controls;
older external source comparisons still work without the absent helper.
Local admission/default-OFF controls PASS; Linux async sender remains NOTRUN
locally and must PASS in the fresh corrected both-host native build.


Corrected frozen015b55c384c6e6ed2cce7a591863ae77a27c3355:791 inputs,
both-host complete affected native PASS including real Linux async sender,
new enqueue/default-OFF/fences/allocation/full-capacity controls and ASan/UBSan,
real Raft duplicate-vote controls and existing physical/KV fault/cold negatives.
No stores/history reset. Fresh controlled deployment and791-input/binary
proofs PASS on both hosts; actual2 daemons/host and4+3 clients verified,
containers/TCP/direct4G/node, .efs/expiry-date/packing/profiling/flow OFF,
only explicit EFS_CONTAINER_INBOX_FOLD1 candidate ON. Build identity is exact
source SHA/branch devel-container, both built2026-10-13T06:25:47Z.
Intel1 efsd SHA256bd253b35193b1f6ae4d069c573ac63589aa68ff8e58e9af601b3b9735cb046bd;
Intel2 efsd SHA25611bfff66c8af53e2c053058d2ba8273fc2fa8f29b4ad752aed37abac4b9ad507.
Raw external artifacts/container-20261012/inboxfold2015b55c3-{deploy,qualification}.
Reproduction: helper start/mount --inbox-fold1 after global graceful stop and
proof; qualification each original15 gates --inbox-fold1 (including restart).
Seven-client jobs16 with unchanged30/75/120/180case and600outer limits;
full/removal/reclaim then quiet/peer/mmap/lifecycle/tiny/cold/global restart/
final physical as earlier. Native PASS does not qualify loaded performance;
full exact-source live sequence is running, no all-passing acceptance yet.


### 2026-10-13 — duplicate inbox live result and next memo diagnostic

Exact015b55c3 profiling-OFF full1491/1512PASS21FAIL: deep60/crazy/deep100
all7;123.697–142.327s. Immediate strict physical105.053s PASS,zero105.035.
No reset or deadline/workload/concurrency change. Remaining integrity gates
running; duplicate folding did not resolve the loaded directory gate and
is not accepted as a performance fix. After this sequence ends, restore
--inbox-fold0. Raw external inboxfold2015b55c3-qualification as above.

Next diagnostic measures the unchanged one-shot50ms LOOKUP-to-GETATTR memo:
EFS_RPC_PROF-only MEMO-PROF cumulative attempts/hit/active/expired/missing,
one line per1024 attempts, outside the memo mutex. No cache lifetime, kernel
cache timeout, mutation fence, lookup RPC or quorum rule changes. Existing
actual memo/callback controls PASS (mutation success/failure, overlap, stale
RPC reply rejection, TTL and read-only hits), including MEMO_TEST_CFLAGS=
'-fsanitize=address,undefined -fno-omit-frame-pointer'. This tests the
hypothesis that unrelated simultaneous mutations suppress useful memo hits;
no scoped memo or coherence change is implemented or justified yet.
Build/native and separate original-workload diagnostics still required.


Exact015b55c3 all15 profiling-OFF gates completed: full1491/1512PASS21FAIL
and immediate physical105.053s PASS as above. Quiet core432/432PASS58.490/
59.797s; peer134/134PASS44.810/45.269s; private mmap PASS/shared ENODEV
skips. All lifecycle/tiny warm/cold/global controlled restart/deletion PASS;
final physical8.994s PASS,zero8.991. Duplicate inbox optimization remains
not accepted for loaded performance; disable it for the next investigation.

Next frozen3213688badf141ab2ee4b3d3140fa62f0f2d3e59 has791 inputs and
profiling-only memo counters. MEMO-PROF separates stored/active/fenced PUTs
from hit/active/expired/missing TAKEs, logging every1024 TAKEs outside the
memo mutex; missing entries alone are not evidence of mutation suppression.
Local actual memo/callback normal and MEMO_TEST_CFLAGS sanitizers PASS.
Both-host complete native checks in progress, live diagnostics NOTRUN yet.
Original seven-client workload/deadlines and transparent path observer stay
unchanged, folding OFF; .efs/expiry-date/packing OFF and TCP/direct retained.


### 2026-10-13 — memo diagnostic complete and reproduced older-ACK defect

Exact3213688badf141ab2ee4b3d3140fa62f0f2d3e59 both-host complete affected
native, memo/callback sanitizers and791-input proofs PASS. Folding OFF;
retained Intel four-node4G/node containers/TCP/direct/seven clients/jobs16,
features OFF, explicit diagnostic profiling ON, original deadlines/observer.
Full1490/1512PASS22FAIL: deep60/crazy/deep100 all7 plus one concurrent
create/unlink child unlink EBUSY (Intel2client0,c1-5).138.444–146.805s.
Immediate physical55.160s PASS,zero55.132. Raw external
artifacts/container-20261012/memoprof3213688b-diagnostic. No acceptance claim.
Memo samples reach2048 attempts/client:931–939 hits,640–653 active misses,
464–470 missing,zero expired;2525–2858 inserts and8989–9223 active suppressed
inserts. First1024 samples occur after the deep60 deadline, so aggregate memo
suppression is not proof of the deep-path bottleneck. No cache lifetime,
scoped mutation or namespace authority change is justified/implemented.
The new unlink EBUSY is an additional loaded diagnostic failure to retest.

A real native old-core reproducer now FAILs at duplicate older ACK retirement:
ACK prefixA starts the newer windowA+1..B; an identical delayed ACKA then
clears that newer window and sends it again. This is not a timing assumption.
Container host startup enables preserve_ack_window; ordinary files retain
cfg default0. Only a successful ordinary ACK whose match index lies strictly
below the outstanding window's FIRST index preserves that window and its
original heartbeat age. Progress ACKs, refusals, liveness, match/next,
current read context, commit/apply and all term/boot/config/quorum guards stay
unchanged. No context folding, credit retirement or expiry/lease shortcut.
Profiling-only ack_older_window records this case; transport/disk formats
unchanged. All real tests/test_raft invariants now run in legacy and container
policies. New controls prove partial progress/new window, duplicate retention,
original-age lost-ACK retransmission, covering ACK commit/apply, old-context
read refusal and fresh applied quorum success; normal and ASan/UBSan PASS.
Forcing original unconditional retirement FAILs exactly the new window
control in both fixture rounds. Reproduce via
python3 tests/test_container_ack_window.py (actual core/test sources,
sanitizers and explicit old-retirement negative); no raw fixture artifacts
committed. Both-host Linux native and full exact-source live15-gate rerun
remain required. This fixes a reproduced protocol bookkeeping defect; its
Intel directory performance benefit is not established yet.

### 2026-10-13 — older-ACK candidate Intel gates and next path diagnostic

Frozen06b4624ba05c0a28ab969b0a0f3c2277b9b96856:792 tracked regular build
inputs. Both Intel complete native build/affected Python and Raft dual-policy
checks, explicit sanitizer controls and native storage/durability/GC/overwrite,
KV, snapshot/tail/index, simulation, metadata and physical late-PUT guards PASS.
Build identity06b4624b/devel-container,2026-10-13T06:56:49Z both hosts.
All four private daemons/seven cross-host clients deployed; both source proofs
PASS and actual argv/start/executable hashes match current binaries. TCP/direct,
containers4G/node/eight writers, preserved stores/history, existing balanced
leaders and20ms GC progress yield; .efs/expiry-date/packing OFF, profiling and
experimental inbox-fold OFF. No production replacement or acceptance promotion.
External raw artifacts/container-20261012/ackwindow06b4624b-deploy and
ackwindow06b4624b-qualification.
Original concurrent core seven clients/jobs16/216 cases each/unchanged original
30/75/120/180s case budgets:1498/1512PASS14FAIL,128.931–136.069s. Deep60
and names_crazy_dirs timeout all seven. Deep100 and concurrent unlink PASS in
this run. Immediate strict physical PASS93.336s (zero93.331), all four maps
and container bodies zero; reserved metadata excluded as in the tracked gate.
Quiet core432/432PASS56.706/57.210s. Remaining peer/lifecycle/tiny/restart/cold/
final reclamation gates still executing at this checkpoint. Earlier runs are
not evidence for those exact-source remaining gates. No causal speed benefit
claim; retained older-ACK fix solves the native reproduced bookkeeping bug.
Reproduce using scripts/intel_container_node.py fresh exact-source build,
normal start/mount four/three clients, then scripts/intel_container_qualification.py
concurrent/reclaim/core/peer/mmap/lifecycle-create/read/churn/verify/tiny-create/
restart/lifecycle-cold/tiny-cold/lifecycle-delete/reclaim with unique run IDs
and output outside Git; original committed workloads/limits unchanged.

Next diagnostic adds only explicit --diagnostics --path-prof callback timings:
EFS_CONTAINER_PATH_PROF requires RPC profiling. Exact d[0-9]{2} LOOKUP/MKDIR
callbacks record caller PID, parent, monotonic start/service duration and result,
maximum16384 records/client with saturating admission. Normal callbacks do not
read diagnostic clocks/write lines; namespace resolution, reply, authority,
mutation fences and deadlines unchanged. Join these records to fresh-child
scripts/container_deepsteps_sitecustomize.py syscall observations by PID/time.
Callback duration excludes kernel queue and reply logging overhead; profiling
can perturb performance and is never normal acceptance. Local memo/callback
controls and fixture syntax PASS; Linux compile/native and live diagnostic
NOTRUN until the executing06b4624b gate sequence finishes. No cache lifetime,
scoped authority, expired-proof reuse or test workload relaxation introduced.

Exact06b4624b normal all15 gates finished: concurrent FAIL14 as above, all
remaining gates PASS. Peer134/134PASS45.047/47.081s; mmap private PASS and
shared ENODEV skips. All lifecycle/tiny warm, global graceful restart, cold
hashes and deletion PASS; final strict physical9.027s PASS,zero9.024. Existing
production PIDs5945/5953/6602 and10086/10094/10331, production mount and
opposite-host17432 seeds reverified; no immutable payload baseline claim.
Next frozen53db7b7f7769e6321eda062b7ed476f7e8884684 callback diagnostic:
792-input fresh builds complete on both Intel hosts, same complete native and
sanitizer/fault controls PASS. Build2026-10-13T07:06:21Z, branchdevel-container,
fullSHA version; both deployed source proofs PASS. Explicit diagnostic run
now in progress, profiling ON/path-prof ON, folding OFF, all requested feature
settings unchanged. Diagnostics are not normal loaded acceptance.
