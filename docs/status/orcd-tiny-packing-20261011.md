# ORCD priority: production tiny-file packing — W95 / P4.1

October 11, 2026. Implementation branch: `devel-orcd`, based on `devel`
`82f0f59d`. This is the first ORCD work item; production packing remains open.

## Problem and correction to the earlier estimate

The ordinary tiny-write path in `src/client/write.c` encodes an entire 128 KiB
chunk even for a short span. Each of its three 2+1 fragments is 64 KiB; the
file storage backend adds a 4 KiB checksum tail. Thus one retained generation
occupies 208,896 bytes, or **408 times a 512-byte application payload**, before
metadata, filesystem directories, WALs and obsolete generations. That is a
format calculation, not a measured total-cluster amplification ratio. Smaller
payloads have a larger ratio. FUSE stat block counts are synthetic and cannot
establish underlying physical usage.

A live sample on fcstor007 is `/tiny-files3/file_000000.txt`, inode 20650,
486 bytes. GETCHUNKS returned one span, generation 3379240209574875210,
nodes 4/1/2. Audit the exact fragment paths across those nodes and configured
roots, recording `st_size` and `st_blocks * 512`; do not walk millions of files.
All three exact files were found: each is 69,632 bytes with 136 allocated
512-byte blocks. This sample therefore uses 208,896 allocated bytes, or
429.827 times its 486-byte payload. GC or replacement can invalidate an
older mapping. Evidence lives in `results/measure/20261011-orcd-tiny-packing/`.

Existing documentation already calls for small-file packing. W95's independent
immutable-body/index experiment passes finite recovery and churn gates; P4.1's
server container backend packs existing fragments into larger files. Neither
establishes production packing of tiny logical payloads. Putting padded 64 KiB
fragments into containers reduces pathname work but retains payload padding.
Legacy `efs_inode.pack_ino/pack_off/pack_len` fields do not constitute the current
Raft format: `src/meta/meta_apply.c` does not serialize them. Simply enabling
those fields would omit durable publication and shared-object reclamation.

## Selected direction and implementation sequence

1. **Measure a baseline first.** Use the attached `tiny-files` screen on
   **fcstor007** (the requested fcstir007 appears to be a spelling error).
   Preserve its existing run. Record interval files/s, payload distribution,
   open/write/close latency, fragment PUT bytes/count, physical allocation,
   metadata/WAL writes, compaction, client/server CPU and memory. Sample mappings
   and exact physical paths. Separately measure CREATE waiting; packing alone
   cannot eliminate metadata round trips.
2. **Pack logical records before encoding.** Use bounded immutable segment
   objects containing many small payloads, record identities, generations and
   digests. Encode the shared body once with existing 2+1 protection. Choose
   fragment lengths in 4 KiB units for partially filled segments, rather than
   always padding every individual file to 128 KiB. Keep large files on the
   existing path. Reuse W95's demonstrated publication principles; its private
   manifest is a model, not the distributed production index.
3. **Add a versioned authoritative mapping.** Each file version needs segment
   identity/generation, record offset/length, geometry and integrity identity.
   Extend actual Raft serialization, replay, GETCHUNKS/client decoding and cold
   reads. Bind publication to the current session, content epoch, durable PUT
   receipts and expected previous version. Batch updates within existing Raft
   groups; a pack spanning groups must not require a new global transaction.
   Define safe retention for partially published cross-group packs before
   enabling them; initially coalesce within one publication group.
4. **Bound packing delay and resources.** Flush on a short configurable deadline,
   capacity, explicit sync and shutdown. Start with a measured 1–5 ms deadline
   sweep, not an assumed optimum. Sixteen workers blocked in close must not
   wait for 256 records to fill a segment. Count retained bodies, queued writes,
   index state and recovery scratch against admission limits. Sync callers wait
   for required data durability and metadata publication; asynchronous callers
   retain their existing contractual guarantees. Never count buffered completion
   as equivalent to durable completion.
5. **Implement shared-object lifecycle.** Overwrite/truncate/promotion publish a
   new version with compare-and-swap fencing. Hard links share inode identity.
   Unlink retires one reference, not its neighbors' payloads. GC needs durable
   reference/publication accounting, pending-ticket retention and safe recovery;
   segment reclamation waits until all references and recovery obligations are
   gone. Compact live records into new segments and republish before retiring
   old ones. Quotas include reservations, shared bodies and garbage.
6. **Gate and deploy an opt-in mixed format.** Existing files remain readable;
   new-format writes require compatible readers/servers and explicit feature
   negotiation. Deploy first to private ORCD roots/ports, then coordinate the
   seven-client rollout. No wipe or forced rewrite is required. Revert new writes
   independently from support for reading already published packs.

## Expected geometry, not a performance result

For the staged format's 72-byte record entry, 96-byte pack header and
32-byte body digest,
round each data fragment up to 4 KiB, then add one 4 KiB checksum tail to each
of three fragments. The table excludes metadata, index/WAL writes and garbage;
this is a staged packed-object format, not an activated production wire format.

| 512-byte files per pack | Application bytes | Stored fragment bytes | Ratio | Reduction versus separate fragments |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 512 | 24,576 | 48× | 8.5× |
| 16 | 8,192 | 36,864 | 4.5× | 90.7× |
| 64 | 32,768 | 73,728 | 2.25× | 181.3× |
| 128 | 65,536 | 135,168 | 2.0625× | 197.8× |

These predictions require variable segment geometry; the current minimum chunk
validator rejects such smaller chunks. Introduce a distinct segment geometry
contract rather than weakening ordinary chunk validation globally. A lone
1-byte synchronous write cannot have the same ratio as a well-filled cohort.
A full manifest snapshot per publication can erase data savings: measure and
bound index/WAL overhead, using incremental authoritative rows in production.

## Acceptance and benchmark procedure

First correctness gates cover 1/17/512-byte records, singleton/deadline cohorts,
full and boundary-sized packs, close/fsync/O_SYNC, concurrent writes, append,
truncate, hard links, unlink and large-file promotion. Inject failure at data
write/sync, publication, ACK, reference retirement and GC. Cold restart and
2-of-3 degraded reads must recover exact acknowledged bytes; stale tickets and
conditional deletion must not delete a newer version or neighboring record.
Test leader changes, follower replay, mixed formats, quota exhaustion, bounded
memory, ENOSPC and corrupt/missing segment evidence.

Use the existing screen as the observational baseline. For matched A/B runs,
use fresh sibling directories, the same tiny_files.py revision, 16 workers,
1–512-byte distribution, directory geometry and a fixed file count. Record
one-minute rates and p50/p95/p99 through the sustained slow phase, not just the
initial burst. Alternate baseline/candidate and retain at least three runs.
A native-XFS control provides context, not equal distributed durability.
Verification payload hashes are a separate correctness gate from rate sampling.

Initial geometry acceptance target: at least **20× less fragment storage** than
separate legacy fragments for 16-or-more 512-byte records per cohort, including
headers, padding and tails. Report singleton costs and observed cohort sizes.
Report total durable bytes per application byte separately, including index,
metadata and garbage. Seek at least 2× sustained files/s on matched ORCD runs;
this is an experiment target, not a promised result or a reason to weaken gates.
Preserve EBUSY/timeouts and refresh failures as failures. Packing acceptance
requires unchanged POSIX/peer/cold-recovery and hardware RDMA gates.

## ORCD work order

| Order | Items | Remaining work |
| --- | --- | --- |
| 1 | W95 / P4.1 | Physical baseline, production packed representation and all publication/recovery/GC gates above |
| 2 | W109 / W97; W66 / W23 / D30 | Separate CREATE/admission/compaction costs; investigate ORCD EBUSY, deep-directory/query timeouts and lane-sweep errors; repeat original full-suite deadlines |
| 3 | W108 | Resolve owner names in `.efs/ls -l` and `.efs/find -l`, numeric option, cache/fallback, rebuilt-client/live gates |
| 4 | W110; W99 / W42; W106 | Remaining runtime shared-domain capacity and designated hardware RDMA fault/scale gates |

This is a work order, not a claim that every historic mention of ORCD is a new
assignment or that any item is closed. Coordinate ownership and live deployment
with `efs-coding 1 - orcd`; do not interrupt its current acceptance runs.

## Branch implementation checkpoint

The first code step adds `efs_segment_geometry`, `efs_encode_segment` and
`efs_decode_segment` to the existing 2+1 codec. Segment sizes are 8 KiB multiples
up to 128 KiB, giving 4 KiB-aligned fragments. Ordinary chunk callers retain
exactly their previous geometry checks. No client PUT, metadata mapping or
storage reader selects this API yet; production format integration remains open.
The new test covers every supported geometry and every missing-fragment choice,
padding, half boundaries, malformed sizes, and equivalence with the old codec at
128 KiB. It and the existing erasure regression pass locally with ASan/UBSan.

`tools/tiny_fragment_audit.py` checks only authoritative exact paths across
configured roots, reporting apparent and allocated bytes. Missing, duplicate,
wrong-sized or nonregular fragment files fail the audit; missing files are never
reported as zero allocation. Its path/bounds/failure regressions pass locally.
Both tests are registered in `make test`.

The user explicitly authorized ORCD source transfer/build/test/deployment and
continued ownership. The private Linux codec tests then passed on fcstor006
under UID 246111. The audit tool independently confirms all three exact physical
fragments. [Linux gate](../../results/measure/20261011-orcd-tiny-packing/linux-codec-gate.txt)
and [timestamped allocation audits](../../results/measure/20261011-orcd-tiny-packing/fragment-audits.json).
No daemon switch or benchmark interruption occurred for those gates.

The next code step introduces a bounded immutable packed-object format with
big-endian fields, inode identity/generation, content epoch, record generation,
segment identity/generation, publication group, geometry and per-record digests.
Header and body checksums cover identities and ranges; padding must be zero.
A reader matches the authoritative reference before returning bytes. Unknown
versions, duplicated records, corrupt bodies, noncanonical ranges and stale
references fail closed. References have a canonical 96-byte encoding, intended
for a separately versioned metadata record; no native structs are persisted.
Local ASan/UBSan gates recover every member through every missing EC fragment,
cover 1/17/512/4096-byte payloads, bounded maximum record count, corrupt checksums
and structures, reference mismatches and atomic rejection on capacity failure.

Durable Raft publication/ownership, real GETCHUNKS/client selection, upload
receipts, shared-object GC and matched A/B remain open. Codec/format unit gates
must not be described as production packing or a throughput gain.

The committed format/codec snapshot `72b1a88fbf09` passes complete Linux
`make test` on fcstor006 after regenerating derived architecture documents.
The initial generated-document failure is retained in the remote unit log;
[successful full-suite tail](../../results/measure/20261011-orcd-tiny-packing/full-linux-unit.txt).

The next storage step adds a distinct `data/packs-v1/` namespace and explicit
fragment geometry to the files backend. The ordinary direct writer forces
export-sized fragments; the new helper does not. It writes immutable objects
with exclusive creation, verifies digests, atomically reserves payload quota,
retains uncertain charges and re-establishes file/ancestor durability on retries.
It refuses container-backend and benchmark-build writes initially. No production
RPC selects this helper. Individual-inode GC does not traverse this namespace;
future shared-object retirement needs its own authoritative upload fences.

A private development-overlay gate on fcstor006 exercises the actual storage
entry points with sixteen 512-byte payloads. Both buffered and direct modes
allocate **36,864 bytes** for the packed fragments, versus **3,342,336 bytes**
for sixteen separate legacy files: **90.6667 times less fragment allocation**.
The gate verifies cold retained records through every missing-fragment choice,
immutable retries after rerouting, file and directory sync failures, quota
limits, restart accounting, duplicate-root rejection and inode-GC isolation.
[Private storage gate](../../results/measure/20261011-orcd-tiny-packing/linux-packed-store-dev.txt).
This uses private local `/tmp` roots on one ORCD host, not distributed hardware
RDMA, the data-volume benchmark, or end-to-end files/s acceptance. The committed storage snapshot `ebb7ab464985` then passed the complete Linux
suite on fcstor006, with zero compiler warnings. The [audit](../../results/measure/20261011-orcd-tiny-packing/linux-unit-audit.txt)
records Linux 5.14.0-687.39.1.el9_8, private roots on local XFS `/dev/md0`,
source identity and unchanged live runtime `52bc8edb`. No mounted tiny-file
packing is activated yet.

The shared-object lifetime planner now binds each record's canonical reference
and the immutable body to a captured owner/session and member mask. Pending
records retain the body; publication makes a record live; release/cancellation
are terminal and cannot be undone by publication replay. Collection requires
no pending or live records, then durable fence ACKs from every captured member
before any deletion ACK, including a fourth member that may hold a stale PUT.
Its versioned big-endian ledger has a checksum and bounded size (maximum
17,056 bytes for 512 records; 688 bytes for sixteen). Unknown/malformed states
fail closed. No production RPC or Raft handler selects it yet.

This is an internal transition planner, not an authority oracle: real apply
must verify the session, PUT receipts and mapping CAS, and commit mapping,
publication receipt and shared ledger together. Release requires the exact
current mapping; cancellation requires authoritative revocation. Several
updates to one shared object must modify a single loaded ledger in a batch
to prevent lost references. Local ASan/UBSan tests cover neighbor retention,
terminal replay, all-member fence/delete ordering, canonical corruption and
an atomic KV batch failure that leaves both mapping and ledger unchanged.
The rebuilt W108 candidate `757b0a56d2ed` also passes full Linux `make test`;
live mounts remain unchanged pending the production integration gates.

The committed ledger snapshot `5a5caee9ceb1` passes complete Linux `make test`
on fcstor006. Its persistent-KV gate publishes sixteen mappings plus the ledger
and a synthetic receipt in one durable batch, exits without KV close/flush,
and recovers all records from the WAL. After fifteen releases and cold
compaction, the final neighbor still prevents collection. Further abrupt exits
recover partial fence/delete ACK sets and terminal retirement without allowing
publication replay to revive a reference. These are local KV/planner gates,
not a distributed Raft/RPC publication or POSIX close acceptance result.

Storage upload fencing is implemented independently of inode death pages. A
certificate identifies export, segment ID and generation with a checksum.
A shared generation lock serializes uploads against retirement, and fence ACKs
require both file and ancestor persistence, including retries. Fences do not
delete reader bytes. Their stable primary-root namespace must survive restart;
pruning requires a future authoritative admission floor. No external retirement
RPC or physical pack GC is enabled by this step.

A private fcstor006 overlay passes the packed-storage tests in both buffered
and direct modes with these fences. Failed file and ancestor barriers remain
failures on retry; a valid visible fence immediately rejects every fragment
upload, and reopened storage still rejects the old generation after private
files are removed. Fencing does not delete existing reader bytes, does not
poison a different export/new generation, and corruption refuses both PUT and
fence ACK. The earlier 90.6667× allocation control still passes.
[Private typed-fence gate](../../results/measure/20261011-orcd-tiny-packing/linux-packed-fences-dev.txt).

The staged minimum-reader barrier persists a monotonic capability in the
authoritative KV MANIFEST before format-dependent metadata can be published.
Unmarked stores retain `KVLSM1`; marked stores use `KVLSM2` with the required
capability. Existing segment/WAL data stays intact. Flush/compaction retain the
requirement; switching off future packed writes cannot clear it. The marker
API checks file and directory barriers on retries and can run under serialized
apply ownership, even if the subsequent metadata batch aborts. Unknown flags,
versions, malformed headers and wrapped numeric capabilities refuse KV open.
No current mounted writer calls it; all owning nodes and snapshot/import paths
still need the barrier before production publication, plus live build/feature
negotiation. This is not an automatic migration or a completed rollback gate.

Private ORCD tests pass persistence failures, retries, aborted apply, cold
recovery and unknown-format refusal. A probe linked to the actual pre-barrier
`5a5caee9ceb1` library refuses a private marked store (exit 77); the updated
library successfully reopens the same store.
[Fault/recovery gate](../../results/measure/20261011-orcd-tiny-packing/linux-reader-gate-dev.txt)
and [actual legacy-reader proof](../../results/measure/20261011-orcd-tiny-packing/linux-legacy-reader-proof.txt).

The minimum-reader snapshot `b8d35eaa2c08` passes complete Linux `make test`
on fcstor006, with zero build warnings. A subsequent focused refinement accepts
only canonical body-offset order for ledger initialization and reuses reader
barriers proven successfully in the current process. Failed/uncertain barriers
and reopened stores cannot infer that proof from a visible marker; they repeat
the checked barriers. This avoids per-pack manifest sync after a successful
activation barrier. Local ASan/UBSan and focused Linux ledger, reader-guard and
cold-KV recovery tests pass.
[Focused gate](../../results/measure/20261011-orcd-tiny-packing/linux-reader-confirmed-dev.txt).

At the final observational checkpoint, 05:50:12 UTC, the same fcstor007 Python
PID 1381139 was still running: 1,881,198 / 10,000,000 files, 4,918.5 seconds,
382 files/s cumulative. Query policy read back mask 3, version 2.
[Live checkpoint](../../results/measure/20261011-orcd-tiny-packing/live-checkpoint.txt).
Mounted writes still use legacy fragments. Production admission/session and
receipt checks, versioned mapping/GETCHUNKS reads, bounded client coalescing,
shared reclamation and matched ORCD throughput acceptance remain necessary.

## Authoritative reference and publication checkpoint · Oct 11

The staged profile `PACK_K2F1` extends the ordinary 128-byte base metadata image
with one canonical 96-byte packed reference. Ordinary delta records follow that
extension, rather than overwriting it. Metadata reads bind the reference to the
row's inode, inode generation, chunk zero and owning publication group, and
reject mismatched record generations/epochs, truncated references and malformed
trailers. Legacy base images retain their existing encoding. Private marked LSM
stores retain the packed reference and mixed delta list through compaction and
cold reopen; no new wire format or mounted writer is activated by this change.

`PACK_OBJECT` (kind 36) reserves a group-anchor key for an immutable segment
identity. Its checksummed `EFSPMAP1` value binds placement and fragment checksums
to the existing shared lifetime ledger. A bounded mapping planner loads each
shared object once per cohort, checks transaction exclusion and local upload
owner epoch for new references, and returns ledger changes to commit in the
same batch as mappings and durable receipts. It does not establish admission,
prove durable PUT or grant permission to publish a new packed object.

The existing publication apply now uses that planner when an authoritative
packed base is present: ordinary deltas preserve its LIVE reference, and
promotion to an ordinary full chunk retires the exact old reference atomically
with the new mapping and durable publication receipt. Legacy publications take
the existing path without additional ledger/mapping lookups. Lost-ACK retries
return the stored receipt without restoring released ownership. Unsupported
packed retirement paths still fail BUSY instead of putting shared fragments
into the ordinary per-inode GC queue. Those paths must be connected before
activation; this guard is not completed shared-object GC.

Private ORCD tests cover actual chunk metadata keys, mixed deltas, corrupt image
refusal, multiple members sharing one ledger, failed atomic batches, prepared
ledger exclusion, retained neighbors, terminal-reference replay rejection,
actual publication promotion/receipt replay, abrupt WAL exit and cold
compaction. The complete Linux `make test` suite passes on fcstor006, and the
successful development build has zero compiler warnings.
[Development overlay gate and source manifest](../../results/measure/20261011-orcd-packed-metadata/).
This is a metadata integration checkpoint, not distributed packed PUT/GET,
mounted coalescing, physical reclamation or throughput acceptance.

Containers and logical packing compose at separate layers:
`logical files → packed segment → erasure-coded fragments → container extents`.
Existing fragment containers retain padding inside their input fragments. Packed
fragments are initially supported by the files backend only; the staged storage
helper explicitly rejects containers. Container support must preserve immutable
segment identity, typed upload fences and shared-reference retirement, and
reclaim extents only after the segment's captured-member fence/delete protocol.
It needs separate backend durability, recovery, allocation and survivor tests.

The user now explicitly permits erasing ORCD test data if needed for this work.
No stores were erased for these private gates. This authorization does not
extend to unrelated clusters or change their retention requirements.


## Admission, distributed reads and coalescing checkpoint · Oct 11

The development path now admits an immutable `EFSUPLD1` manifest through Raft,
with exact owner/session epoch, deterministic segment identity, placement,
checksums, record identities and captured membership. Group-local upload permits
are bounded to 128 outstanding cohorts per session. Fresh authority and the
stored immutable manifest resolve uncertain admission replies; absence never
permits a PUT. Completed permits retire in order behind a durable floor while
LIVE shared-object rows remain for later reclamation. Keeping permits until file
deletion would otherwise stop a writer after its first 128 live cohorts.

Typed packed PUT/GET RPCs persist actual variable-length fragments in the files
backend. PUT requires a fresh ACTIVE admission, current FileID/content epochs,
matching placement/checksum and the durable local session guard. GET validates
geometry, actual checksum tails and exact payload length. Initial packed
publication binds each reference into the intent digest, then commits the
mapping, LIVE ledger transition and publication receipt in one metadata batch.
The versioned GETCHUNKS path returns authoritative references; the client reads
any two valid fragments, verifies the immutable body/record digest, and
zero-fills the logical chunk before applying existing delta/fence semantics.
Legacy publication tickets retain their existing command encoding.

Packed reference retirement is now atomic with ordinary promotion, bounded and
historical lane sweeps, truncate range deletion, truncate tail promotion and
verified chunk materialization. Packed objects never enter per-inode fragment
GC. Tests cover failed cleanup batches, repeated cleanup, surviving neighbors,
last-reference transition and cold compaction. Physical shared-object deletion
requires a durable typed fence, checks exact generation/checksum/geometry,
serializes against PUT and repeats directory barriers on retries; quota and GC
accounting change only once. Its internal caller must still prove all captured
members fenced before physical deletion.

A bounded cohort primitive copies captured snapshots into caller-owned storage,
coalesces only within one publication group, flushes at capacity or deadline,
and supports explicit singleton sync/shutdown flush. Immutable owner/body/refs
survive retry without new identities. Its memory must be charged before byte
acceptance; the caller must pin each writer snapshot until durable publication
or fenced cancellation is reconciled. This primitive is exercised through the
private RPC driver; the mounted write-cache scheduler is not yet connected.

A private four-server TCP/direct-I/O cluster on ORCD port 19832 passes real
16-record coalesced upload/publication, durable receipts, retired permit probes,
degraded reads with either data server stopped, and all-server cold restart.
The actual FUSE client reads those packed files and writes/fsyncs an ordinary file
alongside them. Mounted promotion/grow/fsync, truncate-zero, unlink and hard-link
survival also pass; a cold client and all-server restart retains exact modified
ordinary bytes and all twelve surviving packed neighbors. An audit across the
four private nodes finds three 12,288-byte allocated fragments per original
16-record/8,192-byte cohort, or 4.5× payload before metadata/WAL/directories.
The separate eight-worker, 20-minute randomized physical soak
passes 299,520 cohorts, including buffered/direct I/O, retry PUT, degraded EC
reads, fences, GC and cold quota inventories. These are correctness gates,
not mounted tiny-file throughput acceptance. The full Linux regression suite
passes; a final focused refinement also rejects legacy session epoch zero.
AddressSanitizer passes the coalescing, packed reader and actual metadata
lifecycle implementations. UBSan could not link because the host lacks its
runtime library; no UBSan result is claimed.
[Source manifests, logs and private runtime hashes](../../results/measure/20261011-orcd-packed-integration/).

Snapshot import now durably installs the minimum-reader capability before
committing packed mappings or admission/lifetime rows, including prepare and
empty-diff imports. A permanent replicated capability row survives retirement
of the final object, preventing a snapshot from silently losing its format
requirement. Unsupported incoming versions fail before changing rows.

The native RPC record struct is extended in this development build. All private
servers and the tested client use matching binaries; an eventual live rollout
must drain and update every server/client together unless an explicit legacy
ABI translation/negotiation path is added. A minimum-reader disk marker does
not itself provide network feature negotiation. Packed read scratch is bounded
per call but must also be included in the mounted client's aggregate memory
budget before activation.

Production remains `c52a4643` on port 17432 with the legacy mounted write path.
No production stores were erased. Remaining activation work is the mounted
coalescing scheduler and accepted-byte/cache/close/fsync ownership integration;
a distributed shared-object GC and abandoned-PENDING cancellation coordinator;
peer capability negotiation and cache persistence review; then mixed POSIX,
fault/recovery and matched baseline/candidate throughput/physical-allocation
runs. The existing tiny-file benchmark still exercises legacy fragments, so
its 408× format amplification has not yet been removed from that workload.


Before measuring throughput, batch fresh record-authority validation within a
cohort's existing metadata group. The current correctness-first PUT checks
writer authority for each record separately, so a sixteen-record cohort can
perform sixteen fresh ReadIndex rounds per receiving fragment. Replace that
with one fresh same-group read barrier and transaction-aware checks of every
captured FileID/epoch, retaining the local session and object guards. Likewise
measure/batch same-group publication without weakening per-file CAS/receipts.
Otherwise data padding savings may coexist with increased metadata latency.
Sweep 1–5 ms cohort deadlines against actual close arrivals rather than assuming
sixteen workers always produce a full sixteen-record cohort.

## Next implementation checkpoint

Connect the cohort primitive to the existing retained writer/publication state,
rather than introducing a second completion contract. Only eligible fresh,
whole-file snapshots at chunk zero and at most 4,096 bytes enter a cohort;
existing files, append, oversized writes and unsupported backends use the
ordinary path. Allocate the cohort's storage and publication ownership before
accepting its bytes. A group-local scheduler seals on capacity, deadline or an
explicit close/fsync/O_SYNC/shutdown request, including a singleton. Keep each
captured FileID, epoch, writer snapshot sequence and publication identity pinned
while admission, PUT or publication is uncertain. Retry the same body and IDs;
consume only the exact durable per-record verdict, and retire upload permits in
order after all records have terminal states. A failed cohort must not silently
fall back with replacement identities after any ambiguous PUT/publication.

The packed decoder now accepts caller-supplied scratch, and mounted reads
allocate it through the existing bounded client body pool. Its maximum scratch
is three fragments plus one reconstructed body, or 2.5 times segment geometry.
The mounted-branch budget gate exercises real pool exhaustion: it refuses before
fetch, leaves output and retained body charges unchanged, honors the deadline,
and releases scratch on both failed and successful reads. The decoder gate also
rejects undersized or overlapping scratch. Standalone tools retain an allocating
convenience wrapper. Coalescer copies and wire/EC staging still need inclusion in
that aggregate budget; this change does not activate mounted packing.

Implement shared GC as a durable coordinator over the existing ledger phases.
Only RECLAIMING objects with no LIVE or PENDING slots may start deletion.
Obtain actual durable generation-specific fence ACKs from every captured member,
replicate those ACKs, then delete exact placement/checksum/geometry identities
and replicate actual durable delete/absence ACKs. A missing member is not an
ACK. Retire the object only after all required proofs and the ordered upload
floor are durable. For abandoned PENDING cohorts, prove completed authoritative
session revocation and each local rejection floor before cancellation; elapsed
time or transport loss cannot supply that proof. Replay the same captured
membership and identities after coordinator crashes or leadership changes.

Keep mounted writes disabled until peer capability/native ABI rollout and the
scheduler/GC correctness gates pass. Then deploy matching committed binaries to
all four servers and seven clients, run mixed packed/ordinary POSIX and peer
fault gates, and use fresh benchmark siblings for alternating baseline/candidate
runs. Measure cohort occupancy and deadline distribution, fragment bytes/count,
metadata/WAL writes and p50/p95/p99 durable close latency as well as files/s.
Packing can reduce fragment padding while CREATE, per-record ReadIndex or
publication coordination still limits throughput; those costs need separate
attribution and same-group batching.


The concrete writer entry points are `efs_dcache_begin_publication`,
`efs_dcache_publication_request_ticketed`, `efs_dcache_consume_publication` and
`efs_dcache_flush_ino` in `src/client/write.c`. Select packed versus ordinary
publication before `result_bound` freezes the token's identity/size/digest.
Patching a previously admitted ordinary request into a packed request would
change its immutable intent and must fail. Build a dedicated packed request
from the same captured snapshot and its sealed reference, then reuse the
existing exact receipt-consumption path. Release dcache shard locks before
waiting for cohort flush/network progress; otherwise blocked close callers
can prevent other writers from filling the cohort. Later writes remain owned
in the existing post-publication state until the first packed receipt resolves,
then promote through the ordinary path as required.


### Retained packed publication adapter

The retained writer token now holds the exact completed PUT record, including
its packed reference and flag. `efs_dcache_publication_report` constructs retries
from that immutable token rather than mutable cache placement. Its allocation
remains charged through `sizeof(efs_writer_state)` in the metadata budget.
The plan adapter accepts a packed PUT only for a fresh, absent base at chunk
zero whose single owned range starts at zero and exactly covers the tiny record.
It checks FileID, captured epoch, record generation, geometry and bounds before
making publication ready. Ordinary sparse merges retain their existing path.

The actual cache-function gate checks invalid identities, epoch, extent and
flags without releasing accepted bytes; it then changes cache placement and
retries after a lost result using the same packed reference and digest. Exact
matching durable completion releases the body. This is another integration
checkpoint, not mounted coalescer activation: the group scheduler, upload-permit
ownership and distributed shared-object GC remain required.
