# K33/W157 — membership protocol: automatic formation and online node addition (Oct 23, 2026)

**Design for [K33](alpha-1.0-2plus1-checklist.md) / [W157](work-queue.md).
Model: [`tests/d39_model.py`](../../tests/d39_model.py) C16–C22, clean
(results in the [evidence page](k33-w157-formation-addition-20261023.md)).
Contract: [alpha 1.0 node addition](alpha-1.0-node-addition-20261023.md),
"Required invariants" and "Alpha acceptance checklist". Builds on
[W153](w153-committed-membership-20261023.md).**

Rules M1–M9 below are the protocol. Each names the model action and
invariant that checks it. "Implemented" marks what the K33 increment on this
branch builds; "target" marks what a later increment adds on the same
record and rules.

## M1 One membership authority

- The **committed membership record** is the only source of who is a
  member, of the member count and of every group's voters. Group 0 holds the
  authoritative copy on the root anchor (`EFS_KV_KIND_MEMBERSHIP`); group 2
  holds a copy on its anchor, which may lag but never runs ahead
  (model `copy_ahead_of_authority`). Each member's store holds the newest
  revision it has learned.
- The HELLO node table is a liveness and usage cache. It never adds a
  member, never changes the count and never selects a voter. Before
  formation it is the input from which the formation record is built and
  then committed; after formation it keeps committed members only.
- Discovery (target) finds endpoints; it never defines membership.

## M2 Stable identities

- **Cluster identity** = the salt of the committed MKFS (`cluster` in the
  record). It never changes. A store, HELLO or record of another cluster is
  refused (incompatible cluster).
- **Member identity** = (member id, endpoint, store token). The 64-bit
  token is minted from the kernel RNG when a member store is initialized
  and kept in `identity.bin`; it never changes for that store. A wiped or
  replaced store is a new token. The record carries every member's token.
- Before formation a node's provisional identity is (operator id,
  endpoint, token). The seed's table refuses a second store claiming an id
  that a different store holds (`HELLO` rejected, duplicate id).
- A store whose token differs from the record's entry for its endpoint is
  refused at start and at learning ("store replaced"; model
  `store_not_member`, negative control `no-token`). Its member stays a
  member (M8); replacing a member's store is removal plus addition, which is
  deferred.

## M3 Ordered revisions

- Record v2: `[version 2][count][formed][cluster:8]` then per member in id
  order `[id:4][port:2][token:8][alen:1][addr]`. `formed` is the formation
  count (≥ 3); `revision = count − formed + 1`.
- A revision extends its predecessor by exactly one member with id
  `count + 1`, a new endpoint and a new token. Records form one prefix chain
  (model `divergent_membership`, `record_replaced`, `identity_gap`,
  `duplicate_identity`); the count never decreases (`membership_shrank`).
- Apply rules (`EFS_MD_CMD_MEMBERS`, log tag 92, and the record part of
  MKFS/SALT):
  - group 0: install a record that extends the stored one by exactly one
    member; a record the stored one already extends (or equals) is a
    success verdict (a retried or stale proposal of history); anything
    else is `FENCED` (a competing claim lost). Without a formation record
    it is `UNFORMED`.
  - group 2 (copy): install any extension; history is success; divergent is
    `FENCED`.
  - MKFS keeps the first record (W37); SALT applies the group-2 rule.
  Verdicts never halt a group.
- A member learns a revision only if it extends what its store holds, the
  cluster matches and the record names this store (id, endpoint, token). It
  persists the record (fsync, rename, directory fsync) before acting on it.

## M4 Automatic formation (implemented)

- The group-0 leader (one of ids 1..3; before formation group 0 runs on the
  operator's ids 1..3 as in W153) forms the cluster without an operator
  step: when its node table holds the contiguous ids 1..k, k ≥ 3, every entry
  with a token, unchanged for one second, it runs the formation: MKFS with
  the record of 1..k (proposer salt = cluster identity), then the notice to
  every member, then SALT on group 2. Before that the daemons log
  `unready: formation needs 3 members (joined: …)` and serve no client
  metadata (`EFS_ERR_UNFORMED`), as in W153. One or two nodes stay unready
  indefinitely (model `growth_one_or_two_nodes_stay_unready`,
  negative control `form-two`).
- A formation proposal lost with a leader change, a crash of any member
  before or after the commit, and a repeated formation all converge: MKFS
  keeps the first record, the notice and SALT repeat until done (model
  `growth_automatic_formation_exhaustive`, 2 crashes, every state settled).
- `efs-mgmt raft-mkfs` stays as an optional, idempotent "form now": it
  runs the same steps on the joined table and then adds, in id order, the
  table's nodes after the formed prefix (M5). Nodes that join after
  formation are pending, not in the table; their own HELLO retries add
  them (every two seconds). Nothing requires the command.
- **Target (automatic ids, M4′):** the seed allocates provisional ids in
  join order and persists its proposal; each candidate persists a promise
  for that proposal (refusing another cluster's); the seed commits the
  formation only when every proposed member has promised, and group 0 is
  bootstrapped from the committed ids. Modelled
  (`growth_automatic_ids_formation_exhaustive`); not built in this increment,
  where ids remain the operator's `--node-id`.

## M5 Online addition (implemented)

1. **Pending.** A node with an unformed store says HELLO (with its id,
   endpoint, token and no cluster) to any member. The member answers
   `EFS_HELLO_REJECT_PENDING` when the node is eligible: its endpoint is not
   in the record and its id is `count + 1` (manual ids; target: the record
   allocates `count + 1`). A higher id waits (pending) until the lower ids
   are added; an id held by another identity, an endpoint held by another
   token or another cluster is refused (`EFS_HELLO_REJECT_FORMED`, the
   joiner exits). A pending node is not in the node table, not in any
   placement and not a voter. Before formation a node whose store already
   belongs to a formed cluster is told to wait (pending), never entered in
   the formation table: it is a member this node has not learned about yet,
   or one of another cluster.
2. **Commit.** The member proposes `MEMBERS(view + joiner)` on group 0. A
   member that hosts no group-0 replica forwards the request
   (`EFS_MD_REQ_MEMBER_ADD`, tag 93) to the group-0 members in turn,
   skipping unreachable ones, until one gives a verdict. Duplicate HELLOs,
   retries and concurrent proposals by several members are the same record
   (success); two stores claiming one id: one commits, the other gets
   `FENCED` and is refused at its next HELLO (model
   `growth_competing_and_retried_joins_exhaustive`).
3. **Learn.** Every node that applies the revision (group 0 or 2 replicas)
   learns it from its own apply; the **notice pump** of the group-0 leader
   sends the current record (`EFS_MD_REQ_FORMED`, now revision-aware) to
   every member until each has acknowledged it, again after any leader
   change and every two seconds for a member that did not answer. A down
   member learns when it returns; no node coordinates permanently, so losing
   the seed changes nothing (the leader moves).
4. **Joiner.** The joiner keeps retrying HELLO (every two seconds) until its
   store holds a record naming it. Then it is a member: it enters node
   tables (endpoint from the record, storage and usage from its HELLO) and
   new placements, and attaches the metadata groups it votes in (M6).
5. **Copy.** The group-0 leader proposes the new record to group 2 (group-2
   rule of M3) until group 2 holds it.

Each numbered step is a durable handoff; the model crashes every actor
after every step (two crashes per run) and settles every reachable state to
the converged membership.

## M6 Metadata voters (implemented)

- Two groups, alpha layout unchanged: group 0 voters {1,2,3} for every
  count; group 2 voters {1,2,3} for count 3 and {2,3,4} from count 4
  (`efs_md_group_voters` over the record's count = the **target**).
- A replica's **boot configuration** is the voters at the formation count
  (`formed`), never the current count; the Raft log and snapshot overlay
  every later JOINT/COLD. A first-time learner (node 4) therefore starts
  outside the configuration and cannot campaign (I18; `attach_replica`).
- A node attaches a group when it is a boot voter, a target voter or holds
  that group's Raft state, and only after its store learned a record naming
  it (model `voter_without_identity`, negative control `early-vote`).
- The **voter reconciler** on the group-2 leader compares its voters with
  the target of the record it holds. If they differ and no joint change is
  in progress it calls `efs_raft_change(target)`: the engine adds node 4 as a
  learner, refuses (`BUSY`) until node 4's match index reaches the commit
  index (catch-up), then appends JOINT {1,2,3}+{2,3,4} and COLD (model
  `uncaught_voter`, `disjoint_quorums`; negative controls `no-catch-up`,
  `direct-change`). The reconciler runs every tick (200 ms), so a leader
  change or restart at any point resumes it.
- **Retired replica.** A node that is not a target voter of a group under
  the record's count (node 1 for group 2 from four members) marks its
  replica retired as soon as it learns the revision: it never campaigns,
  steps down if it leads, and never serves that group locally (requests
  are forwarded to a voter). It still answers votes and entries while the
  joint configuration includes it. Raft does not guarantee that a removed
  voter receives the COLD entry that removed it, so this rule comes from
  the record, not from the replica's own (possibly stale) configuration.
  `raft-status` shows `retired=1`. Found by the daemon test: without it a
  handler on node 1 waited on its stale group-2 replica.
- Members 5 and later attach no metadata group (`storage_only_node_votes`).
- If the new voter's store is lost before catch-up, the change waits;
  group 2 stays on {1,2,3} with three voters (model
  `growth_store_replacement_refused_member_stays`).

## M7 Placement (implemented)

- **Existing data keeps its recorded destinations.** Every published chunk
  records its three fragment nodes (publication refuses a zero node,
  `pub-reject reason=node0`); reads, repair (W154), reclaim and GC use the
  recorded nodes. Packed segments carry their destinations in the pack
  ledger. Nothing recomputes an old chunk from the enlarged list (negative
  control `recompute-placement`; the model shows recomputation would move
  chunks).
- The read path's ring computation in `efs_client_decode_placed_chunk_attempts`
  is used only for a chunk without a published entry (nothing to read).
- **New capacity is used by new writes.** New placements use the committed
  member view (`LIST_NODES` returns committed members; the packer and the
  storage-session fence use the server's committed table). In-flight
  writes keep the nodes their admitted ticket names; all of them stay
  members (no removal), so their publication authority is unchanged.
- **Client view (implemented, [evidence](k33-client-view-20261024.md)).** A
  client mounts with the committed record of a member
  (`EFS_MSG_MEMBERSHIP_READ`, wire types 245/246) and refuses to mount when
  its `LIST_NODES` table contradicts it. While mounted it asks one member
  (rotating) for its record every second and installs a later revision:
  same cluster, same formation, the installed members unchanged as its
  prefix (`include/efs/member_view.h`). New members are appended to the
  node table and the count is published last; rows are never rewritten.
  Another cluster, a divergent record or an invalid reply is refused and
  logged; an older revision (a member that has not learned yet) is
  ignored. A request for a node id the table lacks wakes the view and waits
  for one round over every member (at most 2 s). New writes place over the
  installed members; existing chunks keep their recorded nodes. Peer
  connections to added members open on demand (TCP, RDMA upgrade).

## M8 Unreachable members and the other consumers

- A down member stays a member (`membership_shrank`, negative control
  `drop-unreachable`). Heartbeat "down" is a placement hint only.
- W154 rebuild-on-return, reclaim, the GC fan-outs, the storage-session
  fence and packing enumerate the node table, which after formation holds
  committed members only (W153) and grows only by learned revisions. They
  therefore see the same membership authority; none of them changes it.

## M9 Handoffs, restart and refusals

| Durable step | Who | Restart / interruption |
| --- | --- | --- |
| store init (token) | every node | same token; wiped store = new token, refused once committed |
| MKFS commit | group 0 | first record kept; notice and SALT repeat |
| store learns formation | each member | re-learned from own KV (`host_membership_reconcile`) or the notice pump |
| SALT / MEMBERS on group 2 | group-0 leader | repeated until held; history = success |
| MEMBERS commit on group 0 | any member (forwarded) | lost proposal: joiner's next HELLO repeats it; committed: pump notifies |
| joiner store learns | joiner | until then it retries HELLO; its member check passes on (id, endpoint, token) |
| group-2 learner catch-up | group-2 leader | reconciler retries; learner restarts from its log |
| JOINT / COLD | group 2 | Raft I18; reconciler completes after leader change |

Refusals are fail-closed and name the conflict: another cluster, another
token for a committed endpoint or id, a record that does not extend the
store's, a node id out of order (manual ids). No refusal removes a member.

## Wire and storage numbers

- Log tag 92 `EFS_MD_CMD_MEMBERS`; request tag 93 `EFS_MD_REQ_MEMBER_ADD` (90 and 91 before the merge with K32, which took them).
- `EFS_HELLO_REJECT_PENDING` = 4. `struct efs_msg_hello` gains `token` and
  `cluster`; `struct efs_node` gains `token`.
- Record version 2 (above); member store `identity.bin` version 2 (token).
  No KV kind added (the record stays `EFS_KV_KIND_MEMBERSHIP` 55).
- Earlier stores and records are refused (no backward compatibility).

## Remaining increments (not in this branch)

1. Automatic ids and the positional CLI (`efsd ADDR:PORT STORAGE`,
   `efsd --join [SEED] STORAGE`), M4′ promise formation.
2. Discovery on the local subnet and explicit seed across subnets.
3. ~~Live client view refresh on a new revision (mounted clients).~~ Done
   ([evidence](k33-client-view-20261024.md)).
4. Rebalance (not required), removal (deferred).
