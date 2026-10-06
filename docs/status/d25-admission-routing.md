# D25 writer admission routing — decision pending, Oct 6 2026

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

## Recommended next implementation: lane-local authority first

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

This is the recommended route and keeps the documented hot path intact. The
alternative is to activate the existing inode-plus-lane RPC temporarily,
explicitly accepting the added traffic, then replace it with lane-local views.
That alternative requires an accepted temporary exception to §7.3.

The writer-token validation fixes proceed independently. Neither alternative
has been activated and no logical truncate/history-retirement gate is claimed.
