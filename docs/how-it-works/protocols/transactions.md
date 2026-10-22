# Cross-shard transactions

**Implementation scope:** this is the accepted transaction protocol.
Namespace and resize primitives exist; request-wide public data publication
and observation remain [W77](../../backlog/work-items.md#w77). Do not infer
FUSE activation from the presence of metadata transaction helpers.
Decision retirement ([W83](../../backlog/work-items.md#w83)) follows the GC
rule below; namespace predicate bounds remain
[W84](../../status/spec-implementation.md); coordinator authority also
depends on the read-freshness gate [W82](../../backlog/work-items.md#w82).

[Architecture](../architecture.md) · [Data protocol](data.md) ·
[Directory protocol](directory.md) · [Sessions](sessions.md)

The operations that genuinely touch more than one shard (see the
operation→participant matrix in
[§6 of the spec](../architecture.md): mkdir/rmdir, cross-dir rename,
hardlink, unlink with nlink>1 or with the dentry on a different shard than
the inode, and every multi-lane atomic `write()`) need **atomic visibility**,
which a reconcile rule alone does not provide. efs uses a **Raft-backed
distributed transaction** — decentralized, no global transaction server:

```text
txid (stable across all participants)
coordinator = participant[hash(txid) % participant_count]
participant set

PREPARE  -> each participant writes a durable intent
DECISION -> coordinator Raft-commits COMMIT | ABORT (durable)
RESOLVE  -> participants apply/abandon per the decision
```

The coordinator is **dispersed by txid**, not pinned to a fixed participant:
this protocol is on the hot write path (every multi-lane `write()` uses it —
see [the data protocol](data.md)), and a large file's writes repeatedly touch
the same lane set — a deterministic-per-file coordinator would re-create a
same-file serializer. Hashing the txid spreads decision records across the
file's own lane leaders, so independent write transactions never funnel into
one shard.

Atomic *commitment* is only half the problem. The other half is
**concurrency control** — without it, two concurrent transactions over the
same keys (`rename(a,b)` vs `unlink(a)`; `rename(a,b)` vs `rename(a,c)`) have
no serializable history even though each commits atomically.

- **Conflict detection at PREPARE.** A prepare is conditional:
  `prepare(T, key, expected_version)`. It fails if another live transaction
  holds a conflicting intent on `key`, or if `key`'s current version no
  longer equals `expected_version` (the key changed since the transaction
  read it). An intent on an **exclusive** key is that key's lock: at most one
  live transaction holds it.
- **Not all transaction state is an exclusive key — and getting this wrong
  silently re-creates the hotspot one level down.** A transaction's effects
  split into kinds, and only the first kind takes an exclusive lock:

  ```text
  exclusive / CAS keys        the chunk-map entry, a dentry, an inode row
                              -> conflict, version check, one holder

  commutative reductions      MAX(lane.max_end,   ...)
                              MAX(lane.max_mtime, ...)
                              MAX(lane.max_ctime, ...)
                              MAX(dir_lane.mtime, ...)
                              -> transaction PAYLOAD, not a lock

  read / predicate guards     lane_seq · dentry_seq · parent_version ·
                              content_epoch · mtime_gen
                              -> SHARED: many guards coexist; a guard
                                 conflicts with anything that would change
                                 the guarded state, through the decision
  ```

  Consider two writers publishing chunk 10 and chunk 74 of one file, which
  by construction share a write lane ([data protocol](data.md)). They
  conflict on nothing: different chunk-map keys, and `MAX` commutes. If the
  transaction machinery treated `(FileID, lane)` as an exclusive intent key
  because both transactions update the lane's high-water marks, they would
  conflict anyway — and the per-file serialization the lanes were introduced
  to remove would simply have moved from the inode to the lane. So
  **reductions are carried in the intent and applied by the reducer at
  resolve time; they never block a prepare.** Several live transactions may
  hold reduction payloads on the same lane concurrently, provided their
  exclusive keys are disjoint. This is P1 applied to the transaction layer
  itself.

  **"Not a lock" must not mean "not visible."** A reduction still becomes
  effective at its transaction's *decision*, and the decision can precede
  the reducer materializing it into `lane.max_end`:

  ```text
  lane.max_end = 1 GiB
  T publishes at 2 GiB; T's COMMIT decision is durable
  T's intent has not been resolved yet; lane.max_end is still 1 GiB
  stat() reads lane.max_end -> 1 GiB          -- WRONG: T already returned
  ```

  So a lane's reduction state is not just its materialized marks. Pending
  reduction intents are stored **discoverably under the lane's own key
  prefix**, and the authoritative read of a lane is

  ```text
  effective(lane.max_X) = MAX( materialized lane.max_X,
                               reduction payloads of pending intents on this
                               lane whose decision resolves to COMMIT )
  ```

  resolved by the same four-state rule below. Materialization by the reducer
  is therefore a background *compaction* of already-visible state, never the
  event that makes a committed reduction observable. Because the pending set
  is bounded by the in-flight transactions on that lane, this is a short
  scan, and it is what makes I21 hold end to end rather than only at the
  moment of commit.
- **Read/predicate guards: the third primitive, and several protocols are
  unsound without it.** A transaction frequently needs an *observation* to
  stay true until it decides — not a value it intends to write. Checking a
  version at prepare time is not enough on its own, because the gap between
  the final check and the decision is exactly where the observation can be
  invalidated. So a guard is a real, durable, *shared* participant record:

  ```text
  guard(T, key, observed_version)
      -> succeeds alongside other guards on the same key
      -> conflicts with any mutation that would change that key's state
      -> is held until T decides, not until T finishes reading
  ```

  Guards are what make these protocols correct, and they are the same
  mechanism in every case, which is the point of naming it once:

  ```text
  O_APPEND reservation   guards the active lanes' lane_seq  (§7.3)
  RMDIR on a HASHED dir  guards each dentry shard's dentry_seq
  directory rename       guards every ancestor's parent_version (§7.4)
  stat() / read fallback guards the collected lane / chunk-map versions
  ```

  **The RMDIR case also needs the guard to cover things that do not exist
  yet — the classic phantom.** Emptiness is a statement about a *range*, and
  an empty range contains no key whose version could be checked:

  ```text
  RMDIR: shard 17 holds no entries of this directory   -> "empty"
  CREATE: inserts `foo` into shard 17
  RMDIR: commits on its earlier observation
  -> a directory removed while an entry existed
  ```

  A version check over the existing keys cannot see this, because the
  conflicting write created a key that was not there to be checked. The fix
  is to give the predicate a **materialized witness**: every directory
  carries a `dentry_seq` per dentry shard, bumped by *every* dentry mutation
  on that shard including inserts. `RMDIR` guards those sequence keys, so the
  `CREATE` above conflicts with the guard and one of the two aborts. This
  gives predicate-level isolation without MVCC and without a distributed
  entry counter (rejected in [directory.md](directory.md) — a counter
  rebuilds the per-directory hotspot that spreading exists to remove).
- **No-wait, and what that buys.** A prepare that finds a conflicting
  intent **fails immediately** — nobody ever waits while holding an intent,
  so no wait-for cycle can form and deadlock is impossible by construction.
  The loser aborts its acquired intents and retries with randomized backoff
  (livelock is bounded by backoff/priority tuning, a liveness knob, not a
  correctness mechanism). Because no-wait already gives deadlock freedom,
  prepares do **not** need sequential acquisition:
  - **Write-publication transactions (hot path): all PREPAREs are sent in
    parallel** (P1 — the transaction must not serialize what the hardware
    can do concurrently).
  - **Namespace transactions (rare):** participants and keys are prepared
    in a canonical global order (by shard id, then key) — a simple,
    deterministic schedule that minimizes retry churn for the
    rename/unlink/link family.
- **Visibility rule.** An intent is never externally visible as a committed
  effect; readers that encounter an intent resolve it against the
  coordinator's decision, so clients never observe an illegal intermediate
  (I17). **A transaction becomes visible at its decision, not when background
  cleanup happens to run** — a reader that meets a decided-but-unresolved
  intent must reflect the decision, or a client could fail to see its own
  just-returned write. Resolution has exactly four outcomes, and the last one
  is a safety rule, not an error path:

  ```text
  authoritative COMMIT        -> the new value
  authoritative ABORT         -> the old value
  authoritative NO-DECISION   -> the old value; this read linearizes before
                                 whatever the decision turns out to be
  cannot establish authority  -> retryable error (EIO/EBUSY). Never guess,
                                 and never report the effect as absent.
  ```

  The fourth line is I9 again, in transaction clothing: *unknown because
  unreachable* must never be answered as *not there*.
- **Idempotency / retry.** The whole transaction carries one `txid`;
  re-prepare and re-resolve are idempotent (I16).
- **Crash recovery.** A recovering participant or a reader that finds an
  intent queries the coordinator's durable decision and drives the
  transaction to completion (L5).
- **GC.** Resolved intents are reclaimed (L7-class). A **decision record**
  outlives its intents — a recovering participant must still be able to ask
  what happened — so it has its own explicit retirement rule
  ([W83](../../backlog/work-items.md#w83)):

  **A decision is retirable once every participant's RESOLVE has been applied
  in that participant's group log with an OK verdict; it is then retired by a
  replicated `TXN_RETIRE` command on the coordinator's group, which keeps a
  verdict-bearing tombstone that is reclaimed only by a later `TXN_RETIRE`
  whose logged clock is at least the retention (60 s) past that tombstone's
  retirement. An absent decision whose txid is not newer than the shard's
  replicated reclaim floor is *unknown*: readers refuse it as retryable BUSY
  and a late DECIDE is refused, so a reclaimed verdict is never answered as
  the old value or re-minted.**

  ```text
  DECIDE  [txid][coord][dec][participants]   live record = verdict + parts
  RESOLVE on every participant               each verdict OK -> proof
  TXN_RETIRE [txid][coord][now]              live -> tombstone(verdict, now)
                                             + reclaim <= 32 tombstones with
                                               now - retired_at >= 60 s,
                                               resuming at a stored cursor;
                                               floor = max txid time reclaimed
  lookup(txid): live | tombstone -> verdict
                absent, txid time >  floor -> NO-DECISION (undecided)
                absent, txid time <= floor -> UNKNOWN (STALE -> reader BUSY,
                                              DECIDE refused)
  ```

  Every input of the rule is replicated state or a field of the logged
  command (the proposer's clock rides in `TXN_RETIRE`), so all replicas delete
  the same keys at the same log index; no replica deletes on a local timer.
  Each apply touches a bounded window (one live record, at most 32 tombstones,
  one cursor/floor record), so retirement cannot stall the apply loop. The
  proposer of a transaction (its RESOLVE wait, the finisher, or recovery)
  proposes `TXN_RETIRE` after the last RESOLVE verdict. If that proposer dies
  or its RETIRE is lost, the decision stays live — never deleted by age — and
  the recovery sweep re-drives RESOLVE to the participants recorded in the
  decision and then retires it. Recovery that meets an UNKNOWN decision for a
  pending record knows every RESOLVE already applied, so the record is a
  straggler PREPARE and is dropped on that shard only. A transaction whose
  txid time trails the floor (clock skew beyond the retention) cannot decide
  and is aborted: fail-closed. Retained state is therefore live decisions of
  unretired transactions plus one retention window of tombstones per
  coordinator shard, not completed operation history.
  Duplicate-suppression records have the matching bound in
  [sessions.md](sessions.md).

With conditional prepare + no-wait, the protocol yields strict
serializability for multi-shard ops: conflicting transactions are ordered by
intent acquisition, non-conflicting ones run concurrently.

**Terminology.** What the old design deleted is the **global root-snapshot
2PC durability mechanism** — the one whose commit point was an
insufficiently-replicated coordinator/root-generation. A per-operation
Raft-backed 2PC-like protocol for cross-shard transactions is a different
thing and is exactly what's needed here. Raft (replicated ordering within a
shard) and 2PC (atomic commitment across shards) solve different problems;
efs uses each where it fits.
