# W88/D29 publication replay package — Oct 8, 2026

[Current handoff](in-flight.md) · [Accepted D29 contract](../backlog/decision-contracts.md#d29--durable-publication-receipts)

## Implemented in this portion

The staged typed-publication endpoint now durably admits an immutable intent
before attempting its mapping mutation. A pending receipt commits **the intent**,
not successful publication, and does not assert that a mapping proposal has
committed. It survives process crash/restart and binds the same UUID, epoch,
sequence, FileID, authority and payload digest as the terminal result. Queries
still require fresh lane authority and session admission. Unknown outcomes
remain unknown; proposed indices/terms are never treated as commitment proof.

Pending receipts share the existing 64-per-stream admission bound. Already-owned
pending work can complete at that bound. A successful mapping and its final
receipt remain atomic; a losing mutation replaces pending with its exact durable
rejection. Pending receipts cannot retire or advance replay floors. Existing
final receipt and floor behavior is preserved.

The client RPC recognizes pending separately. Both actual dirty-cache completion
and ordered ACK consumption refuse to release pending requests/bytes. A pending
RPC response is not fsync success. This does not yet wire these staged helpers
into every production flush/REPORT path.

## AMD direct-I/O/TCP validation

[Raw evidence](../../results/measure/20261008-publication-pending/README.md):

- Full unit suite passes with `EXTRA_DEFS=-Werror`; no compiler warnings.
- Additional targeted tests pass after adding the exact W88 eight-span/fold,
  later full overwrite, trailer eviction and original retry sequence. Durable
  identity suppresses resurrection; retirement floors reject later old retries.
- Faulted admission/finalization preserves pending ownership; 64 pending receipts
  refuse excess admission but allow owned completion. Altered requests, missing
  session admission and fenced sessions are refused.
- Real four-daemon private fixture drops all admission replies, observes pending,
  refuses early retirement, kills/restarts all daemons, recovers pending from
  authoritative state, rejects an altered retry and records final rejection and
  retirement. Fixture: `/data1/efs/evidence/pending-receipts-20261008-r2`.
- Installed four-daemon/two-mount AMD service remains running; no mounted export
  or installed stores/binaries changed. No new POSIX run or hardware durability
  claim; the live publication intent deliberately has an invalid FileID and
  performs no fragment PUTs.

The first probe build failed `-Werror` on test-only misleading indentation;
corrected before any fixture daemons started. An earlier full-unit attempt hit
that same harness warning; the corrected full suite passed. No runtime failures
were discarded.

## Remaining package work

**W88 remains open on the active legacy REPORT path.** Replace that path with
immutable per-publication ownership and exact per-item results before claiming
end-to-end protection. The production path still owes lost final ACK, overlapping
later writes, history eviction, retained objects and GC/re-PUT, leader changes,
partial batches and exact cold bytes. D27 whole-call deadline/fault/RSS and W72
production sessions remain open. D25 public activation remains disabled.

This portion deliberately uses separate admission/publication proposals and
retains all existing validation. It adds Raft/read cost to the staged endpoint;
measure before activation rather than claiming a performance improvement. New
command 31 and pending receipt encoding require updated participants; mixed-old
binary replay/downgrade is not accepted by these tests. The accepted D29 committed-mapping/apply-pending proof remains separate:
durable intent admission alone does not establish it. Applied receipt locators
are now implemented as described below.


Follow-up [writeback hour](writeback-hour-20261008.md) adds actual immutable
fragment PUTs and exact cold FUSE reads through eight-span history eviction,
physical GC, identical old-object re-PUT and durable retirement-floor recovery.
Same-group and explicitly bootstrapped cross-group typed endpoints pass. These
staged gates do not activate or close the production REPORT/session integration.


## Agent 4 follow-up: durable receipt locators

Agent 4 now owns D29/W88 alongside D30/W23/W89, with receipt/replay correctness
before admission/pacing integration. The staged endpoint persists the applying
Raft group, index and term with each new receipt. These come from the replicated
apply callback, never from client fields or a proposed index. Replaying an exact
identity preserves the first terminal locator; pending-to-terminal replacement
stores the mapping/verdict locator atomically with its result. The immutable
intent digest excludes the locator.

Version 2 receipt values are 64 bytes and include a reserved zero word. Existing
40-byte version 1 values remain readable and return an absent locator. Retired
identities resolved from replay floors also return an absent locator. Malformed
locators and receipt encodings are rejected. The staged RPC reply grows to carry
the locator; all communicating participants must be upgraded together. This is
not a mixed-binary wire compatibility or downgrade guarantee.

[AMD evidence](../../results/measure/20261008-publication-locators/README.md)
records a passing warning-free build and full unit suite, plus a four-daemon
lost-admission-ACK/crash/restart test. Pending group 0/index 7/term 1 survives
restart unchanged; the later rejection is group 0/index 12/term 2. Tests also
cover replay preserving the original terminal locator, legacy values, malformed
wire locators and metadata consistency checking.

A pending locator proves durable intent admission only. It cannot authorize byte
release or fsync success. Mapping-committed/apply-pending proof and immutable
ownership on the production REPORT path remain open; D29/W88 and D30 activation
are not complete. The installed AMD service remains at its earlier deployment,
with pressure admission disabled; these tests use private stores and ports.


## Agent 4 follow-up: committed command while apply is pending

The typed endpoint now distinguishes `APPLY_PENDING` (state 5) from admitted
intent `PENDING` (state 4). On an apply timeout it obtains a fresh correlated
quorum round from the current leader and searches at most 64 committed,
unapplied log entries for the exact immutable mapping command. It checks the
already-applied session admission and intent digest. Admission-only entries,
proposed/uncommitted indices, changed payloads and missing retained commands
cannot provide this proof. A follower redirects to the leader; absence of proof
remains unknown. Retirement cannot use this fallback.

The quorum-only inspection preserves the existing own-term commitment and
configuration checks. It is independent of local apply; all ordinary KV reads
still require apply through their fresh ReadIndex. The proof establishes command
commitment, not a successful CAS or a fresh terminal KV verdict. The eventual
mapping may reject. Clients refuse byte release and ordered ACK consumption for
both pending states. The final receipt uses the same actual mapping index/term,
including after cold replay in a later leadership term.

[AMD evidence](../../results/measure/20261008-publication-apply-pending/README.md)
includes full units, cache/ACK ownership tests, fault release and all-daemon
crash after committed mapping but before apply. Only private daemons/stores are
faulted. The live intent deliberately rejects; no hardware durability or active
production REPORT claim follows from it.

### Production REPORT integration findings

The current production path still cannot be activated as typed publication by
changing its RPC call alone. `put_id` retains one mutable latest PUT per chunk;
newer PUTs can replace that identity. REPORT reconstructs base/size on later
flushes, and aggregate STALE triggers a whole-set repull/replay. Direct full
writes can release their body after fragment PUT before metadata publication.
FileID is absent from the legacy inode cache, and production mount sessions are
not established for typed publication. These are concrete ownership/admission
requirements, not receipt-proof defects.

The remaining migration must capture FileID/authority before byte admission,
retain a budgeted immutable request and accepted bytes independently of newer
writes, resolve and consume each exact terminal result before any rebase, and
retain ordered retirement ownership across lost ACKs. It must cover both write
entry points and both flush paths, including zero/full overwrites. Existing
D25 typed helpers cannot adopt previously accepted legacy bytes. D25 public
logical resize remains disabled; completing D29 must not silently enable it.

D29/W88 and D30 default activation remain open until that migration and its
real-byte FUSE fault/recovery gates pass. No new installed deployment is claimed.

**Raw evidence note (2026-10-23):** the bulk data in `pending-receipts-20261008-r2` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
