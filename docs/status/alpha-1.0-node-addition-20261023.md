# Alpha 1.0 — simple startup and safe node addition

**Accepted user amendment, October 23, 2026. Implementation OPEN; acceptance
NOTRUN.** This supersedes D39/D40's mandatory user-supplied member list,
fixed membership and explicit all-configured-member `raft-mkfs` startup
workflow. Other alpha decisions are unchanged. Alpha is always **2+1** on
3, 4 and more nodes. Online node addition is required; removal is deferred.

## Normal operator interface

Proposed CLI spelling, expressing the accepted workflow:

```sh
# First node: create a cluster in fresh storage
efsd 10.0.0.11:17432 /data/efs
# Additional node: discover and join
efsd --join /data/efs
# Explicit seed, including across subnets
efsd --join 10.0.0.11:17432 /data/efs
```

No mandatory cluster file, manually assigned node id, member count or coding
mode. Quota and other justified deployment inputs remain optional with safe
defaults. Restart uses persisted cluster/member identity and membership;
existing storage is never silently reformatted or reassigned to another cluster.

Discovery advertises the service endpoint on the local network, using the
interface's actual subnet rather than assuming /24. The joiner uses the
interface reaching the discovered/explicit seed and adopts the service port.
A fixed discovery endpoint can advertise a non-default service port. Multiple
cluster matches require an explicit seed; no match never creates a new
cluster as a fallback. The exact wire discovery mechanism remains to implement.

The first two nodes remain in formation. Initial service requires at least
three distinct storage members, agreement on the initial membership, both
metadata groups ready and the required 2+1 placement/durability conditions.
Formation is automatic; normal startup does not require a separate management
format command. Subsequent joins are additions to this formed cluster, not
another format operation. The initial seed is not a permanent coordinator:
any available member can handle or route joins after formation.

## Required invariants

- Discovery finds peers; it never defines quorum membership. Membership and
  automatic member identities are durably agreed, not inferred from liveness.
- Joining members are pending until required state and eligibility are installed.
  Duplicate/retried joins, concurrent joins and restart must not create duplicate
  identities or expose partial installation. A down node is not removed.
- Preserve store ownership, durable incarnation checks, incompatible-cluster
  refusal, Raft safety and idempotent operation ownership from D39. Its current
  immutable configuration digest cannot simply become mutable cluster identity:
  specify stable cluster identity separately from ordered membership revisions.
- The 3-to-4 transition must safely change the default second metadata group's
  voters from {1,2,3} to {2,3,4}, including catch-up and consensus transition.
  Later nodes supply storage under the existing two-group alpha layout.
- Added capacity becomes usable without losing existing chunk/pack placement
  or weakening acknowledged-write protection. Do not recompute old placement
  solely from a newly enlarged node list. Full redistribution is not required
  if new capacity is usable and old data remains readable and protected.
- Cross-group membership installation, in-flight writes/packing/reclaim and
  W154 rebuild-on-return must use consistent membership/placement authority.
  A changed member count alone is not a completed addition protocol.

No operator tuning flag chooses a weaker membership or protection mode.
Online removal, administrative shrink and full automatic rebalance are not
made alpha requirements by this amendment. Node addition is not implemented
by removing D39 checks or reinstating uncommitted HELLO-table membership.

## Alpha acceptance checklist

- [ ] Three fresh nodes form using the minimal commands, without a file,
  count, manual ids or manual raft-mkfs; one/two nodes stay visibly unready.
- [ ] Automatic discovery, explicit seed across subnets, interface/port
  selection, multiple clusters and no response behave as specified.
- [ ] Add node 4 to a live three-node cluster, then further nodes; demonstrate
  usable added capacity and successful metadata membership transition.
- [ ] Verify exact existing and new payloads, including packed files, across
  additions, concurrent I/O, orderly restart and each single-member outage.
- [ ] Interrupt additions at every durable handoff; restart/retry and concurrent
  joins converge to one membership without duplicate identity or orphaned data.
- [ ] Loss of the original seed does not block operation or joining through
  another available member. Unreachable members remain members.
- [ ] Quota/headroom defaults, readiness, protection reporting, reclaim and
  W154 repair remain correct as capacity and membership grow.
- [ ] Update model, code, help, normal launch scripts and live drivers to this
  contract; retain exact-source evidence and all failed attempts.

## Current implementation and handoff

At devel `f77daf110`, D39 admission is merged via `246bae7cf`, but startup
requires `--cluster` and rejects `--join`. That implementation does not satisfy
this amendment. Existing fixed-membership model results are historical scoped
results, not node-addition qualification. Extend the model and protocol before
claiming these gates complete. No runtime code or devops repo is changed by
this policy update, and it grants no new deployment, wipe or reformat action.

## Parent-session review of `64493ca8f` (Oct 23)

The contract is clear and is the user's decision; it reverses the three D39
choices of D40 taken earlier the same day, and the page says so. Four points
for the record and one decision the user has to take now.

1. **devel HEAD cannot run a cluster the way the contract requires.** The D39
   admission merge (`246bae7cf`) makes `efsd` refuse `--node-id` and `--join`,
   stop on a set `EFS_MD_RAFT_N`, and require `--cluster FILE`; `efs-mgmt
   raft-mkfs` requires the file and every member present; the online
   membership change (`raft-change`, `CFG`) was refused and removed; 28
   committed shell drivers and 6 Python tests were converted to the file.
   So the current devel is undeployable under this contract, and orcd-mini
   stays on `6b621993d` (the last pre-D39 build). The code gate still passes
   (431 commands, 0 FAIL at `1324cbd99`) because every test was converted
   with the code.
2. **Decision needed: revert or rework.** (a) `git revert` the admission
   merge now, so devel is deployable again with today's seed/`--join`
   startup (non-compliant with the amendment, but the working baseline the
   new design replaces), and re-apply separately the parts the amendment
   says to keep and that do not depend on a cluster file: the exclusive
   store ownership lock, the durable boot counter with its fail-closed
   checks, the identity digest machinery as library code with its tests,
   and the uniqueness tests. The model (`tests/d39_model.py`) stays as the
   base to extend. (b) Keep the merged layer and rework it in place to the
   new contract, accepting that devel cannot be deployed to orcd-mini until
   that lands. Recommendation: (a); the rework is a design task first (the
   reviewers' work order: model, then code, then gates), and a devel that
   no live cluster can run blocks every other orcd-mini gate meanwhile.
3. **What the new design must settle before code** (the amendment names
   the invariants; these are the mechanisms): automatic member identity is
   allocated by consensus in the formation record and in each committed
   addition, never by the HELLO table; the cluster identity is the MKFS salt
   plus a formation digest, with membership revisions as a separate
   committed sequence (the amendment's "stable identity apart from ordered
   membership revisions"); the 3-to-4 transition reuses the existing
   joint-consensus `CFG CHANGE` path for SHARD2 {1,2,3} → {2,3,4}, so that
   path returns; added members take new placement only (no recomputation of
   old placement) and the placement ring, pack ring and W154 rebuild read
   one committed membership revision; discovery is a separate, optional
   finder (the ORCD fabric may not carry multicast, so the explicit seed is
   the qualified path there). W153/W153a's goal, no silent four-node
   default, is met by deriving the member count from the committed
   membership, not from an environment variable.
4. **Tracking.** New Stage 1 row K33 and work item W157 point here;
   W153/W153a stay OPEN with their direction redirected to the committed
   membership. The D39 pages are marked historical where they conflict, as
   the amendment did.

Process note: the one-line uncommitted edit to
`d39-consolidated-model-20261023.md` in the shared checkout is not mine and
was left alone, as the amendment's commit did.

## Independent review of `1085baade` (Oct 23)

**Verdict: accept the implementation gap and K33/W157; qualify the rollback
recommendation and three protocol statements.** Source inspected at
`1085baade`: `src/server/efsd.c`, `src/mgmt/efs_mgmt.c`,
`src/raft/raft.c`, `src/common/placement.c` and the D39 admission tests.
Documentation review only; no binary, cluster, deployment or rollback run.

1. **Noncompliant startup is not proof that no cluster can run devel.** The
   daemon rejects join/count inputs and requires a member file; management
   still implements file-based formation. This confirms failure to provide
   the accepted interface, not universal inability to form a live cluster.
   Existing orcd-mini launch scripts need adaptation to run that interface;
   formation/failover qualification is separately NOTRUN. Unit success does
   not supply that evidence either. The blanket claim that every other gate
   is blocked is therefore not established. The reported orcd-mini revision
   is historical context, not independently checked live in this review.

2. **Prefer extending the merged implementation; do not require another user
   policy decision before design work.** The user has settled the product
   requirement. My engineering recommendation is to retain the admission and
   store-safety work and extend its model/protocol for automatic formation and
   additions. Keep an exact known-working baseline available for comparisons;
   its results do not qualify changed code. A blanket merge revert would also
   unwind driver/test and wire changes, including integration conflict fixes,
   so it is not demonstrated to be the smallest or safest route. A selective
   rollback remains an option if a concrete regression warrants it, with its
   dependencies and retained safeguards tested. No rollback is authorized or
   performed by this review. The fact that primitives are independent of a
   file does not prove their extraction is independent of the merged call sites.

3. **Discovery is required functionality, with explicit seed as an alternative.**
   `efsd --join STORAGE` must work on a supported local network. Explicit-seed
   startup permits routed or broadcast-filtered networks; it does not make
   discovery optional for alpha acceptance. The ORCD fabric's discovery
   capability is unverified here, and an explicit-seed run alone cannot qualify
   automatic discovery. Neither path needs a permanent first-node coordinator.

4. **One committed membership revision is necessary but insufficient for old
   data.** Current placement computes destinations using node count and the
   sorted member ring (`efs_place_fragments` and `efs_get_placement`). Merely
   installing a larger ring changes those results. Specify how each existing
   chunk/packed segment retains authoritative destinations or its placement
   revision, and how old revisions remain usable for reads, repair and reclaim.
   New writes may adopt the new placement only after its eligibility barrier;
   pre-addition in-flight writes need defined publication authority. Full
   rebalance can stay deferred. Do not treat a consistent current ring as proof
   that existing data survives an addition.

5. **Consensus allocation needs an explicit first-formation step.** Automatic
   ids committed with membership are the right direction, but before the first
   Raft configuration exists, candidates still need a stable provisional
   identity and an agreed initial voter set. Model concurrent joins, seed
   restart and competing formation attempts; commit the resulting initial
   identities before service. The Raft engine's `efs_raft_change` still exists;
   restoring the application path needs catch-up, joint consensus and
   cross-group installation gates, not merely restoring the management command.
   Stable cluster identity versus ordered membership revisions is accepted.

**Next work:** extend the consolidated model with these formation/addition
transitions, then implement and qualify the accepted CLI and live growth.
K33/W157 remain OPEN; runtime and topology gates remain NOTRUN. No extra user
choice is required to begin this work, and no fixed-file run closes A13.
