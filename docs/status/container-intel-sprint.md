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
