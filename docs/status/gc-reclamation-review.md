# GC reclamation review — Oct 7, 2026

[Status](README.md) · [In flight](in-flight.md)

Status: **correctness defects found; incident root cause not yet established**.
Queue identities: **W61** and **W62** are correctness bugs; **W63** is an
observability enhancement. The canonical queue is [the status index](README.md).
No new architectural decision is made by this review.

Initial incident review was read-only. Later repairs are committed in
`3a1b4a52`; the [GC implementation checkpoint](gc-implementation-20261007.md)
records private-store tests, the buffered append failure W87, and staged PUT
tickets W86/D31. Round 6 inspected source and the checkpoint, not its remote
raw fixture logs. Existing-service rollout and the incident's lost-reference
reconciliation remain unresolved. The descriptions below retain the
defect-bearing incident baseline, not a claim that the repaired defects remain
in current source.

## Incident evidence

The user deleted approximately 40 GiB through EFS and reported no reclaimed
space after ten minutes. Read-only observations at approximately 08:59–09:00
UTC on the xorinox cluster:

- xefs1 `/data1`: 118,970,560,512 bytes used; 1,221,414,912 available.
  Two samples at 08:59 and 09:00 had identical values.
- xefs2 `/data1`: 119,041,163,264 bytes used; 1,150,812,160 available.
- `efs-mgmt status` reports node usage 99.14 / 99.21 / 99.21 GiB;
  logical usage 198.41 GiB, logical free 1.59 GiB, all three nodes up.
- Deployed build: `bed7a83e-dirty`, built 2026-10-07T08:47:09Z;
  all three servers report identical versions. Local review HEAD:
  `128f6b7d19abca1609642d138abe1d99f9316c1b` with working changes.
- xefs1's last logged GC summary was 08:50:39.937 UTC:
  `ms=5900 reap=5900 frag=0 fscans=2 fkeys=514 femit=514`.
  Its log had not changed since 08:50:41 at the second observation.
- GC is not disabled in the service environment. Live kernel wait samples
  show sleeping/waiting threads, but do not identify a userspace deadlock.
- Direct SSH inspection of xefs3 was blocked by a missing trusted host key;
  its usage and health above came from the management protocol.

There is no pre-deletion physical-space measurement or identified deleted
inode in this review. These observations support investigating stalled or
lost reclamation, but do not establish the precise leak size or cause.
Log silence alone is not proof of a stuck GC thread: default `gc-pass`
logging emits only passes exceeding 5 ms. `reap`/`frag` are milliseconds;
`fkeys`/`femit` are scan activity, not confirmed freed fragments or bytes.

## W61 — Local GC proposals discard the apply verdict

In `src/server/raft_host.c`, `host_gc_propose()` calls
`host_bg_propose(..., verdict=0)`. For a locally led group this waits through
`host_wait_settled()`, which confirms applied index and checks a retained
term mismatch, but does not return `g->arc_rc` for that entry.
`host_gc_reap_pass()` treats this result as the lane-sweep success verdict.
After all apparent successes it proposes REAP_DONE.

`efs_meta_apply_lane_sweep()` can return errors, including BUSY from
transaction exclusion checks or an I/O error from its KV batches. Those
errors are logged by the apply path, but the local GC caller can still
receive EFS_OK. REAP_DONE can then remove the dead inode's only reap marker
while a lane still contains chunks that were never queued for fragment GC.
A successful log apply is insufficient evidence that lane cleanup succeeded.
This path existed in the original local review tree and the deployed xefs1
source inspected at incident time. The committed repair uses apply verdicts.
Foreign-group forwarding has different semantics: the leader reply carries
its apply verdict.

Required change: use exact entry/term apply verdicts for cleanup commands;
retain/retry markers on failure or unknown verdict. REAP_DONE must follow
confirmed successful sweeps. Do not merely increase the retry interval.

Acceptance: inject BUSY and I/O errors in a local lane sweep; assert the reap
marker survives and all fragments are eventually collected after the fault
clears. Repeat for forwarded sweeps and verdict-ring eviction/term changes.

## W62 — Batch capacity can drop delta GC records

In `src/meta/meta_apply.c`, `sweep_cb()` reserves only two slots before
queuing a chunk-key deletion and its base-generation GC record. It then
queues trailer delta generations until the item array fills, silently
breaking out of the delta loop when full. The original chunk-key deletion
is still committed, destroying the only trailer references for skipped
fragments. `SWEEP_CHUNKS * 2` provides 128 items; each chunk can require
1 deletion + 1 base GC + 8 delta GC items.

Concrete capacity case: twelve chunks with a base and eight live deltas
consume 120 slots. A thirteenth passes the two-slot check, uses two slots
for deletion/base, queues six deltas, and silently omits the remaining two.
A later pass cannot recover those references from the deleted chunk key.
The trailer-capacity pattern also appears in the truncate callback and
needs the same whole-record capacity review.

Required change: reserve room for the entire chunk's base and live deltas
before mutating the batch, or preserve the chunk record until every dead
generation has a durable GC record. Never silently skip live delta records.

Acceptance: sweep and truncate across a batch boundary with eight live
deltas per chunk; count every queued generation, complete GC, and verify
all corresponding fragment files are gone. Include retry/crash boundaries.
This defect is confirmed by control flow, but the incident's deleted files
have not been shown to contain delta trailers.

## W63 — Missing operational visibility

Default summaries cannot distinguish an empty queue from failed deletions,
missing export context, unsuccessful proposals, or a worker blocked before
completing a pass. Detailed per-fragment/reap/ack results require
`EFS_GC_DBG=1`; no pending-byte or reclaimed-byte measure is exposed by the
observed status command.

Validate and roll out the committed `gc-status` reporting and bounded
periodic progress/error reporting: pending reap/GC records,
fragments and bytes actually removed, retries by stage/error, last completed
pass, last successful deletion, and oldest outstanding marker. Keep slow
pass timing and scan statistics separate from progress counters.

## Next investigation

1. Preserve the current metadata and fragment references; do not delete
   `/data1` contents or manually retire reap/GC records.
2. Obtain userspace stacks and identify current group leaders, pending reap
   markers/GC records, held leases, and export context with read-only tools.
   Kernel futex samples alone do not establish which worker is blocked.
3. Trace an identified deleted inode through marker creation, each lane
   sweep verdict, GC records, checksum-conditional fragment deletion, ACK,
   quota decrement, and physical filesystem free-space changes.
4. Determine whether this incident lost markers through discarded apply
   errors, retains a real backlog, or has another cause. Correcting future
   sweeps does not recover references already lost; any orphan reconciliation
   must use authoritative live references and checksum/generation checks.
