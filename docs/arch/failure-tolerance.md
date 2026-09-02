# Failure tolerance — derivation and operations

[Architecture](../architecture.md) · [Design rationale](design.md) ·
[Data protocol](protocols/data.md)

The normative rule lives in
[§2 of the spec](../architecture.md): **N ≥ max(2f+1, k+f), RF = 2f+1**.
This document derives it, explains why there is no weaker mode, and
specifies how f and k change online.

## The derivation

The two planes scale differently with the configured failure target f
(max simultaneous *permanent* node losses with no data loss, capped at 3):

- **Data (EC):** a `k+f` stripe survives any f losses (any k of k+f
  fragments reconstruct). Needs **N ≥ k+f** distinct failure domains.
- **Metadata (Raft):** a committed entry lives on a quorum
  Q = ⌊RF/2⌋+1. Zero data loss through f permanent losses needs every
  committed entry to keep at least one surviving copy (**Q > f**), and the
  system *staying authoritative with no human in the loop* needs the
  survivors to still form a quorum: **N − f ≥ Q**. With RF = N (every node
  carries every shard's replica in the small-cluster limit) that is
  N ≥ 2f+1. More generally RF = 2f+1 replicas per shard, placed on distinct
  failure domains, gives Q = f+1 and survival of any f replica losses with
  both properties.

**f=3 needs 7 nodes, not 5 or 6.** On 5 nodes the best metadata
configuration is RF=5, Q=3, and 3 ≯ 3 — the 3 dead nodes could be exactly
the quorum that acknowledged the most recent writes. The config validator
rejects any configuration that violates N ≥ max(2f+1, k+f).

## Why there is no N = 2f mode

At N = 2f exactly (e.g. 4 nodes wanting f=2), f permanent losses leave a
minority of survivors. Every committed entry still has ≥1 surviving copy
(Q + f > N), so the *bits* survive — but plain Raft cannot re-establish
authority without a majority, and **no local merge rule over the surviving
logs can reconstruct the committed prefix.** Commitment information died
with the majority.

The tempting merge — "per log index, the highest-term entry wins" — is
unsound. Raft's log-prefix properties hold only *within logs produced
through Raft*; they do not license constructing a new log by independently
selecting entries per index. A legal situation:

```text
survivor A:   index 5  term 10  X        (uncommitted branch)
survivor B:   index 5  term 9   Y        (committed earlier)
              index 6  term 11  Z        (appended by a term-11 leader
                                          whose log contained Y at index 5,
                                          elected by a majority excluding A)
```

The merge produces `X@5, Z@6` — a log that **never existed**. `Z` was
created on a prefix containing `Y`, not `X`. Worse, the merge *drops* `Y`,
which was committed and possibly acknowledged to a client. Operation-ID
idempotency does not fix this: the problem is not duplicate application but
a synthesized state-machine history no leader authorized.

Raft is deliberately majority-based; the paper provides availability only
while a majority remains, and production systems (e.g. etcd) treat
permanent quorum loss as disaster recovery, not ordinary log
reconciliation. EFS therefore does not offer the mode: permanent majority
loss is **disaster territory** (an operator procedure over surviving state),
not an efs operating mode. If a 4-node/f=2-style disaster-survivability
guarantee is ever genuinely needed, it is designed as a separate protocol
with its own proof — not bolted onto Raft by prose.

## Changing f or k online

The cluster carries `effective_f` (the guarantee currently in force) and,
during a change, `target_f`. Raising f:

1. add shard replicas via joint consensus (safe reconfiguration, §7.1 of
   the spec);
2. re-stripe **every protected data generation** to the new k+f width —
   each re-striped chunk is a *new immutable generation* written under the
   new **coding profile** (`coding_profile_id`, see
   [the data protocol](protocols/data.md)), never an in-place
   reinterpretation of an existing generation;
3. verify;
4. only then commit `effective_f = target_f`.

The guarantee changes at that commit, not when the operator asks. Lowering
f runs the reverse. Both are placement changes, not reformats. EC stripe
width k is independently configurable (wider k on larger clusters trades
encode CPU for storage efficiency, e.g. 4+3 = 1.75× overhead instead of
2+3 = 2.5×); the binding constraint is always N ≥ max(2f+1, k+f).

## Degraded operation

While the cluster is already operating with `u` unavailable failure domains
(`u ≤ f` failures already consumed), a data generation may publish with
`D ≥ k + (f − u)` durable fragments, marked degraded and queued for repair
— the invariant "survives the remaining f−u further losses" is maintained
throughout. Degraded publication is never used because a target is *slow*,
only because it is *unavailable*. The full rule is in
[the data protocol](protocols/data.md); rebuild scheduling is in
[performance.md](performance.md).
