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
