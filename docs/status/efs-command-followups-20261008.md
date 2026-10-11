# W91/W92 command follow-ups — Oct 8

Branch `devel-find-stats`; clean NUC candidate
`ecabac6ff587-commands-d2573704`. The independent `/Users/mike/git/efs`
checkout is untouched. Source patch/build identity and raw fixture evidence:
[results](../../results/measure/20261008-efs-followups).

Latest: [server startup disablement and catch-up](efs-command-policy-20261008.md).
The [Oct 10 durable single-export policy candidate](efs-query-persistent-policy-20261010.md) supersedes the historical remote-policy gap below; scale and platform acceptance remain distinct.

## Repairs

- Refresh now fences **transaction activity**, rather than every metadata write,
  across the post-capture ReadIndex barriers. Ordinary atomic create/lane writes
  no longer force rejection of an otherwise safe immutable source. The KV fence
  advances for intent, guard, reduction and decision writes/deletions; it is
  captured under the same lock as the source, and saturation fails closed.
  Snapshot import generation changes also reject the cut. Pending transaction
  records still refuse catalog construction; full-group authority/import checks
  remain mandatory. Neither barriers nor partial-transaction refusal was removed.
- Immediate `ls` charges each scanned directory record, including tombstones and
  page lookahead, against its visit budget. A per-request KV wrapper keeps this
  accounting out of the shared immutable catalog. Cancellation/deadline checks
  run even when no visible name is emitted. Failure never delivers partial JSON.
- Published immutable name indexes, partition record arrays/arenas and node
  arrays release geometric allocation slack when shrinking succeeds. A failed
  shrink retains the original allocation and its full accounting. Existing
  candidate caps and transient allocation checks remain; this is not an RSS cap
  or a claim that every million-name layout fits the default budget.
- JSON adds `pending_changes_at_least` (saturated at N),
  `pending_age_ms_at_sample`, `lag_sample_ns`, and `refresh_overdue`.
  These describe a sampled state; they change on pending-count/overdue transitions
  and publication, rather than making unchanged result bytes change on every
  read. Clients can compute observation age from `lag_sample_ns`. A quiet tree
  with an old source timestamp does not become overdue. The first-dirty timer is
  detection time, not the exact commit time; collection pauses during builds.

## Named-build gates

Catalog units include ordinary-write versus transaction prepare/resolve fence
behavior, immutable pinned fence values, pending-source refusal, tombstone-only
scan exhaustion and cancellation before emission. Four-node TCP/buffered private
fixture includes two regular mounts, command-source and invocation checks, raw
names, permission changes, immutable descriptors, collision precedence, identity
exhaustion/recovery, POSIX and peer suites, full-group publisher loss and persisted
rejoin, sustained small-payload writers with exact file/byte oracle, noexec plus
explicit interpreter, a cold opted-out mount, and persisted restart oracle.

The named-build fixture passes all of those gates. Full suites: POSIX
216 pass / 0 fail / 1 skip; peer 64/64. Its final independent restart oracle is
6,131 files / 12 directories / 1,565,507 logical bytes. The load-only tree has
6,115 files and 1,565,440 bytes, created by eight workers; publication advances
while writers run. These are bounded regression workloads, not 400 GiB evidence.
Raw query opens retry only documented transient pre-open races; once a descriptor
is acquired, immutability checks never retry or replace it.

## Remaining acceptance and boundaries

W91/W92 must not be marked completely closed by this repair. Persistent
server-enforced per-export policy/versioned remote feature transitions are not
implemented: current explicit opt-in is still per mounted export. The cold
opt-out gate does not establish remote policy transitions. A comprehensive
leader/transport/fault matrix, macOS native execution and NFS gateway credentials/
path-resolution gates remain outstanding. NFS support is not advertised. Local
macOS native compilation currently encounters the existing Linux `fopencookie`
logging dependency; the clean native C builds/tests above run on NUC Linux.

The original staged 400 GiB **actual payload**, million-path capacity and
checksum acceptance remains open. Tiny-file generator payload does not satisfy
that target. Preserve the existing trees and measure memory/space before growing
payload; never substitute sparse size or inferred allocation for real data.
W90 expiry and W93–W96 performance work remain separate.

## Production rollout

NUC safely drained both mounts and all four daemons, installed the named build,
then restarted TCP/buffered with the existing 1024 MiB catalog cap and command
opt-in. Existing stores and generated trees are preserved. Previous executables
are in `/data1/efs/commands-rollback-d2573704`. The existing `tiny_files` screen
now runs eight workers into `/data1/efs/mnt/tiny-commands-d2573704`.

Cold production stats succeeded before restoring the generator. Under resumed
writes n2 publishes generations 27 and 28 at 17:01:17/19 UTC: 436,119/436,282
files, 1,937/1,940 directories, approximately 112 MB logical payload and
399,432,705/399,583,389 accounted catalog bytes, with builds 2,119/2,058 ms.
The pre-rollout n2 generation98 held 423,670 files and 558,073,928 accounted
bytes. This is about 28% less retained accounted memory despite more files;
allocator RSS and simultaneous candidate memory remain separate measurements.
A quiet cold source age near ten seconds before writes resumed is expected and
must not be reported as pending-write lag.

Seven production samples under eight-worker load advanced generation 39→46 and
file count 438,740→439,865. All refresh errors were zero; sampled source ages
were 1.882–3.611 seconds. A two-second refresh scheduling target does not include
unbounded speed guarantees: these approximately two-second catalog builds add
to capture/queue time. Raw JSON, publication logs, binary hashes, RSS/space and
deployment/build logs are retained in the results directory. The generator is
still running; no generated directory was removed.
