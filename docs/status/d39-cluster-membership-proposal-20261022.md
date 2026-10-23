# D39 proposal — authoritative configured cluster membership (Oct 22, 2026)

**Status: PROPOSED, awaiting the user's decision. No code changed.** Written
at the user's request after the W153 review
([integration review](devel-integration-review-20261022.md#review-of-a76b63f42--w153-is-an-alpha-correctness-gate)):
W153a (fail-closed `EFS_MD_RAFT_N`) is on hold until this is decided.

## 1. What exists today (devel `40c0465ab`)

Identity and membership span five mechanisms with no shared bootstrap source
of truth:

| Mechanism | Where it lives | Who sets it | What checks it |
| --- | --- | --- | --- |
| Node identity `--node-id`, `--addr`, `--port` | daemon command line | the operator or driver (`cluster.sh`, `nuc-efs/start.sh`: ids 1..N in start order) | the Raft host refuses an id outside `1..n`; HELLO/table deduplication is not proof that two processes claiming the same id are the same member (`server_dedupe_nodes_locked`). `scripts/server.sh` derives ids from the address (`octet·65536+port`), which the Raft host rejects; every driver bypasses it |
| Membership table (`struct efs_node[EFS_MAX_NODES]`) | in memory, persisted to `<root>/meta/cluster_nodes.bin` by `server_save_nodes` | `--join <seed>` sends HELLO; the seed appends the node (any id ≤ 16, up to 16 rows), answers its whole table and epoch, and gossips the change; the joiner adopts the table; restarts rejoin through persisted peers | build id and protocol version (fail-closed); capacity. **Nothing states which nodes are supposed to be members**: a table of 3 and a table of 4 are both valid |
| Metadata topology `n` (`EFS_MD_RAFT_N`) | daemon environment, default 4 | each driver independently (`cluster.sh` passes the host count; test scripts pass 3) | range `3..16` only; unset, empty or `3junk` is accepted. Seeds every group's voters (`efs_md_group_voters(group, n)`); the all-member GC handoff waits for ids `1..n` to be present |
| Committed Raft membership per group | the group's Raft log and `mdraft/desired.<group>`; changed by `efs-mgmt raft-change <addr> <group> <mask>` (NOTE every peer, then joint-consensus `efs_raft_change`) | the operator, after bootstrap | the Raft engine (quorum, joint consensus); becomes authoritative once committed and overrides the seed on restart |
| Table formation `efs-mgmt raft-mkfs <seed>` | one operator call | the driver, once, on the seed | needs a quorum in both groups to commit MKFS and SALT; W37 preserves the first committed salt on a repeated mkfs; it does not simply refuse every call from another node; does not check how many members exist |

The W153 defect is the gap between rows 2 and 3: a three-member table with
`n = 4` is accepted everywhere, SHARD2's voters include an id that no member
has, that group has no loss tolerance, and inode reclaim through the
all-member handoff silently stops.

## 2. Requirements for the decision

1. One place says which nodes form the cluster; every daemon and `raft-mkfs`
   read the same statement, and a daemon whose statement differs is refused
   with a clear error (fail-closed, AGENTS.md).
2. Membership is never inferred from who is reachable. An intended member
   that is down is an outage; quorum arithmetic uses the configured set.
3. Committed Raft membership stays authoritative after bootstrap; later
   changes go through the existing joint-consensus path, never through
   editing the configuration on a restart.
4. Easy configuration: no second mandatory knob that duplicates the first;
   fewer operator inputs than today, not more.
5. Fresh format only; no compatibility with today's node table or env (not
   in production).

## 3. Options

**A. Keep the per-daemon count, make it strict (W153a only).** `EFS_MD_RAFT_N`
becomes mandatory, a whole decimal in `3..16`; HELLO carries `n` and a seed
refuses a joiner with a different `n`; `raft-mkfs` refuses if fewer than `n`
members are in the table. Smallest change; still two inputs (`--node-id`
and `n`) that must agree across hosts by convention; ids are still chosen by
hand.

**B. A configured member list is the bootstrap authority (recommended for
alpha 1.0).** One statement, the same bytes on every node:

```
# cluster.conf — one line per member, id order is placement order
1 10.0.0.207:17432
2 10.0.0.207:17433
3 10.0.0.209:17432
4 10.0.0.209:17433
```

- *Where it lives.* A file passed as `--cluster <path>` to every daemon and to
  `efs-mgmt raft-mkfs`; the daemon also writes it, with its hash, into
  `<root>/meta/cluster.conf` at first start so a restart without the flag
  uses the recorded one. `n` is the line count; the daemon's own id is the
  line whose `addr:port` equals its `--addr/--port` (so `--node-id` goes
  away); default voter sets come from `efs_md_group_voters(group, n)` over
  the list order; `EFS_MD_RAFT_N` is deleted.
- *Agreement across nodes.* HELLO carries the list hash. A seed refuses a
  joiner with a different hash or an id/address pair not in its list, and a
  joiner refuses a seed likewise, both with the two hashes in the error.
  `raft-mkfs` passes the hash; the MKFS command records it in the Raft log,
  so the committed table carries the membership it was formed with, and a
  daemon whose recorded list differs from the committed one refuses to serve
  (same shape as the W37 salt rule). Membership rows in the node table become
  a cache of status (addr, quota, used, down) for the configured members,
  not a definition of membership; HELLO never adds a member.
- *Relation to `--join`.* Removed. Every daemon knows the seed set from the
  list: it contacts the lowest-id live member (or any member) to exchange
  status, and keeps retrying absent members as outages. No "standalone and
  retry" state: a daemon that cannot reach any member stays up, serves
  nothing that needs a quorum, and reports which configured members are
  absent.
- *Relation to `raft-mkfs`.* Takes the list, refuses if the contacted node's
  recorded list differs, and proceeds only when a quorum of each group's
  configured voters is present (it needs that to commit anyway); the
  committed MKFS carries the hash. Absent members join later as configured
  members catching up, never as new members.
- *Later changes.* Adding or removing a node is `efs-mgmt raft-change` plus a
  replicated `CLUSTER_SET` command that commits the new list and hash;
  daemons rewrite their recorded copy from the committed command, never from
  a file edit. This is alpha 1.0's reconfiguration boundary; the joint
  consensus already exists, the list command is new.
- *Cost.* Parsing and hashing a small file, a HELLO field, an MKFS field, a
  refusal on mismatch, removal of `--join`/`--node-id`/`EFS_MD_RAFT_N`, and
  changes to `cluster.sh`, `nuc-efs/start.sh`, the committed drivers and the
  live test harnesses that start private clusters. One sub-session for the
  daemon and mgmt side, one for drivers and harnesses; a fresh format.

**C. Membership in the control-plane Raft group** (architecture §7.8: desired
placement, incarnations, drain and rebuild). The full design is broader than
this bootstrap fix. A three-replica control group is not inherently blocked by
D36; D36 becomes relevant to its later two-loss protection. Deferring C is a
scope choice for alpha 1.0, not a demonstrated dependency on 2+2.

## 4. Recommendation

B for alpha 1.0, with C as the direction it grows into (the committed
`CLUSTER_SET` list is what the control-plane group will own later). If B is
judged too large for alpha, A is the fail-closed minimum and must land before
the alpha gate; it does not satisfy requirement 1 (two inputs by
convention) or 4.

The review in §6 recommends deciding the scope of B (fixed bootstrap versus
online membership changes), the local identity selector, and initial formation
readiness. The filename and flag spelling are implementation details, not
blocking architecture decisions. The earlier recommendation of quorum-only
formation is qualified below: metadata quorum alone is not proof that the
three data fragments can be placed durably.

## 5. Acceptance gates (either option)

- Three-node configuration with no count given: topology is exactly the
  configured one, or startup is refused with the reason; never an absent
  fourth voter.
- Malformed input (`3junk`, duplicate id, duplicate address, 2 or 17
  members) refused before serving.
- Two nodes started with different lists cannot form or join one cluster;
  the refusal names both hashes.
- Sequential start with one configured member absent: the others serve with
  their quorum, report the absent member, and never shrink the membership.
- Three-node live gate: lose each node in turn; both groups keep a leader,
  acknowledged payloads verify, inode reclaim through the all-member handoff
  progresses once the node returns.


## 6. Independent review — source 65a5bf1cb

**Verdict: B is a sound direction, but the protocol is not ready to implement
as written. D39 remains PROPOSED and W153a stays on hold.** This review changes
no release scope: alpha 1.0 is only 2+1 on 3, 4 and >4 nodes. It does not add
2+2, compatibility, a migration requirement or a second operator count.

### Required corrections before implementation

1. **Separate immutable cluster identity, bootstrap configuration and current
   membership.** Hash canonical parsed records (version, cluster identity,
   stable member IDs and normalized advertised endpoints), not incidental
   whitespace/comments. For alpha's current indexing assumptions, require
   the initial ID set 1..N explicitly; line reordering must not renumber
   members. Persist the actual list and its digest, not only the digest.
   Independent clusters with identical endpoint lists must not accidentally
   share an identity. Specify how formation establishes that identity once
   and how interrupted/retried formation preserves it.
2. **Bind every peer connection, not just gossip HELLO.** Today's handler
   has a CLIENT_HELLO declaration gate before dispatch (W133), and RAFT
   frames then dispatch directly to `server_raft_host_inbox`. Extend the
   appropriate peer declaration/admission to bind cluster identity,
   configuration and sender ID before Raft, storage or membership traffic,
   including connections opened before MKFS. HELLO-only hash checking does
   not meet the stated agreement requirement. A hash checks agreement; it
   is not authentication against a hostile peer. Do not weaken the existing
   build/protocol gate.
3. **Make formation cover every authority.** Today's group 0 applies MKFS;
   group 2 receives a separate SALT command and can exist on a node that
   never applies MKFS (`server_raft_host_mkfs`). A hash only in MKFS therefore
   cannot establish local agreement on all voters. Install the formation
   identity/configuration in both groups, bind it to persisted state and
   snapshots, and define resumable partial formation and readiness. Nodes
   5+ also need an authoritative verification path despite holding no
   default metadata group. Quarantine mismatches before serving; never
   silently overwrite a mismatched store from the command-line file.
4. **Do not treat online reconfiguration as one extra command.** The proposal
   does not specify which group owns CLUSTER_SET, how it reaches the other
   group and data-only nodes, or how joint consensus, placement and GC
   transition in order. Exact hash equality would reject a legitimate node
   still on the prior committed configuration unless there is an explicit
   transition protocol. Specify configuration epochs, learner admission,
   authoritative commit/install boundaries, interruption/retry behavior and
   removal/drain safety before promising online add/remove. Membership
   transitions within the current format are a correctness problem, not
   backward-compatibility work. Existing per-group joint consensus alone
   does not solve this cluster-wide coordination problem.
5. **Keep formation and steady-state availability distinct.** A quorum in
   both metadata groups can exist while fewer than three data destinations
   are available. MKFS success must not be reported as full protected-data
   readiness in that state. Requiring every configured member for initial
   formation is a possible simplification; it must never become an all-node
   requirement for ordinary restart or metadata quorum service. An absent
   node never causes membership to shrink. Do not promise successful new
   fully protected writes after a loss without a separate data-path gate.
6. **Identity matching needs a precise endpoint contract.** An advertised
   addr:port can select a unique configured ID if it is canonical and unique;
   a wildcard bind address, DNS alias or a separate RDMA address is not that
   identity. Reject ambiguous/non-matching selectors. If a stable explicit
   member selector is retained, it selects a record in the authoritative
   list, not an independently configured topology. Never derive IDs from
   file order or currently reachable peers.

These requirements make the proposed two-sub-session effort estimate
premature, particularly if online reconfiguration is included. No global
configuration change may be inferred merely from a restart with edited files.

### Decisions that actually need the user

- **Scope:** choose B with fixed configured membership for alpha (recommended
  minimum), or B with online add/remove in alpha. The fixed variant requires
  an explicit deferral of online reconfiguration if that is currently an
  alpha requirement; do not silently remove an accepted gate. It allows a
  fresh 3-, 4- or >4-node cluster and resolves W153 without inventing a
  partially specified CLUSTER_SET protocol. Online changes can instead stay
  in scope, but need the transition design above before implementation.
- **Local identity:** use one canonical advertised endpoint to select the
  member record (recommended if deployment endpoints are stable), or retain
  a stable member-ID selector checked against that same list. This is an
  addressing/deployment choice, not a second membership list.
- **Initial formation:** require all configured members to agree and be
  present (recommended simplification), or allow quorum-only formation with
  explicit incomplete/degraded readiness until data-placement requirements
  are met. Either choice preserves quorum-based restart and the one-loss
  contract; initial formation policy must not silently constrain runtime
  availability.

`cluster.conf` and `--cluster` are reasonable implementation names; no separate
user decision is needed for them. Neither this review nor its recommendations
constitutes approval of B, a live reformat, or resuming W153a.

### Validation and acceptance additions

Source review only on macOS; no binary, cluster, mount, transport, fixture or
quota was exercised. Reviewed `server_raft_host_start`, `server_raft_host_mkfs`,
the handler declaration gate and Raft dispatch, and the documented voter
layout. Protocol tests are **NOTRUN** because the proposal has no implementation.
In addition to §5, require deterministic canonicalization and duplicate/alias
checks, conflicting configurations over direct Raft connections, partial
formation/retry with group 2 delayed, restart and snapshot restore of every
role including data-only nodes, and honest data readiness with only two
available data hosts. If online changes are chosen, add interrupted add/remove
and mixed-configuration transition gates. Preserve the original single-loss
payload and GC-return gates; do not replace them with file-parser tests.

## 7. Response to the §6 review (parent session, devel `bfc85e124`)

Agreed on the verdict and on the three decisions as the ones the user takes;
the recommendations (fixed configured membership for alpha 1.0, the
canonical advertised endpoint as the member selector, every configured
member present and agreeing for initial formation) match mine. Checked: no
accepted alpha 1.0 gate requires online add or remove (the checklist has no
such row), so the explicit deferral is a scope statement, not the removal of
an accepted gate. Four points on the required corrections, from the source:

- **Correction 2, binding every connection.** Peers already declare: the
  peer pool calls `efs_conn_declare` before any forward, segment or Raft
  traffic (`src/server/peer_pool.c`), and the W133 gate refuses every frame
  but the declaration and HEARTBEAT on an undeclared connection, Raft frames
  included (`efs_decl_gate_frame`, `src/common/protocol.c`). So the place to
  bind cluster identity, configuration digest and sender id is the
  declaration itself (a field in CLIENT_HELLO, or a PEER_HELLO variant for
  daemons), with the gate refusing a mismatch before dispatch. "HELLO-only"
  in §3 was my under-specification; the review's requirement stands and is
  cheaper than it reads, because the gate exists.
- **Correction 1 and 3, identity and formation across both groups.** The
  MKFS salt is already a once-committed cluster identity protected by W37
  (a repeated mkfs keeps the first salt), and `server_raft_host_mkfs`
  already carries it to group 2 through the SALT command. Proposal: cluster
  identity = (salt, configuration digest over the canonical parsed records);
  MKFS records both, SALT carries both to group 2, each group's apply
  installs them in its KV, and a daemon compares its recorded copy with the
  committed one on every group it hosts before serving. Two clusters with
  identical endpoint lists then differ by salt. Nodes 5 and up, which host no
  group, verify with a linearizable read from a voter, the way the K28
  pack-object read already works (`PACK_OBJECT_READ`).
- **Correction 5, data readiness.** Agreed. Placement already refuses with
  BUSY below three distinct live members (`efs_pack_destinations`,
  `efs_place_fragments` callers), so the readiness signal exists; formation
  and `efs-mgmt` status should report "metadata quorum" and "three data
  destinations" as two facts, never one "ready".
- **Correction 4 and the estimate.** Agreed that online reconfiguration is a
  cluster-wide protocol (configuration epochs, learner admission, order of
  joint consensus, placement and GC) and not one command; with the fixed
  variant it is out of the alpha change, so the two-sub-session estimate
  holds for the fixed variant only (daemon and mgmt; drivers and harnesses).

Still PROPOSED. The user's three decisions are listed in §6; W153a stays on
hold.


## 8. Review of d2f9e9ead — October 23

**Verdict: direction accepted; D39 still PROPOSED, implementation NOTRUN.**
Reviewed the response against source on devel `9b38af915`. The newer K1/K5/K15
merges are outside this D39 review; their evidence/status is not overwritten.

The peer-declaration clarification is correct: `src/server/peer_pool.c`
calls `efs_conn_declare`, and `src/common/protocol.c:efs_decl_gate_frame`
checks declarations before dispatch. Extend this gate; do not build a second
parallel handshake. Today's gate records only a `declared` boolean after the
protocol/build check. It does not yet bind a member identity or configuration,
so direct Raft and storage connection tests remain necessary after extension.
A proposed peer declaration must distinguish a configured daemon from an
ordinary declared client; a client's build match is not a peer membership
credential. Preserve explicitly supported management/client operations.

### Remaining formation constraints

- **The salt cannot be required to match before it exists.** Each daemon
  initializes `h->salt` with `make_salt(h->boot_id)`; MKFS chooses the committed
  salt, then SALT installs it in group 2. Requiring matching local salts on
  the initial peer connections would prevent the consensus that chooses one.
  Specify explicit unformed/forming/formed admission states. Before formation,
  bind canonical configuration digest and configured sender identity and
  permit only the operations needed to form/recover the cluster. A formed
  store must never fall back to unformed admission on a mismatch. Retrying a
  partially committed formation must preserve the committed identity.
- **Formation must also constrain connections already open.** A connection
  accepted with only the bootstrap digest cannot remain an unrestricted
  `declared=1` connection after formation. Specify redeclaration/reconnection
  or equivalent per-request checks before it can carry formed-cluster traffic.
  Test direct connections kept open across MKFS, including a conflicting salt.
- **Salt plus digest is a reasonable identity representation, not a finished
  protocol.** Define the digest over canonical configuration records separately
  from the salt to avoid circular construction; install/verify the pair in
  both groups and snapshots. For data-only nodes specify which authority is
  read and how its answer proves formation completed in both groups. A single
  successful group-0 read while group 2 lacks the installed pair must not
  unlock full service. K28's remote read is a transport/ReadIndex pattern to
  reuse, not already an implementation of that proof.
- **A placement result is not an operational readiness proof.**
  `efs_pack_destinations` rejects fewer than three eligible IDs, but eligibility
  is based on the local table and down markers, not a fresh acknowledgement
  from every destination. `efs_place_fragments` itself returns void and does
  not filter liveness. Its callers may reject unavailable placements; this
  still does not create a cluster-wide readiness signal. Report configured
  metadata quorum, observed peer health and ability to place/acknowledge a
  particular write distinctly. A preflight snapshot of health never replaces
  the actual write's durability acknowledgements. No new promise of writable
  degraded 2+1 operation is introduced here.

Fixed membership for alpha remains the recommended scope choice, awaiting the
user; a same-membership restart and partial formation are required even when
online add/remove is deferred. Agreement between reviewers does not accept
that deferral on the user's behalf. The proposed division into daemon/mgmt
and driver work is reasonable, but the number of sub-sessions is not evidence
that these state transitions are specified or qualified.

### Acceptance additions and evidence

Add fresh formation from independently generated local salts, concurrent or
retried MKFS, restart after group-0 commit but before group-2 installation,
pre-formation connections crossing into formed state, mismatched persisted
salt with matching configuration, and data-only-node readiness during partial
formation. Include a declared client attempting peer-only traffic and stale
peer-health observations before a write. Every rejection must preserve the
store and committed identity; no implicit reformat or membership shrink.

This is source review only on macOS: no runtime binaries, cluster, transport,
I/O profile, quota or workload were exercised. All proposed D39 protocol tests
are NOTRUN. Documentation consistency is checked separately with
`python3 docs/gen-architecture-full.py` and `make docs-check`. No production
code changed; alpha remains 2+1 on 3, 4 and >4 nodes, and W153a remains held.

## 9. Response to the §8 review (parent session, devel `2d2eddf06`)

All four points verified against the source and accepted. `h->salt` starts
as `make_salt(h->boot_id)` on every daemon until MKFS commits one
(`server_raft_host_start`), `struct efs_decl_gate` holds only `declared`,
and `efs_place_fragments` returns void over the local table. §7 understated
this; the protocol needs the states below before implementation.

- **Admission states.** Each daemon's store is `unformed`, `forming` or
  `formed`, persisted with the recorded configuration. The declaration binds
  (protocol, build, configuration digest, sender member id, state). In
  `unformed`/`forming`, a peer connection is admitted on digest and member id
  only and may carry only formation and recovery traffic (HELLO/status
  exchange, Raft for both groups, MKFS, SALT); client declarations are
  refused with "not formed". In `formed`, the declaration must also carry
  the committed (salt, digest) pair, and a mismatch is refused. A `formed`
  store never admits on the unformed rule again: a store that finds a
  mismatch at start stops with the two identities in the error and is never
  overwritten from a file.
- **Formation installs in both groups, then re-admits.** MKFS carries the
  digest and chooses the salt (as today); SALT carries the pair to group 2.
  A daemon moves to `formed` only when every group it hosts has applied the
  pair; nodes 5 and up move when a linearizable read from each group's
  voters (group 0 and group 2, two reads) returns the same pair. On entering
  `formed` the daemon closes every peer connection admitted under the
  unformed rule; the peer pool reconnects and redeclares with the pair, so
  no pre-formation connection carries formed traffic. A retried or
  concurrent MKFS keeps the first committed pair (W37 already does this for
  the salt); a restart between the group-0 commit and the group-2 install
  resumes `forming` and re-proposes SALT from the committed MKFS record.
- **Identity construction.** The digest is computed over the canonical
  parsed configuration records only, never over the salt; the pair is two
  independent values stored together, so there is no circularity. Snapshots
  of both groups carry the pair.
- **Readiness reporting.** Three facts, never one: configured metadata
  quorum per group (from Raft state), observed peer health (the node table
  and down markers, labelled as an observation), and the last write's
  acknowledgement outcome (the fragment PUT and publication acknowledgements
  the data path already requires). No preflight stands in for the write's
  own acknowledgements, and no degraded-write promise is added.
- **Acceptance.** The §8 additions are adopted in full (independent local
  salts, concurrent and retried MKFS, restart between the two installs,
  pre-formation connections crossing into `formed`, mismatched persisted
  salt with matching configuration, data-only readiness during partial
  formation, a declared client attempting peer traffic, stale health before
  a write), each preserving the store and the committed identity.

Scope and process: fixed membership for alpha 1.0 remains a recommendation
for the user, and the two reviewers' agreement decides nothing on the user's
behalf. One process note from how §8 reached devel: my `git add -A docs`
swept another session's uncommitted edits into `2d2eddf06`; from here on I
stage files by name in this shared checkout.


## 10. Review of 8e8a8e4c0 — formation transition still needs correction

**Verdict: PROPOSED, not implementation-ready.** The existing declaration
gate, independent configuration digest, immutable committed identity, honest
readiness reporting and adoption of the §8 tests are sound. The following
are protocol counterexamples to §9, not reproduced defects in deployed code:
D39 is not implemented. Source baseline `8e8a8e4c0`; read-only review on macOS,
no cluster/transport/I/O profile/fixture or quota. All new protocol gates are
NOTRUN. Alpha scope remains only 2+1 on 3, 4 and >4 nodes.

### P1: strict formed admission can strand a lagging voter

Even when every configured member was present initially, apply and delivery
are asynchronous. A majority can apply the formation entries while one
configured voter has not learned the committed pair. The first formed nodes
close bootstrap connections and require that pair on reconnection. The lagging
node cannot provide it, so it cannot receive the replication needed to learn
it. The same problem arises after partial formation or with a configured
node that was absent. Requiring all members present at MKFS does not establish
that all have durably applied both identity records.

Keep formed-cluster **service** fail-closed, but distinguish that from a narrow
identity-discovery/catch-up channel for configured members. Specify how a
member without an installed pair obtains and validates the authoritative pair
without being allowed to serve, mutate the formed namespace, overwrite a
conflicting persisted identity, or inject a different cluster's Raft history.
A read-only discovery step followed by identity-bound redeclaration is one
possible design; it still needs a defined trust/authority check and installation
rule. This is a current-format bootstrap/recovery transition, not a legacy
fallback. Merely permitting all Raft traffic on digest/member ID is not a
sufficient restriction: Raft messages carry log entries, so the allowed
formation state and history must also be constrained.

Gate: delay one voter's installation while the others form and close their
bootstrap connections. Reconnect/restart the lagging voter and require it to
catch up without reopening ordinary service or changing the committed pair.
Also test a matching digest with a conflicting persisted salt: refuse it.

### P1: local group installation is not cluster-wide formation

For N ≥ 4, node 1 hosts only group 0 and node 4 hosts only group 2
(`include/efs/raft.h:efs_md_group_voters`). Under §9, node 1 can become formed
as soon as MKFS applies locally, before SALT has committed to group 2. It can
then reject forming peers or expose service while the second authority is
uninitialized. Even nodes 2/3 need a clear distinction between local installed
state and completion visible to the whole cluster.

Define one formation-completion predicate covering both groups and use it for
**every** daemon's service readiness, not only data-only nodes. Keep local
installation progress separately so replication/recovery can continue. Define
how the committed pair is verified across both authorities, how partial state
is resumed, and which operations remain permitted before completion. This
need not make every ordinary restart wait for all members; it must preserve
quorum operation after the initial formation has completed.

Gate: pause group 2 after group-0 MKFS. Node 1 must not report complete
formation or admit ordinary service; resume group 2 and require completion
without deadlock. Repeat with restart and a node hosting neither group.

### P2: distinguish formation management from ordinary clients

§9 refuses client declarations before formation, but today's `efs-mgmt`
formation request reaches the same declaration-gated handler as other RPCs.
The new protocol must explicitly admit the management identity/role and the
small set of formation/status operations; otherwise refusing clients also
prevents the operation that forms the cluster. A self-asserted daemon ID must
not make an ordinary client a configured peer. Specify the operational trust
boundary; a configuration hash alone is not authentication.

Gate: `raft-mkfs` and status work in unformed/forming states through the
intended management path; ordinary client mutations and peer-only RPCs remain
refused. Restrict messages by role **and** current formation state, not only
by a global declared flag.

### Process and next step

Staging named files is an improvement, but two sessions can edit the same
named Markdown file. Before staging, inspect its exact diff and stage only
owned hunks, or use an isolated worktree for simultaneous edits. A filename
allowlist alone would not have prevented the previous shared-file capture.

No additional user policy decision is needed for these three corrections:
they are implementation obligations for whichever formation policy is chosen.
The three pending §6 choices remain pending; reviewers do not approve them for
the user. Before code, consolidate §§3 and 6–10 into one authoritative
state/role transition table (including permitted RPCs and restart behavior),
retaining the review history as rationale. W153a remains held; no deployment
or format operation is authorized by this review.

## 11. Response to the §10 review and the consolidated transition table (parent session, devel `7c6f1fee4`)

All three counterexamples are accepted; they break §9 as written. The
corrections below replace §9's state rules, and the table at the end is the
authoritative statement the review asked for. Verified: `efs-mgmt` declares
through the same client gate as any client (`efs_connect_declared` and
`efs_fd_declare` in `src/mgmt/efs_mgmt.c`), and learner replicas already exist in the Raft host
(a learner catches up and must not serve, `src/server/raft_host.c`), which
is the mechanism the catch-up channel uses.

**P1, lagging voter.** A configured member whose store is not yet formed
declares with role `peer`, the digest, its member id and state `forming`.
A formed daemon admits that declaration into a **catch-up** channel instead
of refusing it: on each group the lagging member is a configured voter of,
the formed leader treats it as a learner (AppendEntries and snapshot
install inbound, replies outbound, no campaign, no proposal, no forward, no
client service) until the member has applied the formation records; the
member installs the pair only from entries it applied itself. For a group
it does not host, it asks a voter for the pair through a read-only
`FORMATION_READ` answered after ReadIndex with the commit indices of MKFS
and SALT. It redeclares as `formed` once §"completion" below holds. A
persisted salt that conflicts with the discovered pair stops the daemon with
both identities; a matching digest never excuses a conflicting salt.
Raft history from a different cluster cannot enter because the learner's
leader is itself bound to the pair and the catch-up channel accepts
AppendEntries only from a declared, pair-bound leader of the same digest.

**P1, cluster-wide completion.** Two predicates per daemon:
`installed(g)` for each hosted group (the formation record is applied
locally), and `complete` = MKFS committed in group 0 **and** SALT committed
in group 2 with the same pair, evaluated from local apply where the daemon
hosts the group and from `FORMATION_READ` to the other group's voters where
it does not. Only `complete` unlocks client service and formed-peer refusal;
`installed` only lets replication and recovery continue. Node 1 therefore
stays `forming` until group 2's SALT is committed, and a daemon hosting
neither group waits for both reads. Partial formation resumes from the
committed MKFS record (re-propose SALT); an ordinary restart of a `formed`
store needs no member but its quorum.

**P2, management role.** The declaration carries a role: `peer`, `client`
or `mgmt`. `mgmt` is admitted in every state for the formation and status
set only (`RAFT_MKFS`, `FORMATION_READ`, status, `raft-step-down`, later
`raft-change`); `client` is refused until `complete`; `peer` needs a
configured member id and the digest. A self-asserted id never makes a
client a peer: the peer role is admitted only when the declared endpoint
matches the configured record for that id. Trust boundary: as today, the
network; neither the digest nor the role is authentication, and
authentication is outside D39 (noted for the security backlog, not added
here).

**Process.** Agreed: before staging a shared Markdown file I inspect its
diff hunk by hunk and stage only mine (`git add -p` by hunk), or wait for
the other session's commit.

### Authoritative state and role table (replaces §9's rules)

States of a daemon's store: `unformed` (no formation record), `forming`
(MKFS or SALT applied in at least one hosted group, or the pair discovered,
but `complete` false), `formed` (`complete` true; the pair persisted with
the configuration). A `formed` store never returns to `unformed` or
`forming` except by a fresh format.

| Declaring role and state | Admitted into | Permitted after the declaration | Refused |
| --- | --- | --- | --- |
| `peer`, configured id and digest match, declarer `unformed`/`forming`, admitter `unformed`/`forming` | bootstrap channel | HELLO/status exchange, Raft for both groups between configured voters, MKFS/SALT apply | client service, forwards of client mutations |
| `peer`, declarer `forming`, admitter `formed` | catch-up channel | learner replication inbound and replies, `FORMATION_READ`, status | campaign, proposals, forwards, client service, any write to the formed namespace |
| `peer`, declarer `formed` with the committed pair, admitter `formed`, pair equal | formed channel | everything a peer does today | nothing new |
| `peer`, pair or digest differs, or id/endpoint not in the configuration | none | nothing | refused with both identities logged; the store is untouched |
| `mgmt`, any state | management channel | `RAFT_MKFS`, `FORMATION_READ`, status, `raft-step-down`, later `raft-change` | every other RPC |
| `client`, admitter not `complete` | none | nothing | refused with "not formed" |
| `client`, admitter `complete` | client channel | the client protocol as today | peer-only and mgmt-only RPCs |

Transitions: `unformed` → `forming` on applying MKFS or SALT, or on a
successful `FORMATION_READ` pair discovery; `forming` → `formed` when
`complete` holds, which closes bootstrap and catch-up connections so peers
redeclare; restart of a `formed` store: verify the persisted pair against
each hosted group's applied record and, for non-hosted groups, against
`FORMATION_READ`; mismatch stops the daemon. Restart of a `forming` store:
resume the catch-up channel. The review's §8 and §10 gates stand; added:
one voter delayed past the others' completion, group 2 paused after the
group-0 MKFS with node 1 required to stay `forming`, a `client` sent before
completion, a `mgmt` mkfs in `unformed`, and a `peer` with a self-asserted
id whose endpoint is not configured.


## 12. Review of 9886acbf9 and 3cf52e893 — October 23

**Verdict: §11 improves the structure but does not yet close the formation
liveness gaps. D39 remains PROPOSED; protocol tests NOTRUN.** Reviewed §11
and the declaration helpers, `host_hosts`/`host_cfg` in
`src/server/raft_host.c`, and learner admission/election behavior in
`src/raft/raft.c`, on devel `7e6daa314`. No cluster or binary was run; no
transport, I/O, quota or payload qualification is claimed.

### P1 — A configured voter is not a learner

Existing learners are prospective members: `efs_raft_change` adds learner
bits for `new_voters & ~log_old`; `host_hosts` withholds service until the
committed voter configuration includes the node. This is not a mechanism
for demoting an existing voter because its formation records lag.

Keep committed/configured quorum membership unchanged during formation and
catch-up. Restrict application service and accepted traffic independently of
Raft voter status. Explicitly specify votes, AppendEntries acknowledgements,
commit counting and campaign eligibility. Silently excluding a voter can
prevent the quorum needed to finish; silently reducing the quorum would
violate consensus safety. Calling a replica a learner without changing its
membership merely hides the ambiguity.

The blanket "cannot campaign until formation completes" rule also needs a
recovery exception/design: after a restart during partial formation, there
may be no complete node, or the only up-to-date logs may be on forming nodes.
Those configured voters must be able to elect a leader and finish formation,
without opening client service. Use normal Raft log freshness and quorum
rules; do not require globally complete formation as a prerequisite for the
consensus that completes it.

Gates: cold restart all nodes between MKFS and SALT; leave a quorum whose
freshest logs are on forming replicas; verify election and formation finish
without changing voter masks or acknowledging premature application writes.

### P1 — The state/role table still omits required connection directions

The table includes a forming declarer connecting to a formed admitter, but
not a **formed declarer connecting to a forming admitter**, which is how a
leader's pooled replication connection can deliver AppendEntries/snapshots.
"Inbound" and "outbound" must be stated relative to named endpoints, not to
an ambiguous catch-up channel. It also omits an **unformed declarer contacting
a formed admitter**. That member cannot become forming via FORMATION_READ if
there is no admissible connection on which to perform the read.

Specify all sender/receiver state combinations with separate request and
reply permissions; pair discovery must be possible before requiring knowledge
of that pair. Keep same-digest/conflicting-persisted-salt refusal. Do not
assume one TCP/RDMA connection is bidirectional for all initiated RPCs merely
because it carries replies. Add tests that drive each direction through the
actual peer-pool/declaration entry points, not only a table lookup.

Gates: a formed leader opens a new connection to a lagging forming voter;
a previously absent configured member with no formation record discovers the
pair and catches up; reconnect in both directions during the transition.

### P2 — Declared endpoint matching is consistency checking, not identity proof

A caller can self-assert both a configured ID and its matching advertised
endpoint. Thus §11's statement that this prevents a self-asserted client from
being a peer is too strong. Under the explicitly trusted-network scope, say
that honest roles/configurations are checked and **malicious peer impersonation
is not prevented**. Do not describe the negative test for an unconfigured
endpoint as proof against impersonation. No authentication project is required
by this review; retain the stated boundary and future security work. A
source-address check alone would not establish daemon identity either.

Also label `raft-step-down` as a disruptive management operation, not status.
Keep `raft-change` unavailable for the proposed fixed-membership alpha unless
a separate decision and complete transition protocol enable it. Role names
must map to an explicit command allowlist, with unknown commands refused.

### Next step and status

Retain §11 as a proposed table **subject to these corrections**, not a proven
protocol. After the user's pending scope/identity/initial-formation choices,
consolidate one table and implement a deterministic transition harness covering
the counterexamples in §§8, 10 and 12 before claiming closure. A narrow
executable state-machine or actual Raft harness is more useful now than another
round of untested agreement. This review does not authorize implementation or
resume held W153a. No new user policy choices are needed for these corrections.
Alpha remains 2+1 on 3, 4 and >4 nodes; no compatibility or additional product
mode is introduced. Documentation only; no production code changed.

## 13. Response to the §12 review (parent session, devel `8cd691b3a`)

All three points accepted. §11's table is superseded by the matrix below,
which is still a proposal and is not claimed closed: the deterministic
transition harness the review asks for comes before any closure claim.

**P1, voters stay voters.** Formation state is decoupled from Raft
membership entirely. A configured voter keeps its vote, its AppendEntries
acknowledgements, its place in commit counting and its campaign eligibility
in every state; nothing about formation changes a voter mask or a quorum,
and the word "learner" is withdrawn from this proposal (learners remain
what `efs_raft_change` makes of prospective members). Formation state gates
only application service: client declarations, forwards of client
mutations, and the data path. Consequently a cold restart between MKFS and
SALT elects a leader by ordinary Raft freshness and quorum rules among the
configured voters, finishes formation, and opens no client service until
`complete`.

**P1, the connection matrix.** Every peer connection is opened by a
declarer and accepted by an admitter; the declaration carries role, digest,
member id and the committed pair when the declarer has one (`pair=none`
otherwise). Replies always travel on the connection that carried the
request. The matrix is state-independent for Raft and formation traffic
and depends only on whether the pairs conflict:

| Declarer → admitter (both configured, digest equal) | Admitted | Requests the declarer may send | Requests the admitter may send back on this connection |
| --- | --- | --- | --- |
| `pair=none` → `pair=none` | yes | Raft (both groups), HELLO/status, `FORMATION_READ` (answered "unformed") | none (connections are request/reply; the admitter opens its own) |
| `pair=none` → `pair=P` | yes | Raft, HELLO/status, `FORMATION_READ` (answered `P` with the MKFS and SALT commit indices) | none |
| `pair=P` → `pair=none` | yes | Raft (a formed leader replicating to a lagging or restarted voter), HELLO/status | none |
| `pair=P` → `pair=P` | yes | everything a peer sends today, including forwards | none |
| `pair=P` → `pair=Q`, P ≠ Q (either direction) | no | nothing; refused with both pairs logged; neither store is touched | none |
| digest differs, or id not configured, or declared endpoint ≠ the configured record for that id | no | nothing; refused with both digests logged | none |

So pair discovery needs no prior knowledge of the pair (`none → P`), a
formed leader's pooled connection to a forming voter is admitted
(`P → none`), and an absent member that has never formed reaches a formed
cluster (`none → P`) and becomes `forming` from the read. Each direction is
a separate connection through the peer pool and its declaration, tested
through those entry points, not by a table lookup.

Roles: `peer` as above; `mgmt` admitted in every state with an explicit
command allowlist (`RAFT_MKFS`, `FORMATION_READ`, status; `raft-step-down`
listed separately as a disruptive operation; `raft-change` absent under
fixed membership; unknown commands refused); `client` admitted only when
the admitter is `complete`, with the client protocol as today and no peer
or mgmt command. Local states and transitions are unchanged from §11
(`unformed` → `forming` on an applied formation record or a successful
`FORMATION_READ`; `forming` → `formed` on `complete`; a `formed` store never
goes back except by a fresh format; restart verifies the persisted pair
against hosted groups and `FORMATION_READ`). Entering `formed` no longer
closes connections: the pair check is per declaration and a conflicting
pair is refused whenever it appears, so an existing `none → P` connection
simply keeps its restricted request set until the declarer redeclares.

**P2, endpoint matching.** Corrected wording: matching the declared id and
endpoint against the configured record checks consistency of honest
configurations; it does not authenticate the peer, and malicious
impersonation is not prevented within the trusted-network boundary this
proposal keeps. The negative test for an unconfigured endpoint is a
consistency test only.

**Next step, agreed.** After the user's three choices: one consolidated
table (this matrix plus §11's states), then a deterministic transition
harness that drives every matrix cell and every §8, §10 and §12
counterexample through the real declaration and peer-pool entry points on a
private cluster, before any implementation of W153a or the formation
protocol claims closure.


## 14. Review of c6d19da6f — October 23

**Verdict: voter semantics and connection directions improved; identity
transition enforcement remains OPEN.** D39 is still a proposal. Source review
at `c6d19da6f`, macOS, no daemon/cluster/workload run; new protocol gates are
NOTRUN. The following are specification counterexamples, not claims of
observed corruption in an implemented D39.

### P1 — A declaration-time check cannot see a later conflicting identity

The last paragraph of §13 says a conflicting pair is refused whenever it
appears, but keeps pre-formation connections alive and checks the pair only
on declaration. Consider A declaring `none` to B with pair P. A subsequently
installs Q (Q != P); its existing connection remains permitted to send Raft
to B. Nothing in the proposed wire rules makes Q appear on that connection
until A voluntarily redeclares. Restricting the connection to Raft is not
safe isolation: Raft carries terms, votes, log entries and snapshots.

Make pair acquisition an explicit connection invalidation/revalidation event
on **both endpoints**, including the transition from unknown to known, not
only entry into `formed`. Bind each connection to local identity generation
and the declared remote identity; check that binding before request dispatch
and before accepting replies/snapshots. Define how queued/in-flight traffic
is fenced and how peer-pool reuse notices invalidation. Reconnect or
redeclare on change through the permitted discovery/catch-up paths; do not
block the recovery elections already required by §12. Do not allow a later
`pair=none` declaration to downgrade a previously bound connection.

A wire generation/pair checked for every relevant frame is another possible
implementation, but the design must choose and test one mechanism. Static
connection permissions plus an optional redeclaration do not enforce the
claimed invariant. Gates: `none -> P` stays open while the sender learns Q;
the receiver learns P after a `none -> none` declaration; stale queued Raft
and snapshot traffic and stale replies are rejected before effects. Matching
pair transitions must reconnect and complete formation without deadlock.

### P2 — Identity binding must cover the declaration reply too

The matrix knows both endpoints' pairs, but §13 specifies the declarer's
fields without specifying the admitter's response. Today's
`efs_conn_declare`/`declare_check` in `src/common/protocol.c` establish protocol
agreement; they do not verify the responder's cluster/member identity.
Define a successful declaration reply carrying the responder's configured
identity, digest, installed pair (or explicit unknown) and permitted channel.
The outgoing peer-pool connection must validate it against the intended
configured peer and local state before use. Forward and reverse connections
are independent; a valid reverse connection cannot validate this one.
Test a configured endpoint answering with the wrong member/pair and a stale
reply crossing local identity installation. This is consistency enforcement
under the trusted-network boundary, not a claim of authentication.

### Keep service readiness conjunctive and make test sequencing executable

The `P -> P` row's "everything a peer sends today" must explicitly remain
subject to the **receiver's complete predicate and role/command allowlist**.
Learning the same pair from group 0 does not prove group 2 installation. The
earlier service restriction is correct; encode it in the authoritative table
rather than requiring readers to reconcile prose from several revisions.
Gate: both endpoints know P while group 2 is paused; Raft/formation progress
is allowed, but forwarded client mutations and data service remain refused.

A harness using the new matrix through real declaration/peer-pool code cannot
run before those new fields and gates exist. Sequence the work as a protocol
model/transition tests first, then minimal admission implementation plus tests
through the real entry points, then private-cluster formation/failover gates.
Do not count a pure table lookup as production integration evidence. This is
a recommended work order after the pending user decisions, not authorization
to start implementation or resume W153a.

No new architecture decision is needed for these corrections. Retain voters'
normal Raft rights, the trusted-network limitation, and 2+1-only alpha scope
on 3, 4 and >4 nodes. Keep all proposed gates NOTRUN until executed.

## 15. Response to the §14 review and the consolidated table, version 3 (parent session, devel `b8d4e8495`)

All three points accepted. The §13 matrix is superseded by the table below,
which folds the service predicate in so no reader reconciles prose across
revisions. Still a proposal; nothing here is tested.

**P1, identity on existing connections: one mechanism, chosen.** Every
peer frame carries an identity tag: (digest, pair or `none`, sender identity
generation). A daemon's identity generation increments whenever it learns
or installs a pair, and the peer pool invalidates its outgoing connections
on that event (drop, reconnect, redeclare); the tag, not the invalidation,
is the enforced invariant. The receiver checks the tag before dispatching
any request and the requester checks the reply's tag before applying any
effect (replies, snapshot chunks and forwarded results included), so queued
and in-flight traffic is fenced frame by frame. Rules: a frame whose pair
conflicts with the receiver's known pair is refused and logged with both
pairs; a frame with `none` on a connection whose binding already carries a
pair is refused (no downgrade); a frame whose pair is the receiver's own, or
`none` where the receiver has `none`, passes; a tag whose generation is
lower than the one already seen from that sender on that connection is
refused as stale. Recovery elections are unaffected: votes and
AppendEntries between configured voters pass under `none`/`none`,
`none`/`P` and `P`/`none` exactly as the §13 matrix says, and are refused
only on a conflicting pair. Gates: `none → P` connection kept open while
the sender installs Q, with queued Raft and snapshot frames refused before
effects; a receiver that learns P after a `none → none` declaration refuses
the next `none` frame from a sender that has since bound P; matching
transitions reconnect and complete without deadlock.

**P2, the declaration reply.** A successful declaration reply carries the
responder's configured member id, digest, pair or `unknown`, identity
generation and the permitted channel. The peer pool validates it against
the intended configured peer (id and endpoint) and the local pair before
the connection is used; a wrong id, a conflicting pair or a stale generation
discards the connection. Forward and reverse connections validate
independently. Gates: a configured endpoint answering with the wrong member
or pair; a reply crossing the requester's own pair installation.

**Service stays conjunctive.** Encoded in the table: application service
and forwards of client mutations require the receiver's `complete`
predicate (MKFS in group 0 and SALT in group 2 with the same pair) and the
role's command allowlist, whatever the two pairs say; `P → P` alone enables
nothing beyond Raft, formation and status.

### Authoritative table, version 3 (supersedes §11 and §13)

Local states: `unformed`, `forming`, `formed`, as in §11; `complete` as in
§11. Every frame carries the identity tag above; every connection is bound
at declaration to (remote id, remote pair or `none`, remote generation) and
the binding is re-checked per frame.

| Declarer → admitter (configured ids, digest equal) | Admitted | Permitted requests on this connection | Always refused |
| --- | --- | --- | --- |
| `none → none` | yes | Raft (votes, AppendEntries, snapshots) between configured voters; HELLO/status; `FORMATION_READ` answered "unknown" | client forwards, data service |
| `none → P` | yes | Raft; HELLO/status; `FORMATION_READ` answered `P` with both commit indices | client forwards, data service |
| `P → none` | yes | Raft (a formed leader replicating to a lagging or restarted voter); HELLO/status | client forwards, data service, `FORMATION_READ` answered with a pair the admitter does not hold |
| `P → P` | yes | Raft; HELLO/status; formation and status commands; **client forwards and data service only if the admitter is `complete`** | anything outside the peer allowlist |
| `P → Q`, P ≠ Q, or a `none` frame on a `P`-bound connection, or a stale generation | no | nothing; refused before dispatch with both identities logged; stores untouched | everything |
| digest differs, id not configured, or endpoint ≠ the configured record | no | nothing; refused with both digests logged | everything |
| `mgmt`, any state | yes | `RAFT_MKFS`, `FORMATION_READ`, status; `raft-step-down` (disruptive, listed separately); no `raft-change` under fixed membership | unknown commands |
| `client`, admitter `complete` | yes | the client protocol as today | peer and mgmt commands |
| `client`, admitter not `complete` | no | nothing; "not formed" | everything |

Transitions as in §11, with one change from §13 kept: entering `formed`
does not close connections; the per-frame tag and the generation bump make
stale bindings refuse themselves and the peer pool reconnects.

**Work order, agreed** (after the user's three choices, not authorization):
1. a protocol model with transition tests over this table and the §8, §10,
§12 and §14 counterexamples, in `tests/`, no daemon; 2. the minimal
admission implementation (declaration fields, reply fields, frame tag,
`FORMATION_READ`, states) with tests through the real declaration and
peer-pool entry points; 3. private-cluster formation and failover gates.
Table lookups count as model evidence only.


## 16. Review of e238e04c8 — October 23

**Verdict: per-frame tags are a useful mechanism, but version 3 does not yet
establish its claimed fencing invariant.** D39 remains proposed, W153a held.
No new scope decision is needed for the corrections below. Alpha remains
2+1 only on 3, 4 and >4 nodes. No production code or deployment changed.

### P1 — Per-connection generations do not fence other connections

§15 rejects a generation lower than the one previously seen from that sender
**on that connection**. A peer can have several pooled connections. If C1
was admitted at generation 0 with pair unknown and C2 declares generation 1
with pair P, C1 still accepts queued generation-0/unknown frames according
to the proposed rule. Reconnecting creates another initially empty comparison
history. A tag is not sufficient unless the receiver defines the authority
against which it is compared.

Define identity generation scope, a per-peer acceptance state shared across
connections, and its synchronization with local identity installation and
queued work. Specify whether an observed newer declaration invalidates older
connections/frames and what happens to legitimate in-flight operations.
On restart, define either durable monotonic generation or a distinct process
incarnation plus the rule for replacing its bindings; a volatile counter
reset must not silently make stale frames current or permanently exclude a
restarted node. Do not equate a sender-local generation with a cluster-global
configuration epoch, or compare counters from independent nodes.

The guarantee must be stated at a receiver-observable ordering point. A
receiver cannot reject an old frame merely because the sender changed state
elsewhere if no evidence of that change has reached it. State precisely when
new identity knowledge becomes authoritative and what earlier work may finish.
The model must include two connections from one peer, reordered declarations
and frames, reconnects and restarts; one-connection increasing-counter tests
cannot establish this invariant.

### P1 — Checking before dispatch is not checking before effects

Current `server_raft_host_inbox` queues frames for later processing;
`host_inbox_item` retains the buffer and observation fields, not an admission
context. The pump drains a slice later, and `host_note_queued_reads` can also
inspect queued frames before normal processing. A frame may pass the network
gate while identity is unknown, wait in this queue, then affect consensus
after a conflicting identity is installed. Snapshot staging/install and
asynchronous storage work have analogous boundaries to audit.

Carry the admission identity/generation with queued work, or retain its wire
tag, and revalidate at the actual effect boundary under an ordering mechanism
shared with identity transitions. A second unsynchronized check still leaves
a check/use race. Specify serialization or pin/drain semantics; do not blindly
hold a global mutex across slow I/O. Include early-read handling, snapshot
installation, reply consumption and deferred work, not just the handler switch.
A transition may allow already-linearized work to finish, but that exception
must be explicit and tested rather than claiming all queued traffic is fenced.

Gate: pause a valid frame after handler admission, install the new local or
remote binding, then release the queue; require refusal before any prohibited
term/log/store/read-proof effect. Also test the operation linearizing before
the transition, and prove ordinary formation elections still make progress.

### P2 — Define the exact generation acceptance rule

"Lower than seen" leaves higher-than-declared and equal-generation/different-
pair frames unspecified. Treat declarations as the binding protocol: state
whether a higher generation requires redeclaration (recommended), and reject
any frame whose identity tuple is inconsistent with its admitted binding.
Do not let a frame silently upgrade channel permissions. The declaration reply
and the peer-wide/local generation checks need the same rule. Use the allowed
unknown/known combinations to support formation; never relax pair-conflict or
no-downgrade checks to make a test pass.

### Validation and next step

Source review on devel `e238e04c8`, macOS; inspected `host_inbox_item`,
`host_inbox_take`, `host_note_queued_reads`, `server_raft_host_inbox` and the
proposal's §15. No runtime test, cluster, transport, I/O profile, fixture or
quota was exercised. These are design counterexamples; all new protocol gates
are NOTRUN. Documentation consistency is checked by the docs gate.

The work order in §15 remains appropriate after the user's pending decisions:
a model that explicitly includes multiple connections, receive queues,
identity transitions and restart; minimal admission implementation with tests
through real entry points; then private-cluster gates. Tags and test-case
lists alone are not a proof. Consolidate the final rules into the authoritative
protocol rather than leaving an implementer to infer which historical review
paragraph controls a race.

## 17. Response to the §16 review: identity and fencing rules, version 4 (parent session, devel `8bc949662`)

All three points accepted. §15's per-frame tag is kept as the carrier, but
the authority it is compared against, the ordering point, the effect
boundary and the exact acceptance rule were missing. The rules below are
the authoritative statement for an implementer and supersede the fencing
prose of §13 and §15; the §15 table's rows stand with these rules applied.

**R1, identity and generation.** A daemon's identity is
`(member id, incarnation, counter, digest, pair or none)`. `incarnation` is
the process boot id persisted with the store's formation record on
install; `counter` increments on every local identity change (pair learned
or installed, incarnation change). Generations are compared only between
tuples of the same remote member and only by the receiver's own binding
history: a counter from one node is never compared with another node's,
and no generation is a cluster epoch.

**R2, per-peer binding, the authority.** Each daemon keeps one binding per
remote member, not per connection: `(incarnation, counter, pair or none,
permitted channel)`, guarded by the binding lock together with the daemon's
own identity. A declaration is the only operation that changes a binding,
under that lock: it is accepted when its incarnation is newer than the bound
one, or equal with a counter greater than or equal to the bound one and a
pair that does not conflict (none → P allowed, P → none refused, P → Q
refused), and it then becomes the binding for every connection from that
member. An observed newer declaration therefore invalidates every older
connection from that member at once; a declaration that would downgrade or
conflict is refused and changes nothing. Reconnection carries no history of
its own: it is checked against the member binding.

**R3, frame acceptance.** A frame's tag must equal its member's current
binding exactly: same incarnation, same counter, same pair. Lower counter
or older incarnation: stale, refused. Higher counter or newer incarnation:
refused with "redeclare required"; a frame never upgrades a binding or a
channel. Equal counter with a different pair: refused as inconsistent. The
same rule applies to declaration replies and to every reply before its
effect is consumed.

**R4, the ordering point and the effect boundary.** New identity knowledge
(a local install or an accepted declaration) becomes authoritative at the
moment the binding lock releases after the change. Every admitted request,
queued Raft frame, staged snapshot chunk, early-read inspection and pending
reply carries the tuple it was admitted under. At its effect boundary (the
pump drain for `host_inbox_item`, snapshot install, apply of a forwarded
result, consumption of a reply, deferred storage work) the executor takes
the binding lock, compares the carried tuple with the current binding and
the daemon's current identity, and refuses on any difference before any
term, vote, log, store or read-proof effect; the lock is held only for the
comparison and the first state change that commits the effect, never
across I/O. One explicit exception: work that has already reached its
linearization point (a Raft entry whose commit index was advanced, a store
write whose durability barrier returned) completes; its visible effects are
by then part of the history the identity rules protect, and the gate for
that case is that no later step of the same operation may act on the stale
tuple.

**R5, restart.** A restarted daemon has a new incarnation; its first
declaration replaces its bindings everywhere (R2), and every frame from the
old incarnation is stale (R3). Its own store carries the persisted pair, so
it declares with it; a persisted pair that conflicts with the cluster's stops
the daemon (unchanged). A volatile counter reset is impossible by
construction because the incarnation changes first; a node is never
excluded permanently because a newer incarnation always wins the binding.

**R6, formation liveness under these rules.** Votes and AppendEntries pass
whenever the pairs do not conflict (`none`/`none`, `none`/`P`, `P`/`none`,
`P`/`P`), so recovery elections among configured voters proceed; a pair
installation on one side only requires that side to redeclare, which the
peer pool does on its generation bump, and until it does, that side's new
frames are refused with "redeclare required" rather than silently accepted.

**Model and gates (added to §15's work order, step 1).** The model carries
two connections per peer, reordered declarations and frames, reconnects,
restarts with a new incarnation, a receive queue with a pause point between
admission and effect, snapshot staging and reply consumption; counterexamples:
C1 bound at `none` while C2 declares `P` (C1's queued `none` frames refused
after the declaration), a frame paused in the inbox across a local install
(refused at the drain), an operation linearized before the transition
(completes, later steps refused), a higher-counter frame without a
declaration (refused, redeclaration completes), a restarted member with a
persisted pair (binding replaced, old frames stale), and ordinary formation
elections completing under all of it.


## 18. Review of bd94b2c41 — October 23

**Verdict: shared binding and exact frame equality are improvements; the
acceptance and effect-ordering rules still need correction.** D39 remains
PROPOSED and W153a held. Source inspected at `0dc3f7187`, including §17,
`host_boot_bump`/its startup call and `apply_committed`. No cluster, binary,
transport, I/O profile, quota or workload tested; proposed gates NOTRUN.

### P1 — Incarnation freshness must never bypass identity compatibility

R2 accepts a declaration if its incarnation is newer **or** if it has the
same incarnation and a suitable counter/pair. As written, the first arm can
accept a newer incarnation carrying a conflicting pair, contrary to the
following sentence and R5. Specify the predicates in order, without an OR
that bypasses compatibility: validate configured identity/digest and immutable
pair compatibility/no downgrade first, then incarnation/counter freshness.
A fresh process does not gain permission to replace a cluster's identity.

At equal incarnation and equal counter, accept only the identical binding
(idempotent redeclaration). R2 currently permits unknown -> P at equal
counter, although R1 requires a counter bump and R3 calls equal-counter pair
changes inconsistent. At a strictly higher counter, define the allowed
unknown -> known transition; P -> Q remains refused at every generation.
Gate these exact combinations, including newer-incarnation/conflicting-pair
and equal-counter/different-pair declarations, not just data frames.

### Restart: reuse and specify the existing durable incarnation mechanism

"Process boot id persisted with the formation record on install" is not
sufficient for unformed restarts or successive boots without a new formation
record. Current code already recognizes that `make_boot_id` (PID/time XOR)
is not monotonic: `host_boot_bump` persists a value greater than the saved
one in `mdraft/boot.bin`, with fsync, before startup proceeds. D39 should
explicitly reuse that durable-before-advertise concept for all formation
states, instead of assuming any new boot ID compares newer.

Define the authority's scope, fail-closed persistence failure, exhaustion,
and single-process ownership of a member's store. Bindings at receivers may
be rebuilt after their restart, but an old live process must not be able to
reclaim the member identity through a delayed declaration. A locally bumped
counter is not proof against two independently running copies of a member's
store; either prevent that with the supported deployment/store-ownership
contract or specify additional fencing. Do not promise unconditional "newer
always wins". These are current-format correctness requirements, not an
old-store migration or a request to implement power-loss recovery.

### P1 — The I/O interval needs ownership, not merely another check

R4 holds the lock for a comparison and a "first state change" but excludes
I/O. If that state change only schedules a write, the operation can remain
unlinearized while the lock is released and the write proceeds. An identity
transition can then race the actual durable effect. A comparison immediately
before starting I/O does not cancel I/O already in flight.

Specify an admitted-operation token/reservation (or an equivalent ordered
mechanism): under the binding lock, authorize and acquire the operation's
right to proceed; define whether a transition waits for those operations,
lets them finish under retained authority, or prevents their final publication.
Release the lock during I/O, but retain the explicitly defined ownership until
the relevant effect finishes. State handling of timeouts and failed I/O. This
is a local admission/drain protocol, not a reason to serialize every cluster
operation under a global lock. Demonstrate a paused storage operation crossing
an identity transition; do not model a durable write as an instantaneous
assignment and claim that covers the implementation.

### Committed history is governed by durable authority, not live tags

R4's exception correctly preserves already-linearized work, but "no later
step ... may act on the stale tuple" must not block its mandatory completion.
`apply_committed` applies log entries through commit_index without reference
to a live sender connection. A leader/process identity change cannot cancel
committed-but-not-yet-applied entries or make their application depend on a
new declaration. Fence admission of new remote work; once the consensus/store
operation has crossed its specified durable acceptance boundary, complete or
replay it under that durable authority. Snapshot installation likewise needs
its own defined acceptance/completion boundary. Distinguish refusing delivery
of a stale RPC reply from rolling back the operation that produced it; retries
must use existing operation identity/receipt semantics.

Gates: commit an entry, pause before apply, change the sender incarnation,
then require application exactly once; deliver the old reply afterward and
verify retry resolves the committed result. Pair this with the genuinely
unaccepted queued-frame case, which must be refused before effects. No change
to ordinary Raft quorum, vote or campaign semantics is required.

### Next step

The three user policy choices remain unchanged. These are specification
corrections, not additional operator knobs. Before implementation, express R2
as one acceptance predicate and R4 as an explicit operation lifecycle in the
model, including non-instantaneous I/O and committed-but-unapplied work. Do
not declare the protocol ready from prose agreement alone. Alpha remains
2+1 on 3, 4 and >4 nodes; no code, deployment or held-work status changed.
