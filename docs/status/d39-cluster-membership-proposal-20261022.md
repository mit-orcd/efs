# D39 proposal — authoritative configured cluster membership (Oct 22, 2026)

**Status: PROPOSED, awaiting the user's decision. No code changed.** Written
at the user's request after the W153 review
([integration review](devel-integration-review-20261022.md#review-of-a76b63f42--w153-is-an-alpha-correctness-gate)):
W153a (fail-closed `EFS_MD_RAFT_N`) is on hold until this is decided.

## 1. What exists today (devel `40c0465ab`)

Identity and membership are three unrelated mechanisms with no shared source
of truth:

| Mechanism | Where it lives | Who sets it | What checks it |
| --- | --- | --- | --- |
| Node identity `--node-id`, `--addr`, `--port` | daemon command line | the operator or driver (`cluster.sh`, `nuc-efs/start.sh`: ids 1..N in start order) | the Raft host refuses an id outside `1..n`; HELLO refuses a duplicate id only by overwriting the row (`server_dedupe_nodes_locked`). `scripts/server.sh` derives ids from the address (`octet·65536+port`), which the Raft host rejects; every driver bypasses it |
| Membership table (`struct efs_node[EFS_MAX_NODES]`) | in memory, persisted to `<root>/meta/cluster_nodes.bin` by `server_save_nodes` | `--join <seed>` sends HELLO; the seed appends the node (any id ≤ 16, up to 16 rows), answers its whole table and epoch, and gossips the change; the joiner adopts the table; restarts rejoin through persisted peers | build id and protocol version (fail-closed); capacity. **Nothing states which nodes are supposed to be members**: a table of 3 and a table of 4 are both valid |
| Metadata topology `n` (`EFS_MD_RAFT_N`) | daemon environment, default 4 | each driver independently (`cluster.sh` passes the host count; test scripts pass 3) | range `3..16` only; unset, empty or `3junk` is accepted. Seeds every group's voters (`efs_md_group_voters(group, n)`); the all-member GC handoff waits for ids `1..n` to be present |
| Committed Raft membership per group | the group's Raft log and `mdraft/desired.<group>`; changed by `efs-mgmt raft-change <addr> <group> <mask>` (NOTE every peer, then joint-consensus `efs_raft_change`) | the operator, after bootstrap | the Raft engine (quorum, joint consensus); becomes authoritative once committed and overrides the seed on restart |
| Table formation `efs-mgmt raft-mkfs <seed>` | one operator call | the driver, once, on the seed | needs a quorum in both groups to commit MKFS and SALT; refuses a second mkfs from another node (W37); does not check how many members exist |

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
placement, incarnations, drain and rebuild). The full design; depends on the
unresolved D36 four-node metadata protocol. Post-alpha.

## 4. Recommendation

B for alpha 1.0, with C as the direction it grows into (the committed
`CLUSTER_SET` list is what the control-plane group will own later). If B is
judged too large for alpha, A is the fail-closed minimum and must land before
the alpha gate; it does not satisfy requirement 1 (two inputs by
convention) or 4.

Decisions the user needs to take for B: the file name and flag; whether the
daemon's own id comes from address matching (recommended) or stays
`--node-id` with a consistency check; whether absent-member refusal at
`raft-mkfs` is "quorum of each group" (recommended) or "every configured
member present".

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
