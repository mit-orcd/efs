# W66 protection and routing — implementation proposal, Oct 10

**Open proposal. No topology activation or D36 closure.**
[Binding D36](node-count-protection-20261009.md) requires acknowledged metadata
preservation after any two permanent losses among four nodes. The engine
foundation is tested in [W66 evidence](../../results/measure/20261010-w66-foundation/README.md).

## Recovery contract requiring the user's answer

Recommended: retain a strict three-of-four quorum for live reads/writes.
After two permanent losses, preserve and inspect the surviving certified state,
replace lost nodes and run verified recovery before resuming service. Merely
having a WAL copy does not confer authority. Automatic writes on either half
of a two/two partition are forbidden. Availability after loss is separate from
preservation, and D36 has left it unresolved.

If automatic writes with only two survivors are required, the protocol needs
an explicit way to distinguish permanent loss from a partition and uniquely
fence the other half. A timeout, preferred survivor, highest term or implicit
fifth witness cannot provide that guarantee. Do not silently weaken D36 or
introduce that dependency. The user has been asked to select the contract;
no answer is inferred from elapsed time.

## Durable authority before success

A proposed acknowledgment protocol must durably distribute commit evidence as
well as command bytes before reporting success. Evidence binds cluster and
metadata-group identity, configuration generation, the exact committed index
and term, command/history digest and the replication quorum. Four-node
acknowledgments need evidence and the reconstructible metadata prefix on at
least three distinct nodes, so every two-loss pair retains both. Joint
reconfiguration must satisfy both old and new quorum/protection requirements.
A simple durable leader-local commit index does not meet D36.

The evidence is produced only after durable command quorum acknowledgment.
Its durable distribution is a second boundary; failures before that boundary
cannot produce a successful client reply. Retries preserve operation identity
and may discover a committed result after an unknown reply. A certificate's
presence does not itself prove the second boundary completed, but it must
prove that its exact history was committed; extra committed-but-unacknowledged
operations may survive recovery. This is a crash-fault design, not a Byzantine
or hardware power-loss proof.

Snapshots bind the same committed history, membership, topology epoch and
metadata image digest. Snapshot/image durability and proof precede log removal;
rotation, torn writes, quota refusal and restart must preserve the last valid
pair. The recovery procedure must validate the complete proof/prefix chain,
not select the largest index/term. It must fence old physical incarnations and
establish a unique successor configuration before accepting writes. The
successor procedure and its fencing proof remain to be implemented and tested.

## Routing and migration order

1. Persist a versioned logical-shard/group/replica mapping with stable cluster
   and group identities. Bootstrap preserves the legacy odd/even mapping.
   Restart must load the persisted map and reject incompatible configuration,
   rather than reinterpret existing keys under a new hash or shard function.
2. Expose authoritative map epoch and leader hints. Clients cache per group,
   refresh under bounded retries and reuse bounded connections. An unavailable
   leader must not discard a read's FileID or a mutation's replay identity.
3. Remove two-group assumptions in host attachment, session fences, transaction
   recovery/REPORT routing, GETCHUNKS lane reads, snapshots, GC and management.
   Keep mapping authority independent from an endpoint that happens to forward.
4. Introduce a small configurable set of groups with shared pumps, transport
   and multiplexed WAL; do not create 4096 threads/connections/fsync streams.
   Place leadership across nodes. Adding groups without independent service
   capacity is not a scaling result.
5. For movement, freeze admission for the affected authority, drain/resolve
   admitted operations, persist and verify its snapshot/receipt/session state
   at the destination, then commit an epoch transition. Old owners fence old
   requests; uncertain transitions resolve through durable authority. Test
   crashes at each boundary before permitting online migration.

## Closure gates

- All three single-loss cases at three nodes and all six permanent two-loss
  pairs at four nodes, with cold survivors, exact namespace/attributes,
  publication/session/ticket verdicts and authoritative commit history.
- Faults during both command and evidence distribution, snapshot/rotation,
  reconfiguration and map migration; partitions never authorize two/two writes.
- Stale routing, unavailable groups, cross-group mutations and reads, recovery
  and GC under generalized groups, with bounded retry and memory ownership.
- Controlled independent-key and hot-directory workloads. Set numerical
  targets from recorded baselines; measure useful capacity, tails, errors,
  queue depths/RSS and remaining bottlenecks as groups/nodes increase.
- AMD is one host with four daemons and software RXE. It can test logical
  process/store loss and TCP/RDMA modes; it cannot prove independent-host or
  hardware power-loss behavior. Do not claim 1000-active-client capacity from
  idle connections or eight CLI timing cells.

The tested engine fixes are useful prerequisites, but do not close these gates.
