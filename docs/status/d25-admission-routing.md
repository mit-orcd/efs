# D25 writer admission routing — accepted: lane-local authority, Oct 6 2026

[Queue](README.md) · [In flight](in-flight.md) · [Metadata scaling](metadata-scaling.md)

## Why activation needs this choice

The staged `server_raft_host_writer_view` in `src/server/raft_host.c` establishes
ReadIndex on the inode group and, when distinct, the chunk's lane group. It
then reads the inode's FileID/fence history and validates the lane stamp.
Calling this RPC for every admitted application write adds inode-authority
traffic even when data publication is lane-owned. No capacity number has been
measured for that proposed hot path.

[Architecture §7.3](../how-it-works/architecture.md) says ordinary writes never
consult the inode shard: a rare truncate pushes epoch/history to the active
lanes. Enabling the existing RPC per write would be a temporary departure from
that design, and intersects the user's metadata-leader scaling concern.

## Accepted implementation: lane-local authority first

- Replicate an admission authority record per active lane: FileID, current
  content epoch, complete retained fence history/floor and geometry. The
  existing lane epoch stamp alone cannot establish all of those facts.
- Bootstrap a new lane through the inode/lane transaction before its first
  admitted write, including lanes whose requested chunk is absent. Never
  infer epoch zero or current FileID from absence.
- Fence transactions install matching epoch/history on every active lane;
  later activation of an inactive lane installs the current authority.
  History retirement must update complete floors coherently and preserve
  retained dirty bytes when the available history cannot answer their ages.
- Serve subsequent admission views through the lane's leader and ReadIndex,
  with topology-independent routing. Do not keep a coordinator lookup in the
  ordinary lane read path; unresolved transaction state returns bounded BUSY.
- Feed validated views into cache admission before copying application bytes;
  snapshot bodies/ranges together and carry immutable plans through both flush
  paths, PUT and REPORT. No cached authority can replace this validation.
- Gate cold bootstrap, absent chunks, epoch races, lane-leader changes,
  transaction restart, history-floor changes and 100/1000-client metadata
  distribution. Check actual RPC routing as well as returned byte correctness.

The user accepted this route on Oct 6. It keeps the documented hot path intact. The
alternative is to activate the existing inode-plus-lane RPC temporarily,
explicitly accepting the added traffic, then replace it with lane-local views.
That alternative requires an accepted temporary exception to §7.3.

The writer-token validation fixes proceed independently. The lane-local implementation is staged; authoritative FUSE admission
has not been activated and no logical truncate/history-retirement gate is claimed.

## Implementation checkpoint

The metadata core now stores a versioned FileID/lane admission record with
chunk size, lane geometry, epoch and a conservative history-completeness floor.
Cold bootstrap captures the inode and lane, then freezes inode/history before
atomically preparing the lane stamp, copied history and authority record. It
activates the lane in the inode bitmap so subsequent logical resize includes
it. Existing populated lagging lanes fail BUSY; missing stamps with existing
chunks cannot be adopted. The existing transaction decision/resolution and
recovery paths carry these EXCL intents. An internal Raft adapter coordinates
bootstrap; the public FUSE write path does not invoke it yet.

`LANE_WRITER_VIEW` (109/110) routes directly to the requested lane and establishes
only that group's ReadIndex. It requires exact FileID and geometry, never reads
the inode and never contacts a transaction coordinator. Local unresolved intents
return BUSY even after COMMIT until resolution reaches the lane. Cold missing
authority returns NOT_FOUND, not epoch zero. Shrink PREPARE and the internal
single-authority fence update installed authority with stamp/history atomically.
A retired history with an obsolete floor fails closed rather than re-aging bytes.

Local host/RPC tests cover single-group ownership/forwarding, actual lane
routing, geometry, malformed replies and bounded retries. Linux metadata tests
cover bootstrap of holes, partial resolution, FileID reuse isolation, aborted
fences, storage failure and concurrent fence collections. This is a staged
foundation, not a completed D25 activation or scaling measurement.

Cold bootstrap (111/112) now validates export geometry before adoption; the
client admission helper invokes it only after lane NOT_FOUND. Cache admission
and snapshot APIs validate lane geometry and preserve accepted ownership under
the slot lock. Durable LSM bootstrap restart tests pass for COMMIT, ABORT and
undecided PREPARE; unresolved lane intents remain BUSY. See the
[NUC ten-round checkpoint](../../results/measure/20261006-nuc-ten-rounds/SUMMARY.md).

Next connect FUSE callers to those staged APIs and carry immutable epoch-owned
plans through both flush paths. Before activation, also complete coherent
history/floor retirement, dead-FileID lifecycle cleanup,
live sweep progress and the recorded 100/1000-client routing/byte gates. The
internal cold adapter currently requires a node hosting both relevant groups;
the ordinary lane read path has no such topology requirement.

The [publication continuation](../../results/measure/20261006-publication-ten-rounds/SUMMARY.md)
adds captured FileID validation at REPORT preflight and same-group durable apply,
exact base/list identity, immutable materialization and staged typed cache PUT/
REPORT lifecycle APIs. Failed PUT retains accepted bytes; ambiguous REPORT retains
its pending token. Activation first needs durable per-publication outcome recovery,
because the current aggregate reply cannot distinguish partial commit from a CAS
loser or an evicted apply verdict. Recommend per-record results plus durable
retry/status identity before wiring typed flush paths. No new client-scaling or
live activation result is claimed.

## Durable publication results — implemented, staged

The user accepted durable per-publication recovery before activation on Oct 6.
`PUBLICATION` (113/114) submits one immutable intent; `PUBLICATION_STATUS`
(115/116) recovers its exact outcome under the lane group's ReadIndex. Both
route to a host of that lane alone. Legacy aggregate REPORT is unchanged and
is never evidence for typed rebasing. These endpoints have no active FUSE caller.

Identity includes a mount UUID/session and unique snapshot sequence as well as
FileID, chunk, captured epoch, expected base, candidate object,
observed delta list, range, intended size, placement and checksums. Canonical
big-endian encoding is hashed with BLAKE3; padding, host time and routing are
excluded. Distinct writes of identical bytes have distinct operation identities;
reusing one identity with changed request fields fails closed. The cache binds size and digest before first send and reconstructs
that same request on retries, even when the inode size subsequently changes.

Apply records a successful outcome in the same atomic KV batch as mapping,
lane/inode updates and GC records. Terminal STALE/INVAL with no mutation is
also recorded through Raft apply. BUSY and storage errors are not terminal
results. The status RPC ignores the transient apply-result ring and queries
the exact durable record. A prior result is returned before consulting newer
fences or mappings; an old committed request is acknowledged without applying
its bytes again. A current mapping with no result cannot manufacture success.
Cold cross-group publication still requires lane bootstrap authority; unresolved
transactions remain BUSY.

Matching durable success releases only the captured ownership. Matching durable
rejection clears the pending identity but retains all accepted bytes and pins;
replanning then requires a fresh authoritative view. UNKNOWN, missing results,
transport failures and mismatched identities retain the token and bytes.

## Acknowledged receipt retirement — implemented, staged

`PUBLICATION_RETIRE` (117/118) acknowledges the exact immutable request through
its lane authority. Raft command 29 preserves the publication payload and digest
identity. The applier verifies the stored terminal result and retires only the
lowest outstanding sequence in that FileID/chunk/mount-session stream. A higher
acknowledgement returns BUSY until lower receipts are retired. Unknown requests
cannot advance the floor; a changed request fails digest validation.

Receipt deletion and a durable sequence floor are one atomic KV batch. Requests
at or below the floor return the distinct RETIRED state and cannot mutate data,
even after a crash, superseding write, or fence. Repeating an acknowledgement is
idempotent. RETIRED carries no original verdict or original digest proof: its
returned digest identifies the queried request only. It must never authorize
release or rebasing of accepted bytes. The typed cache therefore retains its
pending token, body and pin on this state.

Each stream admits at most 64 outstanding receipts; a new request at capacity
returns BUSY without publishing or manufacturing a terminal receipt. Retries of
existing receipts remain available at capacity. Enumeration stops after 65 live
receipts, allowing legacy oversized streams to drain in order. This bounds live
receipt count per stream, not total stream count or physical LSM tombstone work.
One compact floor survives per retired stream and never expires on a timer.

Integration must submit and acknowledge monotonically within each stream and
retain an acknowledgement retry queue until retirement is confirmed. Never ACK
past an older locally pending request simply because the server has not received
it yet. ACK only after applying the exact terminal result to local ownership;
retirement itself proves neither original commit nor rejection.

Activation still needs session-lifetime admission and cleanup. Use the existing
I23 session-fencing barrier to reject the old epoch on every touched lane before
reclaiming abandoned receipts/floors; erasing a floor first would reopen replay.
The staged publication endpoints do not yet enforce that session barrier, so no
abandoned-session/floor cleanup is enabled and no global storage bound is claimed.
Finish this admission/recovery lifecycle alongside the ACK queue before enabling
production flush callers. No new product decision is required for retaining
floors safely while staged.

Remaining: connect authoritative admission and both flush paths, coherent
lane-local mtime invalidation, live lane/leader-change and shrink/recovery gates,
and sweep/history retirement. The staged adapter preserves the existing lane
stamp rather than consulting the inode on every publish. No production activation
or live cluster acceptance is claimed. See the
[retirement validation checkpoint](../../results/measure/20261006-publication-retirement/SUMMARY.md).

## Ordered client retirement ownership — implemented, staged

A caller-serialized per-stream queue now retains each immutable request before
submission. It validates canonical digests, requires increasing sequences and
accepts only identical retries. Sixty-four occupied slots apply backpressure
without altering ownership. A later consumed result cannot retire past an
older unresolved local intent, including one not yet submitted to the server.
Only the oldest consumed COMMITTED/REJECTED result is eligible for ACK; an
exact RETIRED reply removes it. Unknown/transport errors, conflicting terminal
results and mismatched digests retain the queue entry. Lost ACK replies retry
the original request. Sequence high-water survives an empty queue.

Client allocation charges the whole queue to the existing metadata hard bound;
free refuses any retained receipt. Tests cover reordered completion, immutable
retries, twenty full ring cycles, capacity refusal, metadata failure/lifetime,
and actual KV publication/retirement with an older initially unsubmitted
intent and a lost retirement reply. These primitives are not yet attached to
public FUSE flushes. The integrating caller must register EVERY stream intent
in order before sending any publication and mark consumed only after cache
ownership has processed the matching terminal result. No process-crash
recovery or abandoned-session cleanup is claimed. I23 admission/fencing,
coherent mtime invalidation and both flush integrations remain prerequisites.

## I23 publication session admission — implemented, staged

PUBLICATION, PUBLICATION_STATUS and PUBLICATION_RETIRE now check lane-local
session admission under authoritative ReadIndex, including after resolving a
proposal outcome. Missing establishment returns BUSY; a durable local fence
returns transport STALE with UNKNOWN publication state. Neither authorizes
consuming/rebasing accepted bytes. Submit and retirement also gate inside
serialized Raft apply before any receipt lookup/mutation, closing a race with
an earlier FENCE_LOC command. Legacy REPORT remains separate.

Production ESTABLISH now verifies authoritative ACTIVE epoch and REGISTERed
shard membership on the session owner before proposing to the target shard.
It denies missing registration and FENCING/new-epoch admission. This owner
lookup happens at establishment, not on every publication. REGISTER precedes
the check, so a later BEGIN freezes that shard into its fence set; a delayed
old ESTABLISH is rejected by the ordered durable local floor.

`efs_session_reclaimable` requires both completed global barrier and a durable
local rejection floor above the abandoned epoch. Its caller must establish
authoritative session/target-shard views. It grants eligibility only; it does
not delete receipts or floors. They remain retained. No timeout-only cleanup,
epoch re-aging, automatic FUSE activation or abandoned-stream sweeper is added.

The NUC regression covers missing admission, ordered fencing, retained exact
receipts, rejection of both submit/retire after fencing, unknown client results,
restart after FENCE_LOC before global ACK, and subsequent epoch admission.
Automatic lane session admission belongs with the pending FUSE integration.
Next finish coherent lane-local mtime invalidation, then integrate both flush
paths and bounded abandoned-stream cleanup under the barrier.
