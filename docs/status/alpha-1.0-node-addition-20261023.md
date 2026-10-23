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
