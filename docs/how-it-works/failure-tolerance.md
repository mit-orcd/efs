# Failure tolerance — derivation and operations

**Implementation scope:** this page specifies the accepted design, not a
current deployment guarantee. The [public-path review](../status/spec-implementation.md)
records persistence, session, fixed-profile/repair and integrity limitations;
consult those gates before claiming this contract is implemented.

[Architecture](architecture.md) · [Design rationale](design-rationale.md) ·
[Data protocol](protocols/data.md)

The normative rule lives in
[§2 of the spec](architecture.md): **N ≥ max(2f+1, k+f), RF = 2f+1**.
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
selecting entries per index. A legal sequence is an old-term uncommitted branch followed by a
current-term quorum commit on another prefix (as in
[Raft §5.4.2, Figure 8](https://raft.github.io/raft.pdf)). The older Y becomes
committed when Z is committed in term 11; Y was not committed in term 9.
For four voters A/B/C/D: B initially stores Y alone; A wins term 10 with
C/D and stores X alone; B wins term 11 with C/D and commits Y→Z on B/C/D.
A remains isolated. Permanent loss of C/D then leaves these two survivors:

```text
survivor A:   index 5  term 10  X        (uncommitted branch)
survivor B:   index 5  term 9   Y        (initially uncommitted)
              index 6  term 11  Z        (quorum-committed in term 11,
                                          committing Y with it; A stayed
                                          isolated from that majority)
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
2. **complete the profile cutover barrier** — the control plane enters
   `PROFILE_CUTOVER(P+1)`, *pushes* `P+1` to every publication authority,
   and waits for each to durably ACK it. From that point publication
   validation ([data protocol](protocols/data.md)) rejects any new
   publication whose durability evidence is not on the target profile, so
   the set of old-profile current generations can only shrink;
3. re-stripe **every protected data generation published before the
   cutover** to the new k+f width — each re-striped chunk is a *new
   immutable generation* written under the new **coding profile**
   (`coding_profile_id`), never an in-place reinterpretation of an
   existing generation;
4. verify that no current generation remains on the old profile;
5. only then commit `effective_f = target_f`.

**Step 2 is a barrier that must reach the authorities that enforce it.** A
control-plane Raft commit does not change what 4096 independent lane leaders
believe; a leader still holding profile `P` will happily accept an
old-profile publication after the "cutover", and the scan below is then
verifying a set that is still growing. This is exactly the mistake session
fencing had before round 7 — an authority cannot enforce a decision it has
never been told about — so `coding_profile_id`/`profile_epoch` is
**installed shard-local configuration**, replicated through each shard's own
Raft group, and the barrier is complete only when every publication
authority has ACKed:

```text
control plane commits PROFILE_CUTOVER(P+1)
push P+1 to every publication authority
every authority durably ACKs           <- barrier complete
only now begin the old-profile scan
```

Without the barrier the scanner races live writers and the verification is
meaningless:

```text
scanner re-stripes chunk X to 2+2
scanner moves on
    client publishes a NEW generation of X, still 2+1     <- allowed
scanner finishes, "verified"
effective_f := 2        -- but the current X survives only 1 loss
```

Because the cutover is committed *before* the scan and publication
validation enforces the current profile (round 7), any generation created
after the barrier is already on the target profile, and the scan only has
to cover generations that existed before it — a set that cannot grow. That
is what makes "no current generation is on the old profile" provable rather
than merely observed. In-flight writers holding the old profile are
rejected at publication and retry on the new one; their fragments are
orphans (L7), which is P3 working as intended.

The guarantee changes at that final commit, not when the operator asks.

**Lowering f is not the same sequence run backwards.** Raising works because
the weaker guarantee stays advertised while stronger data accumulates —
every intermediate state is at least as safe as what was promised. Lowering
inverts that: switching new writes to the weaker profile first would publish
2+1 data while the cluster still advertises f=2, so the advertised guarantee
would be false for as long as the migration takes. The advertised guarantee
must therefore drop **first**:

```text
LOWERING f:  2+2  ->  2+1
1. commit effective_f = target_f (the weaker guarantee)   <- advertise first
2. profile cutover barrier to the weaker profile (as above)
3. re-stripe / allow old wider generations to age out
4. only then shrink metadata RF and release replicas
```

Metadata RF comes last in both directions for the same reason: replicas are
removed only once nothing depends on the stronger quorum, and added before
anything does.

**Re-striping must not touch user-visible metadata.** It rewrites a
generation's *representation*, not the file's content, so it never updates
`size`, `mtime` or `ctime` — a migration that silently restamped mtime on
every chunk it re-encoded would corrupt every incremental-backup and
`make`-style workflow on the system while reporting success. The same rule
applies to repair/rebuild. Both are placement changes, not reformats. EC stripe
width k is independently configurable (wider k on larger clusters trades
encode CPU for storage efficiency, e.g. 4+3 = 1.75× overhead instead of
2+3 = 2.5×); the binding constraint is always N ≥ max(2f+1, k+f).

## Degraded operation

While the cluster is already operating with `u` unavailable failure domains
(`u ≤ f` failures already consumed), a data generation may publish with
`D ≥ k + (f − u)` durable fragments, marked degraded and queued for repair
— the invariant "survives the remaining f−u further losses" is maintained
throughout. The full rule is in [the data protocol](protocols/data.md);
rebuild scheduling is in [performance.md](performance.md).

**`u` is protection debt, not a headcount — repair pays it back, a node
coming back does not.** It is tempting to define `u` as "domains currently
down" and recompute it as nodes return. That is wrong, and it silently
under-protects data:

```text
f = 1, k = 2 (2+1)
A unavailable                       -> u = 1
generation G publishes to B, C      -> degraded, legally (k + f-u = 2)
A returns                           -> "u = 0"?  A does not hold G
B dies                              -> G has one fragment, k = 2
-> acknowledged data lost after ONE failure, at f = 1
```

The returning node restores *capacity*, not the missing fragments, so the
budget it appears to give back was never re-earned. `u` is therefore defined
per generation as **outstanding protection debt**: a degraded generation
consumes budget from the moment it publishes until its missing fragments
have actually been reconstructed and made durable. Aggregate degraded debt
is what the control plane reports and what admission for further degraded
publication is checked against; only repair retires it. This is also why
repair is prioritized above rebuild-for-balance
([performance.md](performance.md)) — debt is a live reduction of the
advertised guarantee.

**Unavailability is a committed control-plane state, never a client's
timeout.** Degraded publication is allowed only against domains the control
plane has *decided* are unavailable and recorded as such; it is never used
because a target is merely *slow*. In an asynchronous network a client
cannot distinguish the two, and letting it try would let any latency spike
quietly lower the protection level of freshly written data.
