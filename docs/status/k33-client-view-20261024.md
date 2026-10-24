# K33 — live client view refresh (Oct 24, 2026)

**Verdict: code gate PASS with host limits (`make -k test-all` on intel1
at `8b4cfbe77`: exit 0, no `make` error, 5 min 34 s). The live multi-host
addition under client load and RDMA are NOTRUN (the parent session's).
Branch `claude/devel-k33-client-view-0c8db4` on `merge-k32-k33`
(`8118d81d1`), not pushed.**

Purpose: close the K33 gap "live client view refresh" of
[K33](alpha-1.0-2plus1-checklist.md) / [W157](work-queue.md) and the
[protocol](k33-membership-protocol-20261023.md) (M7, "Client view").
Before this change a mounted FUSE client placed new data over the members
it saw at mount and used members added later only after a remount
([increment 1](k33-w157-formation-addition-20261023.md)).

## What changed

- **Server** (`src/server/handler.c`): wire message
  `EFS_MSG_MEMBERSHIP_READ` = **245**, reply `EFS_MSG_MEMBERSHIP_READ_REPLY`
  = **246** (`struct efs_msg_membership_reply`: rc, length, the record v2
  bytes). It returns the committed record this node holds, never the HELLO
  table; `EFS_ERR_UNFORMED` before formation. Served only on a declared
  connection, like every request. No new KV kind or Raft tag.
- **Judgement and install** (`include/efs/member_view.h`,
  `src/common/member_view.c`): a record is installed only when it is a
  valid later revision of the installed one: same cluster, same formation
  count, the installed members unchanged as its prefix. Verdicts: same,
  later, older, another cluster, divergent (another formation or a member
  replaced), invalid. Installing appends the new members to the node table
  in id order; rows already in the table are never written; a table row the
  record does not name, or names at another endpoint, is refused. The count
  is published last with release order, and the client's id lookup reads it
  with acquire order.
- **Client** (`src/client/node_cache.c`, `efs_fuse.c`):
  - The mount reads the committed record from a member before it serves and
    refuses to mount when its `LIST_NODES` table contradicts the record
    (members committed between the two reads are added).
  - In the serving process one thread asks a member, rotating, every
    second (`EFS_CLIENT_VIEW_POLL_MS` overrides, 50..60000 ms; tests use it)
    through the ordinary connection pool, so over RDMA as over TCP. It
    installs a later revision and logs `membership view: revision R -> R'
    from node N: added member M at host:port (K members, no remount)`.
    Another cluster, a divergent record and an undecodable reply are
    refused and logged once per (verdict, node) and then at most every 30 s;
    an older revision (a member that has not learned yet) is ignored.
  - A checkout or liveness query for a node id the table lacks (a chunk that
    another client placed on an added member) wakes the thread and waits
    for a round over every member, at most 2 s and never past the caller's
    RPC deadline, with a 1 s negative cache per id. Connections to added
    members then open on demand.
  - New writes place over the installed members (`efs_place_fragments` over
    the table); existing chunks keep their recorded fragment nodes.
  - Unmount stops the thread before the pool is torn down.

## Tests

- `tests/test_member_view.c` (`make test`, 348 checks, about 1.2 s):
  judgement of every verdict; reply parsing (server rc, short, oversized,
  trailing, another record version); table extension (append, old rows
  untouched, mismatched and unnamed rows refused, refused publish keeps the
  count; 300 rounds of two concurrent readers never see a published count
  before its rows); the real client runtime against five loopback fake
  members: mount refused on a contradicting record, mount at revision 1,
  another cluster refused, revision 2 installed, divergent refused, older
  ignored, a checkout of node 5 right after revision 3 commits waits and
  connects (under 2.5 s), a non-member id answered within the bound and
  then from the negative cache, several members asked.
- `tests/test_k33_client_view.py` (`make test-long`): real efsd daemons on
  127.0.0.1 with free ports (TCP), stores under `TMPDIR`, `--quota 2G
  --writers 2`, buffered I/O, `EFS_GC_DISABLE=1`, 2+1. Three nodes form;
  mount A through node 1 and mount B through node 3 (fusermount3). Five
  files 0 B to 3 MiB + 5 B are written, then a writer writes and fsyncs
  1 MiB + 3k B files through A for the whole run. Node 4 joins, both mounts
  install revision 2; eight 2 MiB files are written; node 5 joins, both
  install revision 3; eight more. Checks: the new files' chunk records
  (`efs-mgmt raft-getchunks`) name node 4 (and then node 5), nodes 4 and 5
  hold fragments, the old files' chunks still name only 1..3, every file
  reads back exactly through B (which reaches 4 and 5 on demand) and A, no
  write error, and no remount: same process ids, still mounted, one
  discovery and one mount-time record line each, no refused record.
- Updated: `tests/test_conn_checkout_budget.py` and
  `tests/test_resolver_deadline.py` compile the production
  `efs_client_conn_get` alone and now stub `node_index_or_await`. The first
  gate run (`68e2bbdef`) stopped on exactly this (`implicit declaration of
  node_index_or_await`); fixed in `8b4cfbe77`.

## Gate

Host intel1 (8 CPU, 15 GiB, Rocky Linux 10.2), user `maik`, private tree
`~/efs-k33c` from `git archive`, `TMPDIR=~/efs-k33c/tmp`; no running
cluster, mount, `efs` user process or 17xxx/27xxx port touched; tests use
free loopback ports.

```sh
ssh intel1 'mkdir -p ~/efs-k33c/tmp'
git archive 8b4cfbe77 | ssh intel1 'cd ~/efs-k33c && tar -xf -'
ssh intel1 'cd ~/efs-k33c && export TMPDIR=$HOME/efs-k33c/tmp EFS_TEST_JOBS=8 &&
  make -j8 all && time make -k test-all'   # in screen; output to ~/efs-artifacts/k33c-20261024/
# the change's own tests alone:
./tests/test_member_view; python3 tests/test_k33_client_view.py
# cleanup: ~/bin/edelete --force --delete ~/efs-k33c
```

Tested commit `8b4cfbe77` (code `68e2bbdef` plus the two test stubs).
Build identity: `efsd (no git metadata, branch unknown, built
2026-10-24T02:13:18Z on intel1)` (a `git archive` tree); sha256 efsd
`1016a861a9946108b6c827548ca11e4449f2f3f079f19ec9065d57b1cb1da3d0`,
efs-fuse `1cca340626f4234cbc7333c936672cca4a9d5e2efd764586c9ad42261905aa80`,
efs-mgmt `a0f72504055cfc48be9eade32ded5bd6cd70cf56a90fae1dd4ea8d0aeaebbe08`.

Result: exit 0, no `make … Error` line (429 test command lines).
`tests/test_k33_client_view.py` PASS in 20.5 s: 54 files exact through both
mounts (33 written under load across both additions); fragments on node 4:
345, node 5: 166; chunks of post-install files naming node 4: 24 of 32,
node 5: 19 of 32. `tests/test_member_view` PASS (348 checks).
`tests/test_k33_growth.py` PASS (75.1 s), `tests/test_w153_formation.py`
PASS (8.8 s), `test_kv_kind_unique` PASS (message types distinct with 245,
246). In an earlier working-tree run the client view test passed in 27.2 s;
both mounts installed revision 2 0.5–0.6 s and revision 3 0.8–0.9 s after
the commit logged by the proposer.

NOTRUN host limits (not this change): `test_cluster_stop.py` (no
`~/git/cluster`), the RDMA Memcheck gate and `test_leak_gate.py` (no
valgrind), `test_declaration_gate` RDMA path, `test_rdma_xprt` and the seven
`test_rdma_segmented` modes (no RDMA device), `test_stalled_compactor`
(needs `EFS_FAULTS=1`); `test_make_test_summary_runner.py` NOTRUN counts
are its own fixture. No FAIL, so no base comparison was needed.

Raw output (outside Git): intel1 `~/efs-artifacts/k33c-20261024/`:
`test-all-8b4cfbe77.log` (sha256 `e5e892f4…f21c1`), the stopped first run
`test-all-68e2bbdef.log` (`cdd5f5f8…4e29`), `binaries-8b4cfbe77.sha256`;
`SHA256SUMS` sha256 `615dab5e…d8ef5`.

## NOTRUN (parent session)

- Live addition on real hosts under client load: mounted clients on
  separate hosts writing while nodes 4 and 5 join, placement on the added
  nodes, exact payloads, no remount.
- RDMA: the view uses the ordinary pool, so the read and the peer
  connections to added members take the RDMA upgrade; not run (no RDMA
  device on intel1).
- orcd-mini.

## Limits

- The poll is the signal: a mounted client learns a revision within about
  one poll interval (or at once when it meets an unknown node id). Replies
  do not carry the revision.
- Removal is deferred (K33), so the view only grows.
