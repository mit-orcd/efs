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
