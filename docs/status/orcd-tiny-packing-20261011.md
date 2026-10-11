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
