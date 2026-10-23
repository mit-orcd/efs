# W153 — the member count and voters come from the committed membership (Oct 23, 2026)

**Code gate: `make test-all` on amd PASS with host limits (430 commands, 0 `make … Error` lines; 9 RDMA FAILs that fail identically on the untouched base, 5 NOTRUN). Live formation, restart and
single-loss failover gates NOTRUN (the parent session's). Merged into devel
`4026e254d`; merged-tree `make test-all` on intel2 0 FAIL.**

Purpose: close the code side of [W153](work-queue.md) (alpha 1.0 blocker).
Each daemon took the metadata member count from `EFS_MD_RAFT_N` with a silent
default of 4, so a three-node cluster started without it put an absent node 4
into SHARD2 (no fault tolerance left in that group) and silently stopped the
all-member GC handoff; `atoi` accepted malformed values. User decisions of
Oct 23 ([node addition](alpha-1.0-node-addition-20261023.md),
[decisions](decisions.md)): no cluster file, no `--cluster`, no configured
member count; seed/`--join` stays the startup; online node addition is a
separate item (K33/W157).

## What changed

Tested commit `17c9b5df62cde2417ff898368fd89a6585277901` (base `a23d45540`).

- **Formation commits the membership.** `efs-mgmt raft-mkfs` (the formation
  step that exists today) commits, inside MKFS on group 0 and SALT on group 2,
  the members that have joined at that moment: node ids, endpoints and the
  count (`include/efs/membership.h`, `src/common/membership.c`). Node ids must
  be exactly 1..n with n ≥ 3 and distinct endpoints; otherwise formation is
  refused before anything is proposed, naming who joined and who is missing
  (`raft-mkfs refused: … joined: node 1 at …, node 2 at …; missing: node 3`),
  in efsd and in an `efs-mgmt` preflight. The record lives on each group's
  anchor shard (`EFS_KV_KIND_MEMBERSHIP`). A repeated mkfs keeps the first
  record and salt (the W37 rule) and repeats the steps below, so it also
  finishes an interrupted formation.
- **Every member records it.** After MKFS commits, the mkfs node reads the
  committed record back and sends it to every member (`EFS_MD_REQ_FORMED`,
  host-only, never logged). Each member checks that it is the member at its
  own endpoint, persists the record in its member store (fsync) and only then
  publishes it; then SALT carries the same record to group 2. A member that
  does not answer is named and mkfs fails; a retry repeats the notice.
- **Count and voters come from the record, on start and restart.** Before
  formation the host runs group 0 only, on nodes 1..3 (all formation needs;
  the D38 rule puts group 0 there for every n ≥ 3); group 2 has no voters and
  is attached when the record arrives. From then on `n` and both groups'
  voters (`efs_md_group_voters`) are derived from the record, never from the
  environment or the HELLO table. On restart they come from the store; a KV
  record that disagrees with the store stops efsd, and a store that applied
  MKFS/SALT but stopped before recording the notice learns it from its own KV.
  The node-id range check and the all-member GC handoff
  (`host_gc_inode_nodes_batch`) use the committed membership; `LIST_NODES`
  (client data placement), the join adopt and the node table keep committed
  members only.
- **`EFS_MD_RAFT_N` is removed.** A set variable, even empty, stops efsd
  before it listens: `EFS_MD_RAFT_N was removed (W153): …`.
- **Before formation no client metadata is served.** Inode, xattr,
  publication, writer-view, lane-bootstrap, query-snapshot and retained-create
  messages are refused by closing the connection with a bounded log line
  (`the cluster is not formed; … run efs-mgmt raft-mkfs once`); submits answer
  the new `EFS_ERR_UNFORMED` except the group-0 MKFS and the notice.
- **A node that joins after formation is refused.** HELLO from an id that is
  not committed, or from a member at another endpoint, gets
  `EFS_HELLO_REJECT_FORMED`; the joiner exits with "the cluster is already
  formed with N committed members … node addition (K33/W157), which this
  build does not implement". The HELLO table can no longer change the count.
- **Member store** (from `a0cf68bc0`, adapted): `src/server/member_store.c`
  under `<meta root>/mdraft`: exclusive `owner.lock` (a second process on the
  store stops "owned by another running process"); `boot.bin`, a checksummed
  durable boot counter advanced to `max(fresh, saved+1)` before anything is
  served, now the Raft boot id (replaces the raw 8-byte file); `identity.bin`
  with the node id, the state (unformed/formed) and the committed record.
  Another node id, an interrupted initialization, a corrupt identity and a
  store holding Raft/KV data without an identity (any pre-W153 store) are
  refused. No configuration file anywhere. Fresh format only (AGENTS.md).

## Tests

| Test | Group | Covers |
| --- | --- | --- |
| `tests/test_membership.c` (53 checks) | `make test` | formation input: 2 members refused naming node 3, 3 and 5 committed, gap, duplicate id, shared endpoint, port 0 refused; record round trip; malformed records (other version, count < 3 or > 16, truncated, trailing byte, id out of order, port 0, unprintable address, shared endpoint) refused; voters from the count (3: 0x7/0x7, 5: 0x7/0xe); store lock (second owner in another process), boot counter (fresh, saved+1, restart, corrupt, short, other version, exhausted, missing; refusals leave the file), learn once / same again / other FENCED, restart derives count and voters, corrupt/short identity, malformed record under a valid checksum, other node id, interrupted init, pre-W153 store |
| `tests/test_w153_formation.py` (~6 s) | `make test-long` | real efsd on loopback: `EFS_MD_RAFT_N=3` and empty stop efsd; 2 joined → mkfs refused naming node 3, metadata refused; 3rd joins → committed, g0/g2 voters 0x7 hosted on all three; node 4 joining afterwards exits (K33 message), seed logs the refusal, table stays 3; repeat mkfs OK; node 2 restart derives n=3 and voters from its store; second efsd on node 3's metadata store refused by the owner lock; a five-node cluster: g0 0x7 on 1–3, g2 0xe on 2–4, node 5 hosts neither |
| `tests/test_gc_inode_fanout.py` | `make test` | updated: the handoff targets the committed members; unformed refuses with a full HELLO table |
| `tests/test_node_left_refused.py` | `make test` | updated: the embedded handler prefix includes the unformed refusal |

## Gate

Host amd (32 CPU, Ubuntu, soft-RoCE `rxe0` only), private tree
`~/efs-w153/src` from `git archive 17c9b5df6`, `TMPDIR` inside it, no ports in
17xxx (tests pick free loopback ports; no daemon of the user's cluster touched).

```sh
git archive 17c9b5df6 | ssh amd 'mkdir -p ~/efs-w153/src && tar -x -C ~/efs-w153/src'
ssh amd 'cd ~/efs-w153/src && export EFS_GIT_ID=17c9b5df62cd EFS_GIT_BRANCH=devel-w153 \
  EFS_GIT_DESCRIBE=17c9b5df62cd && EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh'
grep -cE '^make(\[[0-9]+\])?: .*Error' <runner log and tmp/*.log>
```

Build identity: `efsd 17c9b5df62cd (branch devel-w153, built 2026-10-23T22:30:22Z on amd-395)`; sha256 efsd `c5dff3ec2dc20f39c35ee892c8e8534f1a5d0680f9b22409f20d618bad4dd898`, efs-mgmt `9a41f146d020f1c1253ec84f0e3db88455e9bc407afb9ab08c3e5fe34ae78cf8`.

Result: `TOTAL 430 FAIL 9 NOTRUN 5` in 226 s (build 11 s), no
`make … Error` line in the runner log or any command log.
`tests/test_membership` PASS (53 checks), `tests/test_w153_formation.py` PASS
in 5.8 s.

- FAIL (host limit, not this change): `./tests/test_declaration_gate` (its
  RDMA path), `./tests/test_rdma_xprt` and the seven
  `./tests/test_rdma_segmented` modes. amd has only the soft-RoCE `rxe0`; the
  same nine commands built from base `a23d45540` on amd fail the same way
  (each exit 1), as they did for the D39 run on `757d76844`.
- NOTRUN (host limits): `test_cluster_stop.py` (no `~/git/cluster`),
  `test_follower_bound` stalled-compactor case (needs `EFS_FAULTS=1`),
  `test_leak_gate.py` (no valgrind), `test_store_durability` async case
  (TMPDIR not XFS), `test_make_test_summary_runner.py` (its NOTRUN fixture).
- Before the fixes in this commit, the same run surfaced three broken
  source-extraction tests (`test_node_left_refused`, `test_query_page_barriers`,
  `test_gc_inode_fanout`); all pass at the tested commit.

## Limitations and follow-ups

- Live gates NOTRUN here: fresh three- and four-node formation with the
  minimal commands on real hosts, restart of every member, and single-member
  loss per group (the parent's). The real-daemon test above is loopback only.
- Formation is still the explicit `raft-mkfs`; automatic formation, discovery
  and online node addition are K33/W157. Nothing here adds a node; the
  committed record and per-member store are the authority an addition
  protocol would extend (an ordered membership revision in place of the
  single record).
- A member that missed the notice (down during mkfs) serves no metadata and,
  if it votes in group 2, leaves that group short until a retried mkfs
  reaches it; there is no automatic catch-up.
- The storage-session fence, the segment and fragment GC fan-outs and the
  W154 rebuild still enumerate the HELLO table. After formation that table
  holds committed members only, but a member missing from it (lost
  `cluster_nodes.bin`) would be skipped rather than refused; moving them to
  the committed membership is a follow-up.
- `efs-mgmt raft-status` does not show the formed state or count; the daemon
  logs them (`raft-host: up … formed|unformed`, `raft-host: formed: N members …`).
- Drivers with four or five daemons now wait until every member has joined
  before `raft-mkfs` (formation commits whoever has joined). The ORCD, Intel
  and NUC drivers were edited but not run.
- Historical status pages, results summaries and the archive keep their
  `EFS_MD_RAFT_N` wording as records of past runs.

## Changes needed outside this repository (made Oct 23)

Committed, not pushed: devops `c05eb59` (`nuc-efs/start.sh`), cluster
`667ad2c` (`cluster.sh`, `orcd-test.py`, `portal/portal.py`; `cluster.sh`
waits up to 300 s for every node in `efs-mgmt status` before `raft-mkfs`).
Merged into devel as `4026e254d`; merged-tree `make test-all` on intel2:
432 commands, 0 FAIL, 14 NOTRUN (no RDMA device, no cluster scripts, no
valgrind headers). The shared `tests/live/ticket_put.py` harness now drops
`EFS_MD_RAFT_N` and waits for every member before `raft-mkfs`.

The list as reviewed:

`~/git/devops`:

- `nuc-efs/start.sh` line 122: drop `EFS_MD_RAFT_N=${#PORTS[@]}` from the
  efsd launch (efsd refuses it) and update the comment above it. Its
  readiness loop already waits until every port reports `up` before
  `raft-mkfs`; keep that (formation commits the members joined then).
- Stores from earlier builds are refused (`holds Raft/KV data without a member
  identity`): wipe every store and its `.mkfs-done` marker before the first
  start with this build.

`~/git/cluster`:

- `cluster.sh` line 122: drop `-E EFS_MD_RAFT_N=${#NODES[@]}`; update the
  header comment (line 5, "--node-id in 1..EFS_MD_RAFT_N" → node ids 1..N,
  the count is committed at formation). Before the `raft-mkfs` step (line
  ~145), wait until `efs-mgmt status $seed_ip:$EFS_PORT` lists
  `Cluster nodes (${#NODES[@]})`: the start loop waits only for `listening`,
  which precedes the join, so mkfs could commit fewer members.
- `orcd-test.py` line 163: drop `EFS_MD_RAFT_N=4` (its readiness loop already
  waits for four `up` nodes before mkfs).
- `portal/portal.py` lines 529 and 1187: remove `EFS_MD_RAFT_N` from the
  displayed server settings.
- Same store wipe as above.

## Raw captures

Kept outside Git on amd at `~/efs-artifacts/w153-20261023/` (the work tree
`~/efs-w153` is deleted):

| File | sha256 |
| --- | --- |
| `summary.txt` | `755dfc31448df414fd0de771c19b0c4d8071c1f6fff2f6b2f2ee0cdb8896a032` |
| `timings.tsv` | `6e877d89927021ad5e6cbc59094c1f76379d3deab7fea38fafa4454681cc5495` |
| `runner.log` | `ca2dfee38d47411ff908e3e83b204cff8a1812f8305ad58a3bc94efcbaf91be2` |
| `fail-logs.tgz` (the nine failure logs) | `6aa3725b1aec7d7569d22526a2cd72e7d66d13e90bad9e8e0ccdc2d857930fc0` |
