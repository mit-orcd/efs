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
