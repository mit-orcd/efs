# Review of the alpha 1.0 plan and of the code it relies on — 2026-10-22

**Verdict: the plan is sound in its ordering (known bugs, then function, then
performance) and its K register holds up against the code. It has one gap
that blocks its own scope: the code supports at most four nodes, so the
">4 physical nodes" column cannot be qualified without an implementation
item the plan does not contain. Four further corrections below.** Reviewed
on devel `165689c06`: [the checklist](alpha-1.0-2plus1-checklist.md), [the
roadmap](distributed-cluster-roadmap-20261022.md), the W145–W147 Raft
commits and the placement, voter and apply paths the gates depend on. No
code was changed and nothing was run on a cluster.

## Plan findings

### 1. More than four nodes is an implementation item, not a test column

`EFS_MAX_NODES` is 4 (`include/efs/common.h:46`, 139 uses in 23 files) and
the code is built around it:
- `EFS_MD_RAFT_N` must be 3..4 (`raft_host.c:11341`), and `group_voters`
  (`raft_host.c:731`) defines voter sets only for n ≤ 3 (all nodes) and
  n = 4 (shard group nodes 1–3, the other group nodes 2–4). A fifth node has
  no metadata role at all.
- `efs_place_fragments` clamps the ring to `EFS_MAX_NODES`
  (`placement.c:60`, `:80`): on a 5-node membership list the fifth node is
  silently never placed on.
- Client replies refuse a `leader_hint` ≥ `EFS_MAX_NODES`
  (`inode_rpc.c:1394`, `:1672`), so a client would reject hints for node 5.
- `EFS_RAFT_MAX_PEERS` is 7 (`raft.h:21`), so the Raft core could go
  further; the host, placement and client cannot.

A3 ("qualify at least one topology of minimum 5") therefore needs either a
Stage 2 implementation item (raise the cap, define voter sets for n ≥ 5,
placement and capacity tests at 5..max, HELLO/membership and client tables,
one fresh format) or a narrower alpha claim ("3 and 4 nodes"). This is the
user's call (step 1 of the fix order). Until it is made, the third ledger
column is not NOTRUN but UNSUPPORTED.

### 2. The four-node placement restrictions are already known from the code

B4 and F4 ask to measure and disclose them. They can be written down now:
- Metadata: each group is hosted by three of the four nodes (node 4 holds no
  shard-group state, node 1 no state of the other group).
- Packed segments live only on a group's voters (`packer.c:1015`): per group,
  one node of four never receives packed data. Small-file capacity is
  therefore three nodes' worth per group, while `efs_capacity_logical`
  (`placement.c`) models ordinary 2+1 placement over all four. A fill of
  mostly small files runs out of packed destinations before `df` says so
  (this is the W42/K20 protection-debt point, now with a concrete cause).
- Ordinary chunks do rotate over all four: `place_wide` uses stride 1 at
  n = 4 and 5, so each stripe takes three consecutive ring positions and the
  omitted node walks with the chunk index. B3's claim holds in the code.

Move the first two from "measure" to "disclose in F4 now, then measure the
size of the effect in B4/B5".

### 3. The ior write regression has no row

`d94ca85bf` writes at 710–863 MiB/s where `08cc16af4` wrote 1,329 MiB/s on
the same cluster, three runs, reads unchanged
([deploy report](orcd-mini-deploy-d94ca85bf-20261022.md)). The plan defers
performance to Stage 3, but this is a change between two consecutive builds
whose code delta is W143–W147 and the packing fixes: either a side effect
worth understanding before Stage 2 qualifies on this build, or cluster
state (the n2 repair and its 5.2 GB snapshot). Register it as **K27, VERIFY**
with the A/B/A′ already described in the deploy report (two restarts of
orcd-mini, needs the user's go-ahead). It is cheap and it settles whether
Stage 2 would be qualifying a slower write path.

### 4. Stage 1 VERIFY rows are Stage 2 runs

K6, K11, K12 and K17–K21 are "run the recorded fix's gate on separate
hosts". Those runs need the same 3-, 4- (and, after finding 1, maybe 5-)
host clusters and candidate build as Stage 2, and the rule "Stage 2 does
not start while a K row is open" would then wait on itself. Say instead:
VERIFY rows are the first pass of Stage 2 on the frozen candidate; a VERIFY
that fails becomes a FIX row and blocks as the rule intends. Step 11 of the
fix order already implies this; the stage rule should say it.

### 5. `EFS_MD_RAFT_N` is a hidden toggle in the sense of A8/A10

The daemon does not derive its member count: `start.sh` passes
`EFS_MD_RAFT_N=${#PORTS[@]}` (devops `start.sh:122`), and a private cluster
started without it refuses GC handoff. The value also selects the voter
sets, so a wrong N is a correctness error, not a tuning. Under A8 it should
be a derived fact (from the seed's membership or the format), and A10's
"stale launch configuration detected by validation" applies: a node whose N
disagrees with the cluster's must refuse to join, not start with different
voters.

### 6. The K recheck is accurate where I checked it

- K2: submit refuses only `EFS_MD_CMD_IS_RESERVED` (`raft_host.c:12075`);
  apply ends in `return EFS_ERR_UNSUPPORTED` for any other unknown tag, which
  halts the group. Fix at submit with the same allow-list the apply uses.
- K3: `EFS_MSG_NODE_LEFT` is handled on any connection with no authentication
  (`handler.c:856`) and nothing sends it. Delete the handler.
- K4: only three request types need the declaration (`handler.c:200`); the
  version check is `>= EFS_PROTOCOL_MIN_VERSION` (`:219`).
- K24: `EFS_KV_COMPACT_DIE` is read in every build (`kv_compact.c:272`).

Minor: `leader_exit_note` prints seven `match_index` slots, two more than
the daemon can have members; harmless, but the two constants should agree
when finding 1 is decided.

## Code review: W145–W147 and `raft-resync`

- **W145 (`76614e8e8`), the fix is right.** `snap_installed` now raises
  `commit_index` to at most `min(leader_commit, incl, last_i)` and never
  lowers it; the tail beyond the snapshot is proven by AppendEntries. The
  already-applied path (`last_applied >= incl`, `take_cfg` 0) gets the same
  cap, which is also correct there. `test_snapshot_commit_stops_at_snapshot`
  replays the orcd-mini sequence (proven prefix, older snapshot with a higher
  leader commit, replacement of the stale tail).
- **W147 (`a569de971`) is consistent with `snap_ensure`.** `snap_ensure`
  closes the open handle only after a successful `snap_open` of a different
  index, so after a NOT_FOUND at the stored snap point the newer open image
  is intact and `send_snap` can keep sending it while its index is still in
  the log. Note the handle is shared across peers with per-peer offsets, and
  a stale handle is closed only by the next successful open or compaction
  (`raft.c:2014`, `:2560`); that is a resource, not a correctness, point.
- **`raft-resync` (`613bb2c78`) does what it says, with one gap.** While the
  mark is set, a conflict at or below commit and a prev-beyond-log rejection
  both answer `match_index` 0, the leader falls back to InstallSnapshot, and
  the install clears the mark. The KV import is a full two-image diff: keys
  present only locally become `EFS_KV_DEL` (`kv_snap.c:602`), so the
  diverged applied entry's rows are removed, and the test checks the log
  point and state. The gap: `resync_requested` lives in memory only. A
  daemon restart between the request and the install drops it, and the
  follower wedges again, silently, until the operator re-issues the
  command. Persist it with the hard state, or at least say so in S3's
  `raft-resync` documentation. Side effects of the diverged apply outside
  the KV (an acknowledgement to a client) are not undone by any resync; that
  is K1 part 2.
- **W146 (`e75391268`)** was not read line by line here; the commit message
  and `read_whole`'s 64-bit lengths match the symptom.
- **Logging.** W145 adds an unconditional `fprintf` per vote and per
  election. With K1 part 3 open (hundreds of elections per restart) this is
  log volume on exactly the restarts under study; keep it, but rate-limit
  per group like the AE rejection notes.

## What I would change in the plan, in order

1. Decide the node range (finding 1). If >4 stays in alpha, add the
   implementation item to Stage 2 before A3, with a fresh format.
2. Add K27 (ior write A/B) and run it at the next authorized restart.
3. Write the four-node placement facts into F4 now (finding 2).
4. Reword the Stage 1/Stage 2 boundary for VERIFY rows (finding 4).
5. Add `EFS_MD_RAFT_N` to the A6 inventory as a derived fact, not an input
   (finding 5), and persist the resync mark or document its loss.

Nothing else in the plan conflicts with the code I read.
