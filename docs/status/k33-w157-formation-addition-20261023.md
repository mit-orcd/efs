# K33/W157 — automatic formation and online node addition (Oct 23, 2026)

**Verdict: model PASS; code gate PASS with host limits (`make test-all` on
xorinox at `0098e06c4`: 433 commands, 0 FAIL, 13 NOTRUN host limits). Live
addition gates NOTRUN (the parent session's). Increment 1 of the contract;
branch `claude/vibrant-lalande-556adb`, merged into devel (`2d03d9837`, tags renumbered to 92 and 93), not pushed.**

Purpose: the code side of [K33](alpha-1.0-2plus1-checklist.md) /
[W157](work-queue.md), an alpha 1.0 blocker: the cluster forms by itself
once three members have joined, and nodes 4, 5, … join a live cluster
safely, against the contract's "Required invariants" and "Alpha acceptance
checklist" ([node addition](alpha-1.0-node-addition-20261023.md)). Builds
on [W153](w153-committed-membership-20261023.md). The protocol is
[k33-membership-protocol-20261023.md](k33-membership-protocol-20261023.md)
(rules M1–M9); the model is its executable form.

## What is on the branch

1. **Model** (`tests/d39_model.py` C16–C22, `tests/test_d39_model.py`,
   `make test-long` since the test split): an explicit-state model of formation, committed membership
   revisions, pending joiners, the SHARD2 {1,2,3} → {2,3,4} joint change
   with catch-up, storage-only members, store tokens, retained placement and
   restart after every durable step. Breadth-first over every interleaving
   of bounded scenarios, invariants on every state and transition, and
   every k-th reachable state driven fault-free to the converged membership.
2. **Protocol page** (above), including how W154 rebuild-on-return, reclaim,
   packing and client placement use the same membership authority.
3. **Implementation** (increment 1 of the contract; operator node ids):
   - Record v2 (`include/efs/membership.h`, `src/common/membership.c`):
     stable cluster identity (the committed MKFS salt), formation count,
     per-member store token; revisions extend by one member; prefix order.
   - Member store (`src/server/member_store.c`) v2: a 64-bit store token
     minted at initialization; learns later revisions only; refuses another
     cluster, a divergent record or a record naming another store at its id.
   - Apply rules (`src/meta/meta_apply.c`): MKFS keeps the first record,
     `EFS_MD_CMD_MEMBERS` (log tag **92**; 90 at the tested commit, renumbered at the merge because K32 took 90 and 91) installs the one-member extension
     on group 0, SALT installs any later revision on group 2; history is a
     success verdict, divergence `FENCED`; verdicts never halt a group.
   - `src/server/raft_host.c`: automatic formation by the group-0 leader
     (contiguous ids 1..k ≥ 3 with tokens, table quiet 1 s); a membership
     thread (KV learning, joiner additions, notice pump and group-2 copy on
     the group-0 leader, voter reconciler via `efs_raft_change`); boot
     configuration from the formation count; retired replicas (a node that
     is not a target voter never campaigns or serves the group);
     `EFS_MD_REQ_MEMBER_ADD` (request tag **93**; 91 at the tested commit) forwarding from members
     without a group-0 replica; a leaf-locked published view of the record
     for HELLO, `LIST_NODES`, peer addresses and the GC handoff;
     `raft-status` shows `retired`.
   - HELLO (`handler.c`, `cluster.c`, `efsd.c`): carries the store token and
     cluster; `EFS_HELLO_REJECT_PENDING` (= 4) for a node that is not a
     member yet (it keeps retrying); `_FORMED` refusal (exit 1) for an id or
     endpoint held by another store or another cluster; one store per id
     before formation; table entries carry tokens (`cluster_nodes.bin` v2).
   - `efs-mgmt raft-mkfs` is optional and idempotent: forms now and adds the
     joined and recently pending nodes in id order.
   - Test hook `EFS_TEST_MEMBER_EXIT=after-members-commit|before-learn`
     ends a process at that durable handoff (tests only).
4. **Tests** (the user's split: `make test` is CPU-bound units, daemons
   and the model run in `make test-long`): `tests/test_membership.c`
   (86 checks: record, revisions, the three apply rules on an in-memory KV,
   store token and revision learning; `make test`),
   `tests/test_k33_growth.py` (new, `make test-long`, real daemons and a
   FUSE client), `tests/test_w153_formation.py` (updated),
   `tests/test_gc_inode_fanout.py` (stub for the published view),
   `tests/test_placement.c` (initializers).

## Model results

Model commit `0ece226e0` (unchanged since; `python3 tests/test_d39_model.py -k growth`,
Python 3, on the Mac; the whole model file runs in `make test-long`, so the
gate below runs it again at the tested commit).

| Scenario (exhaustive) | States | Transitions | Settled | Result |
| --- | --- | --- | --- | --- |
| 1 node / 2 nodes, 2 crashes | 6 / 30 | 5 / 53 | all | never formed (unready) |
| formation, 3 nodes, 2 crashes | 23,830 | 110,656 | 7,943 | one record, converges |
| automatic-id formation (target M4′), 2 crashes | 2,201 | 6,225 | 733 | converges |
| add node 4, 2 crashes | 13,596 | 74,787 | 1,236 | {2,3,4} by joint change |
| two stores claim id 4, 1 crash | 27,046 | 195,089 | 3,863 | one commits, one refused |
| two automatic-id joiners | 35,137 | 301,976 | 5,019 | distinct ids 4, 5 |
| automatic-id joiner, 2 crashes, 1 wipe | 35,617 | 212,416 | 3,237 | converges |
| node 5 after 4, 1 crash / 4 and 5 together | 1,832 / 8,894 | 9,405 / 65,117 | all k-th | 5 hosts no group |
| add node 4, 1 crash, 1 store wipe | 4,707 | 25,705 | 672 | wiped store refused, member stays |
| add node 4 with writes | 1,477 | 8,814 | 211 | recorded placement kept |

Negative controls (each must fail with the named invariant):
HELLO-table authority → `voter_without_identity`; direct voter change →
`disjoint_quorums`; no catch-up → `uncaught_voter`; replica before
identity → `voter_without_identity`; no store token → `store_not_member`;
recomputed placement → `read_misplaced`; formation with two → 
`formation_below_three`; dropping an unreachable member →
`membership_shrank`. All 17 growth tests pass in about 29 s.

Model findings that changed the design: a committed member whose store is
replaced must stay a member and leave group 2 on {1,2,3} (removal is
deferred), so convergence accepts that state; the catch-up rule is a check
on the JOINT step, not a state invariant (a later disk loss is a fault, not
a protocol violation). The model treats a group's configuration as one
global value, so it could not show the retired-replica issue below; the
daemon test did.

## Daemon test findings fixed on the branch

- A voter removed by COLD may never receive that entry (Raft does not
  promise it). Node 1 kept believing it voted in group 2 and a handler
  waited on its stale replica. Fix: retired replicas (M6).
- The forwarder took a transport `BUSY` from an unreachable group-0 member
  as the verdict "id not next yet" and never tried the next member.
- A joiner refused from the retry thread exited 0; it now exits 1 as at the
  first HELLO.

## Gate

Host xorinox (16 CPU, 60 GB, Rocky 10), private tree `~/efs-k33/gate/src`,
`TMPDIR` inside it; tests bind free loopback ports (never 17xxx); the
user's `efs-fuse` of the intel cluster on xorinox was not touched.
`tests/test_k33_growth.py` topology: up to eight efsd processes on
127.0.0.1 (TCP, no RDMA), one store directory each, `--quota 1G
--writers 2`, buffered I/O (`direct_io=off`, the default on that
filesystem), `EFS_GC_DISABLE=1`, 2+1 coding; one or two `efs-fuse` clients
(fusermount3) on the same host; payloads 0 B to 3 MiB + 17 B before the
additions and 24 × 256 KiB after. Each test removes its daemons, mounts and
stores on exit.

```sh
git archive 0098e06c4 | ssh xorinox 'mkdir -p ~/efs-k33/gate/src && tar -x -C ~/efs-k33/gate/src'
ssh xorinox 'cd ~/efs-k33/gate/src && TMPDIR=~/efs-k33/gate/tmp EFS_GIT_ID=0098e06c4 \
  EFS_GIT_BRANCH=claude/vibrant-lalande-556adb EFS_GIT_DESCRIBE=0098e06c4 \
  EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh'
grep -cE '^make(\[[0-9]+\])?: .*Error' <runner log and tmp/*.log>
# the K33 commands again, to keep their output (the runner keeps failures only):
python3 tests/test_k33_growth.py; python3 tests/test_d39_model.py -k growth; ./tests/test_membership
```

Tested commit `0098e06c4` (base devel `cb970ca7c`; the model commit
`0ece226e0` is its parent). Build identity: `efsd 0098e06c4 (branch
claude/vibrant-lalande-556adb, built 2026-10-24T01:19:06Z on xorinox)`;
sha256 efsd `f4d57dea396cf52f0458ba16278eee086d6452b28c145e0d4a717cfcc327edc6`,
efs-mgmt `c810cbfca57ca5e4cd586d423e6bfe25ed3ea3b1e97eceaec6c2fcaea9cac1c7`,
efs-fuse `9d6bd3cd2cb7ecd7b2753a3874f5ededee0ce71ce713763eca388422c80675b0`.

Result: `TOTAL 433 FAIL 0 NOTRUN 13 STATUS INCOMPLETE (122s) BUILD 13s`, no
`make … Error` line. `tests/test_k33_growth.py` PASS in 51.7 s (FUSE
available; fragments of new files on added nodes 4, 6 and 7: 16, 24, 24),
`tests/test_d39_model.py` PASS in 50.6 s (the growth part: 17 passed, 8
negative controls, 13.8 s), `tests/test_w153_formation.py` PASS in 9.4 s,
`tests/test_membership` PASS (86 checks), `test_kv_kind_unique`,
`test_submit_unknown_tag`, `test_make_test_recipe`, `test_gc_inode_fanout`
PASS.

- NOTRUN (host limits, not this change): `test_cluster_stop.py` (no
  `~/git/cluster` on xorinox), `test_declaration_gate` RDMA path,
  `test_rdma_xprt` and the seven `test_rdma_segmented` modes (no RDMA
  device), `test_follower_bound` stalled-compactor case (needs
  `EFS_FAULTS=1`), `test_leak_gate.py` (no valgrind),
  `test_make_test_summary_runner.py` (its NOTRUN fixture).
- Before the gate, on the working tree, `tests/test_k33_growth.py` ran
  eleven times to completion (a twelfth was stopped mid-run): five failed
  (two on the retired-replica issue, one on the refused joiner's exit code
  0, two on the forwarder bug; all fixed, see above) and six passed, the
  four after the last fix in a row and the last on the committed sources;
  `tests/test_w153_formation.py` PASS; `make test` PASS. Of the failures
  only one forwarder capture is kept (`run2.txt`); the others were
  overwritten while iterating and are described here, not evidenced by a
  capture.

## NOTRUN (parent session)

- Live addition on real hosts: three fresh nodes forming with the minimal
  commands; add node 4 to a live three-node cluster, then nodes 5 and 6;
  usable added capacity under load; exact payloads including packed files
  across additions, concurrent I/O, orderly restart and each single-member
  outage; seed loss on real hosts.
- RDMA transport during addition; orcd-mini; ORCD/Intel/NUC drivers.
- Automatic discovery and explicit seed across subnets (not implemented).

## Remaining increments

Items 1 and 2 are built in increment 2
([evidence](k33-w157-autoid-discovery-20261024.md)).

1. Automatic ids and the positional CLI (`efsd ADDR:PORT STORAGE`,
   `efsd --join [SEED] STORAGE`) with the M4′ promise formation (modelled).
2. Discovery on the local subnet; explicit seed across subnets.
3. Live client view refresh: a mounted client uses new members only after
   remount (new mounts, the packer and server-side placement use them at
   once).
4. Packed-file payload verification across additions on live hosts (the
   daemon test covers unpacked files written before and after additions).

## Changes needed outside this repository

`~/git/devops`, `~/git/cluster` (not edited here):

- Stores from earlier builds are refused (identity v2, record v2,
  `cluster_nodes.bin` v2): wipe every store before the first start.
- The `raft-mkfs` step in `nuc-efs/start.sh`, `cluster.sh` and
  `orcd-test.py` is now optional: formation is automatic once ids 1..3
  joined; keep it (idempotent; it returns once every joined node is a
  member) or drop it. Their waits for every node in `efs-mgmt status`
  before `raft-mkfs` still hold.
- To grow a running cluster: start the new node with the next node id and
  `--join` any member; no other step.

## Raw captures

Kept outside Git on xorinox at `~/efs-artifacts/k33-20261023/` with
`SHA256SUMS`:

| File | sha256 |
| --- | --- |
| `summary.txt` | `20744157d9ef9780f72370c737e2105c46bbf3834d9cb63440a65edb4be85b24` |
| `timings.tsv` | `5afc2e5a152f313d96c1b90bdbdd7687b92541de45e94f3ea68ba8ebd317f403` |
| `runner.log` | `6287af74aad3b917a89c5439f344047420fcf7e089e04e310d3c41259b0c9332` |
| `build.log` | `3ec7d55e53db2ac28d5bf96b3126ebd61b8834873ec8ed162119d1323f9b731f` |
| `k33_growth.out` (exact commit) | `2aa3acf9d17cf0e4980e5a4dfc2f899680517c8cc8382662fee6b12ff2eb6858` |
| `d39_growth.out` (exact commit) | `24f48ed9b88085101c2669c3d3eec9c728d8ee83e895af4bf0c252ff39ad64d4` |
| `membership.out` (exact commit) | `cedc9332c709460db6471aef763244e3179d6c804c567399a22b452270a5e690` |
| `iterations/run1.txt` (PASS, before the forwarder change) | `e8fe284ee48ee169ecdb5872619a8ac9ea044d5d905e149be16456fa44ea0e42` |
| `iterations/run2.txt` (forwarder bug: node 7 stayed pending) | `6755b5c36f70418683d3545161e26be66bf387211d1390f1dbe056c6b6bf09cd` |
| `iterations/rep1.txt` … `rep4.txt` (PASS after the forwarder fix) | `3d6ece33…`, `1a1cfba2…`, `116fdb34…`, `b4ab8565…` |
| `iterations/rep5.txt` (PASS, final sources) | `792ae2a108f8b62cf8bec60e9ed4071a607addca2d1f5d5db22f5339d9919bcb` |
| `iterations/w153.txt` (PASS) | `58b847ed6279e3a39670dd48382349e21117a8691a790b3f209b59a68715edb8` |
| `iterations/fast.txt` (`make test` PASS, final sources) | `08a190b1dbd6546aed10b456359f65260db42a8284d0f6a3e4a190f0cb0b621e` |

Full hashes for every file are in `SHA256SUMS` there. The working trees
`~/efs-k33/src` and `~/efs-k33/gate` are deleted.

## After the merge

Merged-tree `make test-all` on intel2 (K32 + K33 at `2d03d9837`): 435 commands, 0 FAIL, 14 NOTRUN host limits (raw: intel2 `~/efs-artifacts/merge-k32-k33-20261024/`, `SHA256SUMS` `b5649ea9…8c1c`).
