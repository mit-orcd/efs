# K33/W157 — membership protocol: automatic formation and online node addition (Oct 23, 2026)

**Design for [K33](alpha-1.0-2plus1-checklist.md) / [W157](work-queue.md).
Model: [`tests/d39_model.py`](../../tests/d39_model.py) C16–C22, clean
(results in the [evidence page](k33-w157-formation-addition-20261023.md)).
Contract: [alpha 1.0 node addition](alpha-1.0-node-addition-20261023.md),
"Required invariants" and "Alpha acceptance checklist". Builds on
[W153](w153-committed-membership-20261023.md).**

Rules M1–M9 below are the protocol. Each names the model action and
invariant that checks it. "Implemented" marks what K33 increments 1
(formation, addition) and 2 (automatic ids, positional CLI, discovery;
[evidence](k33-w157-autoid-discovery-20261024.md)) build; "target" marks what
a later increment adds on the same record and rules.

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
- Discovery (implemented, increment 2) finds endpoints; it never defines
  membership. A seed, promised or formed node answers UDP probes with its
  cluster identity, state, member count, service endpoint and build; a
  joiner without a seed probes the broadcast address of each up IPv4
  interface's own subnet (or the one `--discover-on` names), ignores other
  builds, refuses when several clusters answer (an explicit seed is then
  required), says so and waits when nobody answers, and then says HELLO to
  a peer it found: the committed record, not the answer, decides.

## M2 Stable identities

- **Cluster identity** = the salt of the committed MKFS (`cluster` in the
  record). It never changes. A store, HELLO or record of another cluster is
  refused (incompatible cluster).
- **Member identity** = (member id, endpoint, store token). The 64-bit
  token is minted from the kernel RNG when a member store is initialized
  and kept in `identity.bin`; it never changes for that store. A wiped or
  replaced store is a new token. The record carries every member's token.
- Node ids are never an operator input (increment 2). A store created with
  `efsd ADDR:PORT STORAGE` is a **seed**: node 1 at that endpoint, with a
  64-bit cluster identity minted at creation (the later MKFS salt). A store
  created with `efsd --join [SEED] STORAGE` is **joining**: a token and no
  id; its provisional identity is (endpoint, token) and it never enters a
  node table. It gets its id from the seed's formation proposal (M4′, a
  durable promise) or from the committed revision that adds it (M5, id
  count + 1). One store per id in the formation table, as before.
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
- **Automatic ids, M4′ (implemented, increment 2):** joining stores say
  HELLO with node id 0 to the seed, which keeps them as candidates while
  their HELLOs (every second) keep arriving. Once at least two are fresh
  and the set is unchanged for one second (or at once on `raft-mkfs`), the
  seed allocates ids 2..k in endpoint order (IPv4 address, then port) and
  persists the proposal as its own promise (identity state `promised`).
  From then on it answers each named store with the proposal
  (`EFS_HELLO_PROMISE`); the store persists its promise (refusing a
  proposal of another cluster or another one than it promised) and says
  HELLO again with its id and the cluster, entering the formation table.
  A store the proposal does not name waits for formation and joins as an
  addition. A promised store knows its id and the boot voters, so ids 1..3
  run group 0; the group-0 leader, itself promised, proposes MKFS with the
  proposal once every proposed member is in its table at the proposed id,
  endpoint and token (each promised), and the MKFS salt is the cluster
  identity. A lost MKFS is proposed again; MKFS keeps the first record.
  Model: `growth_automatic_ids_formation_exhaustive` (formation through
  group 0 with lost proposals since increment 2). A proposed store that
  never returns blocks formation (wipe and start again); removal is
  deferred.

## M5 Online addition (implemented)

1. **Pending.** A joining store says HELLO (node id 0, its endpoint and
   token) to any member. The member answers `EFS_HELLO_REJECT_PENDING` with
   the cluster and the members' endpoints (so the joiner can ask another
   member if this one goes away) and queues its addition, when its endpoint
   and token are not in the record; once the record names it at that
   endpoint with that token, the answer is the record itself (accepted,
   `assigned_id` its id): the joiner persists it and is a member. An
   endpoint held by another token, or this token at another endpoint, is
   refused (`EFS_HELLO_REJECT_FORMED`, the joiner exits). A pending node is not in the node table, not in any
   placement and not a voter. Before formation a node whose store already
   belongs to a formed cluster is told to wait (pending), never entered in
   the formation table: it is a member this node has not learned about yet,
   or one of another cluster.
2. **Commit.** The member proposes `MEMBERS(view + joiner)` on group 0. A
   member that hosts no group-0 replica forwards the request
   (`EFS_MD_REQ_MEMBER_ADD`, tag 93) to the group-0 members in turn,
   skipping unreachable ones, until one gives a verdict. Duplicate HELLOs,
   retries and concurrent proposals by several members are the same record
   (success). Two joiners at once propose the same id `count + 1` from one
   view: one commits, the other's proposal is `FENCED` (it does not extend
   the record) and its next HELLO proposes `count + 2` on the newer record,
   so concurrent joiners get distinct ids (model
   `growth_concurrent_automatic_joins_exhaustive`; manual ids:
   `growth_competing_and_retried_joins_exhaustive`).
3. **Learn.** Every node that applies the revision (group 0 or 2 replicas)
   learns it from its own apply; the **notice pump** of the group-0 leader
   sends the current record (`EFS_MD_REQ_FORMED`, now revision-aware) to
   every member until each has acknowledged it, again after any leader
   change and every two seconds for a member that did not answer. A down
   member learns when it returns; no node coordinates permanently, so losing
   the seed changes nothing (the leader moves).
4. **Joiner.** The joiner keeps retrying HELLO (every second) until a
   member answers with the record naming it, which it persists before it
   serves anything; a member restarted from its store keeps saying HELLO
   until its store learned the revision. Then it is a member: it enters node
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
  storage-session fence use the server's committed table). A client sees
  members at mount; a mounted client keeps its view until remount (live
  refresh is a remaining increment). In-flight writes keep the nodes their
  admitted ticket names; all of them stay members (no removal), so their
  publication authority is unchanged.

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
| store init (token; seed: id 1, cluster) | every node | same token and cluster; wiped store = new token, refused once committed |
| formation proposal (seed's promise) | seed | the same proposal is answered again; never replaced |
| promise | each proposed store | restarts promised with its id; a lost answer is repeated |
| MKFS commit | group 0 | first record kept; notice and SALT repeat |
| store learns formation | each member | re-learned from own KV (`host_membership_reconcile`) or the notice pump |
| SALT / MEMBERS on group 2 | group-0 leader | repeated until held; history = success |
| MEMBERS commit on group 0 | any member (forwarded) | lost proposal: joiner's next HELLO repeats it; committed: pump notifies |
| joiner store learns | joiner | until then it retries HELLO (id 0); a member answers with the record on (endpoint, token): same id after a lost answer |
| group-2 learner catch-up | group-2 leader | reconciler retries; learner restarts from its log |
| JOINT / COLD | group 2 | Raft I18; reconciler completes after leader change |

Refusals are fail-closed and name the conflict: another cluster, another
token for a committed endpoint or id, a record that does not extend the
store's or its promise, another endpoint for a store's member, a joining
store started as a seed. No refusal removes a member.

## Wire and storage numbers

- Log tag 92 `EFS_MD_CMD_MEMBERS`; request tag 93 `EFS_MD_REQ_MEMBER_ADD` (90 and 91 before the merge with K32, which took them).
- `EFS_HELLO_REJECT_PENDING` = 4; `EFS_HELLO_PROMISE` = 5 (increment 2).
  `struct efs_msg_hello` gains `token`, `cluster` and (increment 2) `flags`
  (`EFS_HELLO_F_FORMED` = 1); node id 0 is a joining store. `struct
  efs_msg_hello_ack` gains (increment 2) `cluster`, `formed`, `record_len`
  and `record`; `struct efs_node` gains `token`.
- Record version 2 (above); member store `identity.bin` version 3
  (increment 2: state joining / seed / promised / formed, cluster identity,
  endpoint).
- Discovery datagrams (increment 2, UDP port 17430 by default): magic
  `EFSDISC` version 1, probe `[magic:8][1][nonce:8]`, answer `[magic:8][2]
  [nonce:8][cluster:8][count:4][formed:1][port:2][alen:1][addr][blen:1]
  [build]` (`src/server/discovery.h`). No log tag, request tag, message type
  or KV kind added in increment 2.
  No KV kind added (the record stays `EFS_KV_KIND_MEMBERSHIP` 55).
- Earlier stores and records are refused (no backward compatibility).

## Remaining increments

1. ~~Automatic ids and the positional CLI, M4′ promise formation~~ and
   ~~discovery on the local subnet, explicit seed across subnets~~: built in
   increment 2 (live multi-host and cross-subnet gates NOTRUN).
2. Live client view refresh on a new revision (mounted clients).
3. Rebalance (not required), removal (deferred).
