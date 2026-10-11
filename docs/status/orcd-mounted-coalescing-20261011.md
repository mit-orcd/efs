# ORCD mounted coalescing and shared-fragment GC · Oct 11

This continuation implements W95's mounted writer and the distributed shared
object lifetime coordinator. The last verified live ORCD revision is
`00c1b548cfc8`; its quiet mounted stress run still fails. Follow-ups
`45a1046034ff` (admission discovery) and `a48076c4e937` (pinned stats) are
committed; the latter is undergoing full validation before deployment. Bounded independent query streams are implemented, but full
packing-enabled concurrent acceptance still fails under load. Cold mounted
reads and the real native-RDMA shared-object lifetime gate pass. Matched tiny
file throughput/allocation acceptance remains pending; the old 408× benchmark
has not yet been replaced by a measured mounted packing result.

The rollout captures branch tips, merges every captured tip and verifies their
ancestry before building. Candidate `dbf2b5db` includes `59f6e834` and subsequent repairs; it contains the
latest captured AMD, Intel, Mac and NUC updates and the append age/retry fix.
The Mac namespace conflict combines fresh owner authority and the registered
participant shortcut with the existing exact REGISTER apply proof. A fresh view
never survives its request, and a later fence still blocks ESTABLISH/mutation.
Both authority gates and the fence-after-view regression pass. The full
merged run caught an append fixture still calling the old proposal helper; the
updated gate compiles the real command-cohort implementation and preserves
every applied-result/abort/ambiguous-COMMIT check. A further lock audit moved
owner session RPCs and local size locks outside the scheduler mutex, while
reserving the cohort through those operations. Linux admission fault tests
verify those unlocked calls and retention after an uncertain owner admission.

## Mounted write ownership

`EFS_PACKED_WRITES=1` selects packing for an empty regular file's first write at
byte zero, with a payload of 1–4096 bytes and a 128 KiB export chunk geometry.
Unset export geometry uses the existing 128 KiB default. CREATE's metadata
staging mark is compatible with packing; queued or pinned data writes are not.
Fresh FileID/lane authority and an absent mapping are required before admission.
Larger writes and later writes use the ordinary path after pending packing has
completed.

Each metadata group owns one bounded, budgeted cohort of at most 16 immutable
snapshots. The context, fragment staging and retained publication records are
charged before an application write can succeed. The collection window is
10 ms; close/read/fsync wait for publication without waiting for a full cohort.
Shutdown forces a partial cohort and refuses unresolved ownership. Existing
caller deadlines are preserved, and unbudgeted flush/drain gets an 8-second
budget. Compatibility preflight uses a five-second positive cache keyed by the
exact member set; every actual upload independently checks fresh server
admission, FileID authority and the durable storage fence.

Retries preserve the same object, bytes, checksums and captured members. Each
file waits for its own matching durable publication receipt. A completed record
can retire independently while the cohort retains other unresolved records.
After all three PUT receipts, publication retries never re-PUT the shared
object. Upload permit retirement completes only after every record resolves.
Budget failure before acceptance abandons only the private unsent candidate.

## Shared lifetime and physical deletion

The owning group scans a bounded page of durable object ledgers. Abandoned
PENDING records require completed global session revocation and the local
cleanup floor; absence or transport failure is insufficient. An object cannot
enter physical reclamation while any record remains live or pending.

Every captured member must durably fence the exact object generation before
any fragment is deleted. Storage ACKs follow real file and parent-directory
barriers, checksum/geometry validation and physical deletion. An offline member
prevents progress; the coordinator retains its durable ACK state and resumes
on retry, leader change or cold restart. A captured non-placement member must
also establish its fence. Upload replay retirement does not erase lifetime
identity needed for reclamation.

## Evidence and remaining acceptance

The first merged deployment passed the full Linux suite, all seven clients'
unchanged concurrent POSIX suites (220 PASS and one expected mmap SKIP each),
and 27 prepare/27 cold verification cases without a reset. A repeat wave that
overlapped the peer suite exposed three ordinary POSIX failures on one client
and one peer append timeout. Those failures are retained. The isolated peer
rerun passed all 67 cases with the original deadlines. Concurrent query cases
passed both waves; acceptance of the prior 11-million-entry catalog remains
separate from these tests on the preserved current validation namespace.

An isolated four-server TCP/files fixture verified actual mounted packing:
a four-record cohort remained readable after one record was unlinked and after
a cold restart of all four servers. The remaining three records read exactly.
After the last references were unlinked with a captured storage member offline,
all three physical fragments remained. Restoring that member allowed automatic
physical deletion of all three. The immediate post-rejoin probe still saw the
fragments; the later probe confirmed asynchronous completion. This private
fixture used buffered storage and untagged build identities, so exact-revision
RDMA/direct-I/O rollout acceptance must repeat the lifecycle checks.

The Linux coalescer gates exercise lost PUT ACKs, publication/readback/retirement
failures, immutable retry, budget-before-accept, CREATE eligibility, default
geometry, capability expiry/mixed-version refusal and retained accepted bytes.
The initial 1 ms/immediate-close run produced singleton packs; the bounded
10 ms close collection window produced the verified four-record cohort.
Neither run establishes a throughput gain or full-cohort physical amplification.

The remaining gates are the final merged Linux suite and eleven-host deployment,
full concurrent POSIX and isolated peer acceptance with packing enabled, exact
RDMA/direct-I/O cold/reclamation checks, and matched tiny-file throughput and
physical accounting. The original namespace backups remain preserved.

### Mounted rollout regression and repair

The exact `59f6e834` deployment passed all eleven runtime identity checks,
including native RDMA, direct storage and packing enabled on all seven mounts.
Its seven-client full POSIX wave failed: each client had six truncate/rewrite
EIO cases, and several directory/query cases exceeded the unchanged 30-second
deadline. Raw results are retained under
`results/measure/20261011-orcd-coalescing-rollout/59f6e83485b5/`.

A zero-size inode after truncate still has lane fence history. Revision
`79806bfe` excludes those lanes before accepting packed bytes and uses the
ordinary reconstruction path. Its admission gate passed on Linux; an isolated
RDMA mount passed three consecutive exact truncate/write/close/read cycles and
the full POSIX suite (220 PASS, one expected SKIP, zero FAIL, 26.1 seconds).
The exact-revision full Linux gate passed; all 651 tracked build/gate source
hashes matched the committed snapshot. Seven-client redeployment remains pending;
this single-client result does not close concurrent query or directory timing.

The `79806bfe` seven-client packed wave eliminated truncate/rewrite EIO but
still timed out on directory and virtual-query cases; the runner stopped some
remaining cases after a query worker failed to exit promptly. The identical
revision with packing disabled passed all seven complete suites: 220 PASS,
one expected SKIP, no FAIL or NOTRUN each (42.7–44.3 seconds). This isolates
packing pressure from the merged metadata baseline. A bounded GDB capture
showed the coalescer idle after the wave, rather than a persistent client hang.

Revision `384c45f8` moves typed cache admission, snapshot construction and
private-candidate cleanup outside the scheduler mutex. Presence accounting can
acquire directory/index locks, so the context remains reserved until admission
and size publication finish. Cohort record insertion still holds the mutex.
The Linux admission gate checks unlocked cache calls and retained reservations
on success and failure. Full-suite and mounted concurrent acceptance are pending.

The exact `384c45f8` deployment passed eleven-host identity checks and the full
Linux suite with 651 matching source hashes. The retained RDMA pack survived
cold restart: its remaining record read exactly and all three fragments remained.
The 27 persistence verification cases passed. Its concurrent packed wave still
failed directory/query deadlines. A busy-period GDB capture showed metadata RPC
and durable publication-retirement waits, rather than scheduler mutex waits.

Revision `dbf2b5db` removes redundant publication-session log writes. The client
queries the current owner state and registration; it always calls ESTABLISH,
which rechecks fresh owner authority and a fresh participant rejection floor.
Only an already established matching epoch avoids a no-op log append. Missing
registration/establishment follows the existing durable command path; stale,
fencing, malformed, uncertain and IO results refuse admission. This proof is
limited to the current RPC; it is never cached on the client. Portable RPC,
server-floor, session admission, namespace and append gates pass. Full Linux and
ORCD concurrent acceptance are pending.


### Native RDMA shared-object lifetime acceptance

The main cluster's RDMA/direct-I/O path created a two-record shared object
(`4925959731071590093`, generation `2793619445051681373`). After one unlink,
the other 512-byte record read exactly. It survived a cold restart of all four
servers into `384c45f8`, with all three placement fragments retained.

With captured non-placement node 4 stopped, unlinking the final record produced
exact-generation durable fences on nodes 1–3, but none on node 4. All three
physical fragments remained. After node 4 rejoined, the first immediate probe
still saw fragments; a later probe found zero on every member and the durable
`EFSDEAD1` fence on all four. No manual fragment deletion or fabricated ACK was
used. This closes the actual RDMA shared-lifetime/offline-member reclamation
check; post-upgrade deletion persistence and final concurrent acceptance remain.

Candidate `dbf2b5db` passed the full Linux suite with all 653 tracked build/gate
source hashes matching the committed snapshot. Ten additional staged host builds
passed. The unchanged seven-client mounted wave has not yet been run on it.


### Remaining barrier pressure

All eleven `dbf2b5db69e6` runtime identities passed. Shared-fragment deletion
and all four durable fences survived the cold upgrade. The unprofiled seven
client wave still failed: 4–7 FAIL per client, with seven NOTRUN on five clients.
Directory and virtual query timeouts remain; source matching and shared GC
success do not close this failure. Raw TSV/logs and post-wave/busy captures are
retained under `results/measure/20261011-orcd-coalescing-rollout/dbf2b5db69e6/`.
The separately tagged profiled wave is diagnostic, not acceptance.

The busy-period capture shows many requests waiting for group ReadIndex proof;
one replica's pump was syncing the metadata WAL. Candidate `e2be6073` reuses a
successful local group barrier for subsequent records of one packed PUT request.
Each record still reads its current FileID/epoch; forwarding creates no local
coverage, failed barriers set no coverage, and the next RPC starts with an empty
mask. The production-function gate verifies those cases and an epoch change
between records. The full Linux gate caught an invalid new capacity test: the maximum image is
128 KiB, so sixteen 4 KiB payloads plus the table fit. Revision `c8e420da` removes
that unnecessary capacity change/test while retaining the request-local barrier
repair. The actual barrier and buffered/direct upload gates passed in the failed
run; a complete corrected Linux run and mounted acceptance remain pending.


Corrected `c8e420da6523` passes the full Linux suite; all 654 tracked build/gate
source hashes match the committed snapshot. The quiet packed peer suite passed
67/67 on the still-live `dbf2b5db` revision during that build (116.6 seconds).
Its result directory uses the candidate tag by default; the colocated provenance
JSON records the actual runtime revision explicitly. It is not candidate peer
acceptance. The candidate's 27 persistence prepare cases passed before upgrade;
cold verification and an unprofiled seven-client wave are next.


The corrected candidate deployed successfully to all eleven hosts with packing
on. The original-tag cold persistence verification passed 27/27 (3.6 seconds).
The unprofiled `c8e420da6523-wave1` still failed: 4–7 FAIL per client and seven
NOTRUN on all seven clients following virtual-query worker failures. Request-local
PUT barrier reuse therefore does not establish concurrent acceptance. Isolated
concurrent extras and process-level query tracing are the next diagnostic gates.


All seven `c8e420da6523-extras` runs completed with four PASS, one expected mmap
SKIP and no failures (3.4–8.3 seconds), while the separately tagged full diagnostic
wave still failed on every client. A 12-second syscall trace caught the command
blocked opening its virtual page result. Production inspection found one global
retained page session per publisher: any other query identity received BUSY until
the first stream closed or expired, and the same global mutex covered quorum
waits and scans. This provides a concrete contention mechanism; the mounted
acceptance wave must establish whether it resolves the observed failures.

`f64114b2` replaces that singleton with 16 independently reserved streams per
publisher. Admission/lifetime locking covers only slot selection and release;
busy slots cannot expire or be reused. Repeated opens of the same first page
reuse its immutable pin, while each other identity gets its own token and pin.
Tokens retain publisher routing and boot-dependent uniqueness. Each page still
requires fresh quorum authority, current capabilities and fresh directory access
checks. The eight-request/64 MiB reply bound and aggregate retained disk budget
remain enforced. Idle/total expiry and explicit close release individual pins;
shutdown waits for active slots. The production admission regression exercises
independent progress, capacity refusal, caller isolation, duplicate-open reuse,
busy/idle expiry, stale close and balanced catalog references. The live page gate
now requires two retained streams to coexist. Full deployment acceptance remains
pending; the failed waves and trace are retained.


Candidate `f64114b2a6f0` passes the full Linux suite. All 655 tracked build/gate
source hashes match its committed snapshot, and all ten additional staged builds
pass. Its 27 persistence prepare cases passed on the previous runtime (14.8 s).
The complete eleven-node upgrade is now in progress; these build results do not
replace mounted concurrent acceptance.


All eleven live processes now match `f64114b2a6f0`, caller UID 246111, native
RDMA and their installed executables; packing is enabled on all seven mounts.
Cold persistence verification passed 27/27 (3.7 seconds). The previously retired
shared object remains physically absent on all four members after this restart.
The unprofiled seven-client wave is running.


The first unprofiled `f64114b2a6f0-wave1` still failed on every client: four or
five FAIL and seven NOTRUN, with 208–209 PASS and one expected SKIP. The bounded
stream fix does not close concurrent acceptance. Directory and virtual-query
workers still exceeded 30 seconds under the full workload, despite the isolated
extras passing previously. A separately tagged pressure diagnostic is collecting
client and all four server stacks; it is not an acceptance timing run.


The quiet live page gate passes on `f64114b2`: default/explicit display limits,
complete NUL/NDJSON export, two concurrent retained identities, pinned generation
across refresh, exact no-duplicate continuation, explicit close/stale token and
fresh permission revocation. This validates page-stream semantics separately
from the still-failing full workload.

The pressure trace found query fixture writes still blocked in packed admission,
with no active page requests/pins on either publisher. The client pack worker was
waiting on upload retirement, and a follower was forwarding packed admission and
retirement commands through another metadata RPC. `7c2f1b69` makes packed lifecycle
commands return current leader hints from followers/unhosted nodes, before owner
admission. Only the leader runs fresh owner admission and proposes the command;
existing fresh post-apply proof and exact retirement checks are unchanged. The
production routing gate passes for follower, unhosted, election and role-change
cases. Linux and staged build validation are in progress; no timing improvement
or mounted acceptance is claimed yet.


`7c2f1b693004` passes the complete Linux suite with all 656 tracked build/gate
sources matching its committed snapshot. All eleven runtime identities match;
packing is enabled on all mounts. Cold persistence passes 27/27 (3.8 s), and the
retired shared object remains physically absent on every member. The unprofiled
seven-client wave is running with the unchanged 16-worker/30-second limits.


The `7c2f1b693004-wave1` is a rejected candidate: every client failed 168–169
cases with EIO/EBUSY (52–53 PASS), although all workers completed. The leader-only
check incorrectly covered PACK_STATUS. Fragment PUT invokes this endpoint on
its local storage member and cannot follow the returned leader hint, so two of
three placement members refused admission. `f7ad6b26` restricts leader-only
routing to admission/retirement mutations and restores fresh follower status
checks and unhosted status forwarding. The production route gate now exercises
both storage status paths as well as all mutation routing cases. The quiet
page gate remains valid only for `f64114b2`. A one-at-a-time server-only rollback
to that known-good binary leaves client mounts and their accepted body ownership
intact while the corrected candidate builds; no accepted writes are deliberately
discarded. Full Linux and mounted acceptance of the correction are pending.


Server-only rollback restored all four nodes, but the poisoned clients still
failed 15/27 new persistence prepare cases: the completed rejected POSIX wave
had removed its fixture trees while cohorts retained earlier accepted writes.
The rollback cannot recreate those FileIDs. Under the user's explicit permission
to lose disposable test data, normal stop is attempted first and refused drains
use the audited `stop --force-discard` path. Each retired mount's exact UUID is
captured beforehand and globally fenced after all producers stop, so PENDING
shared objects become reclaimable through the normal coordinator rather than
manual fragment deletion. Identity, stop and fence logs are retained.

`f7ad6b26ed28` passes the full Linux suite and all ten other staged builds. Its
extended routing gate covers the storage-local status dependency missed by the
previous candidate. Mounted acceptance must use fresh client identities after
retirement; the rejected candidate and failed prepare results remain preserved.


All seven rejected mounts are retired and their captured writer UUIDs complete
normal global fencing at epoch 2. The dedicated retirement evidence includes
explicit discard diagnostics and every successful fence reply. The correction's
full Linux log and 656-file source manifest are preserved. Normal all-node
redeployment is now in progress with fresh client identities. Its cold gate uses
the successful `7c2f1b693004` pre-regression persistence fixture; the failed
partial `f7ad6b26` prepare is not reused or reported as successful.


All eleven live processes match `f7ad6b26ed28`, UID 246111, installed executable
and native RDMA, with packing enabled. The attempted reuse of the older cold
fixture failed at harness setup: successful verification deletes its fixture
unless `--keep` is used. This was not a cold-read failure. The controller now
retains persistence fixtures explicitly; a fresh correction-tagged prepare is
running before the full concurrent wave, followed by a real cold restart/verify.


Fresh `f7ad6b26ed28` persistence prepare passes 27/27 (17.1 s), confirming
restored packed PUT admission. Its unprofiled wave still fails on all seven
clients (four–seven FAIL, seven NOTRUN, 206–209 PASS and one SKIP). Mutation
routing correctness is restored, but the original load timeouts remain.

`33b472d3` removes the separate per-file PUBLICATION_ADMIT proposal for packed
records. Their object admission already durably captures an immutable PENDING
manifest. Existing apply atomically installs each mapping, LIVE reference and
terminal receipt, with the same per-file CAS/fence and receipt-budget checks.
The host still obtains fresh initial authority and fresh post-apply proof and
reads the exact durable locator; unknown outcomes retain ownership. Legacy
publication retains its admission/PENDING contract. The production phase gate
checks command selection and admission-failure behavior; the actual metadata
gate mixes an older admitted record with a direct terminal record, injects a
failed storage batch, proves no receipt/mapping/LIVE transition leaked, then
retries through normal replay and cold recovery. Linux validation is underway.


The direct-terminal candidate `33b472d32ba6` passes the full Linux suite,
including its injected failed-batch gate, and all ten additional staged builds.
All 657 tracked source/build/gate files match the committed Linux snapshot.
All seven prior mounts drain cleanly before normal redeployment; no explicit
discard or producer fencing is needed for this restart. Mounted acceptance
is pending and is not implied by these build results.


All eleven live processes match `33b472d32ba6` with native RDMA and packing
enabled. The retained `f7ad6b26ed28` fixture verifies 27/27 after the cold
restart (3.0 s). The earlier shared lifetime object still has zero physical
fragments on all four storage nodes. Its seven-client packing-enabled wave
is running; these cold checks do not establish concurrent acceptance.


The `33b472d32ba6` unprofiled seven-client wave still fails (204–209 PASS,
four–nine FAIL, one SKIP, seven NOTRUN per client; 45.4–49.9 s). Raw
results are preserved. The terminal-proposal optimization alone is not a
concurrent acceptance fix. `237d0673` removes the redundant packed-only
status and session round trips; its exact retained request goes to the host
which still reads fresh authority and durable results before proposing.
A production-driver gate covers lost replies, missing/fenced authority,
terminal rejection, pending/retired outcomes and legacy behavior.


`237d06732ff7` passes the full Linux suite and ten additional staged builds;
all 658 tracked sources match the committed Linux snapshot. Normal ORCD
redeployment and retained-fixture cold verification are in progress. A quiet
post-wave inspection of `33b472d32ba6` found both cohorts empty on fcstor008;
it is diagnostic evidence, not a performance measurement.


All eleven live processes match `237d06732ff7`, installed executable,
UID 246111 and native RDMA, with packing enabled on all seven mounts.
Retained cold verification again passes 27/27 (3.0 s); all four storage
nodes still report zero physical fragments for the retired shared object.
The packing-enabled concurrent wave is running.


`237d06732ff7` still fails the full concurrent wave: all seven clients
record 209 PASS, four FAIL, one SKIP and seven NOTRUN (43.2–45.3 s).
`619301ee` introduces at most four independent record-publication workers
per reserved cohort. Each record retains its own receipt/load/retirement
state; all helpers join before a shared upload may retire or its context
may reset. Deadlines are inherited and restored, failed records remain
owned, and completed neighbors are not repeated. Three helper stacks are
reserved inside the charged nonmoving context before accepting writes;
creation failure leaves the caller to process unassigned records. Its
production fanout gate proves the concurrency bound, precharged aligned
stacks, joined partial failures, exact retry and expired-deadline refusal.
Linux and staged-build validation is underway.


`619301ee3bd3` passes all staged builds and the actual retained-retry
gate, but its full Linux run fails compiling the new fanout harness under
GCC `-Werror=misleading-indentation`. Its failed Linux log is retained, and
it is not deployed. `011cd1d9` splits the two harness statements; the
production implementation is unchanged. Full Linux validation is rerunning.


The corrected `011cd1d958ba` passes the full Linux suite and all ten staged
builds. All 659 tracked source/build/gate files match the committed Linux
snapshot. Normal all-node deployment is in progress; the previous candidate
remains rejected solely on its strict GCC harness warning.


All eleven live processes match `011cd1d958ba` and native RDMA, with
packing enabled. Retained cold verification passes 27/27 (3.0 s) and the
retired shared object remains physically absent on all storage nodes.
Its unchanged seven-client concurrent wave is running.


The `011cd1d9-wave1` packing-enabled stress wave still fails (206–209
PASS, 4–7 FAIL and seven NOTRUN per client). A separate profiled wave
shows all fragment ACKs received, one record awaiting publication retirement,
and another completed cohort awaiting shared-upload retirement. New writes
wait for those sealed contexts. This diagnostic pause is not acceptance timing.

The identical `011cd1d9-off-wave1` comparison on the same namespace
completes on all seven clients: three pass 220/221 with one expected SKIP;
four pass 218/221 with two query-command timeouts. Thus query failure is
independent of packing; directory-only tests pass on the ordinary path.
Both actual query publishers show refresh refusal during transaction activity,
then a 1,024-directory incremental fallback and a roughly 6.5-second full
fold. Later quiet incremental builds take roughly 0.3 seconds.

`2972e4c24973` separates file durability from receipt cleanup: after terminal
publication is consumed and the canonical mapping is loaded, close/fsync may
finish. One precharged immutable retirement slot per group retains exact ACK
queues and the shared manifest while the body context serves the next cohort.
No slot can be overwritten, total active-plus-retiring ACK ownership is capped
at 32 records, and clean drain includes both queues. Fault tests prove lost
ACK retry, partial cleanup, slot backpressure and unchanged fragment identity.
Full Linux validation and the exact-source deployment gate are underway.


`2972e4c24973` passed full Linux validation with 659 matching source files,
all staged builds, clean all-node deployment and 27/27 retained cold persistence
cases (2.9 s). Its unprofiled seven-client wave is running.

`00c1b548` moves final catalog destruction outside query publication and page
admission locks. Borrowed readers are drained before detaching; retained pages
keep independent references, and the single builder finishes retirement before
starting another candidate. Memory/disk limits remain enforced. Removed
old-only subtree parents no longer force a full fold: their surviving ancestors
are independently invalidated by the old parent edges. New edges retain strict
parent validation. The dirty-directory array starts at 1,024 records and may grow
only within the existing caller memory budget and a 16 MiB scratch cap, charging
simultaneous old/new buffers before allocation. Small budgets retain the full
fold fallback; allocation and I/O errors abort the candidate.

The actual Linux builder agrees with an independent full RAM catalog oracle:
removed subtree refolded=1 with 6,000 unrelated files; a small-budget 1,024-dir
change (1,025-dir closure) still falls back safely; the larger existing budget
handles 1,025 changed dirs incrementally with exactly 1,025 refolds. The stream
lifetime gate proves destruction runs outside the admission lock, while pin
identity, busy expiry and balanced release continue to pass. Full validation
and loaded mounted acceptance remain required.


The `2972e4c2-wave1` still fails on all seven clients (209–210 PASS,
three or four FAIL and seven NOTRUN). The separate focused query oracle overlapped
part of that wave, so its elapsed times are diagnostic rather than quiet
performance acceptance. The unchanged next seven-client wave will run only
after all builds and focused tests finish.

`00c1b548cfc8` passed the full Linux suite and all staged builds; all 659
tracked source/build/gate files match the committed validation stage. The
verified rollout checks every preceding command and refuses a changed HEAD;
normal all-node deployment is underway. A follow-up admission candidate uses
the existing fresh GETCHUNKS reply for both exact generation discovery and
empty-base proof, removing WRITER_VIEW discovery. Lane/bootstrap freshness,
truncate exclusion, session authority and terminal CAS remain enforced. Its
focused actual-admission gate passes stale/malformed/occupied-base refusal.
It is not part of the current deployment.


The quiet `00c1b548cfc8-wave1` completed unsuccessfully: six clients had
six or seven failures, and fcstor009 had four failures; each run also reported
seven NOTRUN cases after a timed-out worker group survived cancellation. The
failures were timeout/worker cancellation outcomes, not observed byte mismatches.
All 21 raw evidence files are retained. Separate diagnostic waves sampled packed
flush/publication/retirement waiting for RPCs, and many server handlers waiting
on Raft read/apply progress. The query publisher still refuses transaction-unsafe
cuts during mutation bursts. These diagnostic timings are not acceptance timings.

`45a1046034ff` uses fresh GETCHUNKS to discover the exact generation and prove
base absence in one RPC. Its full Linux suite passed and all 659 tracked sources
match the isolated committed stage; all ten other staged builds passed.

`a48076c4e937` extends the existing 16 retained snapshot slots to stats commands.
A stats invocation now uses one nonce and captured catalog across LOOKUP/OPEN,
then explicitly closes the token. Every request still performs fresh quorum,
capability and ancestor/permission checks; reply, memory, disk and lifetime caps
are unchanged. Legacy nonpaged stats remains supported. The actual production
serializer gate proves valid legacy/pinned JSON; command round trips test stats
open/close and reject filtered, multi-result or nonzero-cursor stats requests.
Local command and snapshot lifetime gates pass. Full Linux validation and staged
builds are running; this revision is not yet deployed or accepted.


## Release integration checkpoint and command namespace

Code checkpoint: `a48076c4e9373ea47c143c231e2b3cdb3c1e20fe`,
264 commits ahead of captured `devel` (none missing from it). Full Linux suite
and all ten other staged builds pass; 660 tracked source/build/gate files match
the validation archive. Normal deployment is underway. This is a code checkpoint,
not packing activation/release acceptance. The deployed `00c1b548cfc8` quiet
concurrent mounted failure evidence remains controlling until retested.

The shared metadata command namespace already reserves the packing block in
`include/efs/meta_cmd.h`: 45 PACK_ADMIT, 46 PACK_STATUS (read only),
47 PACK_UPLOAD_RETIRE, 48 PACK_GC_ACK, 49 PACK_CANCEL, 50 PACK_GC_RETIRE,
and 51 PACK_GC_VIEW (read only). These IDs have been deployed and emitted into
ORCD Raft logs. Do not renumber/reuse them for append-completion or another
command during integration. A proposed undeployed append-completion tag 52 is
outside this reserved block; the release coordinator must audit its uniqueness
against the combined header before deployment.

Compatibility is explicit: packed upload manifests, typed fences, packed mapping
fields and packed PUT/capability RPCs require capable participants and the durable
minimum-reader gate. Mixed-version capability failure must refuse packing before
accepting application bytes. Ordinary writes/reads remain available independently
of the opt-in mounted packing switch. Stats now uses v4 retained snapshots;
legacy stats paths still work. Mixed old servers do not provide new v4 stats
support and must be upgraded together with new command clients.

Remaining release gates: mounted concurrent POSIX zero-failure acceptance,
new pinned-stats live refresh/permission/close tests, fresh final-revision cold
and fault verification, mixed ordinary/packed coverage, terminal-CAS rejection
rebase handling, and at least three matched OFF/ON benchmark pairs with all-file
application-byte hashes and physical fragment/parity accounting. The unit
16×512-byte allocation result is not mounted performance evidence.


`a48076c4e937` completed verified normal deployment on all eleven ORCD nodes.
Every live executable identity, installed inode, UID and RDMA configuration check
passes. Retained cold mixed fixture passes 27/27, and the old last-reference
shared object remains physically absent on all four storage members. The new
stats live gate passes identical same-nonce bytes across a real publication
refresh, mounted command execution, current permission revocation, exact close
and stale-token refusal.

The quiet `a48076c4e937-wave1` concurrent run still fails: fcstor007/010 have
208 PASS and five FAIL; fcstor008 has 206 PASS/seven FAIL; fcstor009/011/012/013
have 209 PASS/four FAIL. Each has one expected SKIP and seven NOTRUN cases after
timed-out worker cancellation. This is not a release acceptance checkpoint.
The stats identity fix alone does not close the transaction-cut starvation or
packing/namespace throughput failures. Raw results and the live stats proof
are preserved alongside revision-specific full Linux source manifests.
