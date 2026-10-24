# K35 · fsync with a member down, only with proof (Oct 24, 2026)

**Purpose.** Fix finding [F22](s-docs-alpha-1.0-20261024.md) (checklist
item K35 in [alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md))
under the conditions of [D42](decisions.md) (F22 row): `fsync` and
`fdatasync` succeed with one member down, but only with proof of
durability for every stripe the file's unsynced writes touched; never by
ignoring failed COMMIT replies.

**Verdict: PASS (scoped, loopback regression evidence).** Branch
`claude/devel-k35-fsync-b63807` on devel `55ad94072`, not merged, not
pushed. Code commits `817a71b1f` and `36ac1b7a6`; `make -k test-all` on
xorinox at `36ac1b7a6`: **exit 0, 0 FAIL** (452 test commands, 14 NOTRUN
lines, all host limits: no RDMA device, no valgrind, no `~/git/cluster`,
no `EFS_FAULTS=1` build of one compactor test). Live multi-host and RDMA verification is **NOTRUN**
(parent session). Single-host loopback daemons are regression evidence,
not qualification.

## Contract as implemented

`efs_fuse_fsync_ino` → `efs_client_data_commit(ino)`
(`src/client/data_commit.c`), after the existing flush and REPORT of the
file (durable metadata naming each object, its placement and, for a stripe
acknowledged on two fragments, the W154 mark in the publication's batch):

1. **Records.** Every PUT object the client stores is recorded
   (`efs_client_stripe_note`, from `efs_client_put_fragments_parallel`)
   with, per fragment, the node, the node's acknowledgement sequence and the
   PUT verifier (boot epoch) of the incarnation that acknowledged it; a
   fragment never acknowledged is `MISSING` (published degraded).
2. **Targeted COMMIT.** fsync COMMITs, in parallel, only the nodes that hold
   the file's unproven fragments, not every known storage node; an
   unrelated down node is never asked. A fragment is **durable** when a
   COMMIT sent after its acknowledgement succeeds with the same verifier;
   with another verifier it is **lost** (incarnation mismatch: never resent,
   never forgotten). A refusal (failed barrier, uncertain recovery, F23/K36)
   or no reply proves nothing.
3. **Proof per stripe.** Three durable, or two durable and the third
   `MISSING`: proven. Two durable and the third unproven or lost: the client
   proposes **`EFS_MD_CMD_DEGRADED_MARK`** on the lane shard's group and
   returns only after its verdict: OK (mark written from the chunk row's
   placement and checksums, or already present), or NOT_FOUND (the object is
   no longer the published version: rewritten, truncated, packed, unlinked).
   Fewer than two durable fragments, or a mark that cannot be written: `EIO`,
   and the client log names the stripe:
   `fsync ino=… chunk=… gen=… nodes=a,b,c fragments=DPM: 1 of 3 durable;
   need 2 (rc=-1)` (D durable, P unproven, L lost, M published missing,
   K marked now). The record stays, so a later fsync proves it (the member
   returned with the same verifier) or fails again. A proven stripe leaves
   the table.
4. **Bounded.** The table holds at most 2^18 records (about 100 B each);
   a full table is swept (every record proven, marks included) before a new
   record, and a table that stays full refuses the PUT (EBUSY) rather than
   forget one.
5. **Unresolved publication** stays an error of the flush/REPORT step that
   runs before the proof (unchanged), so fsync never reaches the proof with
   it. Stripes with a fragment on a recovery-uncertain store are marked
   degraded (the store keeps refusing COMMIT until K36) and fail EIO when
   that fragment is needed.

**Placement on four or more members.** A member that failed a PUT hard
while the other two acknowledged gets its fragment resent once, same
ticket, on a fresh connection (a stale pooled connection after a restart
was the common cause seen); if that fails too and an available member is
left, the object is given up (D31 collects it) and placed again with that
slot on the available member (a 10 s placement "away" mark, before the
four failures that mark it down); only an acknowledgement that stands on
two fragments is recorded and logged as degraded (W154). The client's background reprobe of a
down peer now also starts from `efs_client_node_is_down` (placement asks
there and never checked out a connection), so a returned member takes
fragments within about a second instead of after the 30 s cooldown; before,
three-member writes stayed degraded for that time.

**Visibility.** `efs-mgmt status` prints
`Protection: full (2+1: every chunk tolerates one lost node)` or
`Protection: DEGRADED: N chunks on two of three fragments, no fault
tolerance until rebuilt (W154 rebuilds when the missing node is up)` (or
`unknown` until both group leaders report), and `Cluster state: DEGRADED`
also while every node is up but degraded chunks remain. The status exit
code is unchanged (nonzero only with a member down). The server logs each
mark (`raft-host: degraded mark ino=… ci=… gen=… fragment=… node=…:
COMMIT did not prove it durable; rebuild on return`); the W154 rebuild
pass picks it up like a published mark.

**Numbers.** New Raft command tag **249** `EFS_MD_CMD_DEGRADED_MARK`
(34 B: `[tag][ino:8][inode_gen:8][object_gen:8][ci:4][fragment:1][node:4]`,
from the high end of the free range below 250; announced in the shared
notes). No new KV kind (marks are kind 51 `DEGRADED`), no new wire message
type (the request rides `EFS_MSG_RAFT_MKFS`). New fault hook point
`data-commit` for `EFS_FAULT_MEMBER_EXIT` (EFS_FAULTS builds only). No
format change for existing kinds; per AGENTS.md no compatibility path:
an older daemon refuses tag 249 (W140 gate) and the client's fsync then
fails `EIO` for a stripe that needs the mark.

## Tests

| Contract item | Deterministic (`make test`) | Real daemons (`make test-long`) |
|---|---|---|
| Member down before the write | `tests/test_data_commit.c`: acknowledged on two, published missing, only those two COMMITted, no mark | `tests/test_k35_fsync_degraded.py` step 4 (three members) |
| Member down after the write, before fsync | `test_data_commit.c`: mark proposed for exactly that fragment, then success | step 3: fsync and fdatasync OK, 9 stripes marked, `Protection: DEGRADED` |
| Member dies during COMMIT | `test_data_commit.c`: request sent, no reply, marked | step 9: node 5 (of 5) exits at the `data-commit` hook (status 86), fsync OK, 19 stripes marked |
| Unrelated member down | `test_data_commit.c`: node 4 never asked, no mark; per-file scope (another file's down nodes do not matter) | step 7: four members, node 4 down, fsync OK, count stays 0 (4 re-placements logged) |
| Two members down | `test_data_commit.c`: EIO, no mark, EIO again, success once they return with the same verifier; one at write + one before fsync | step 5: fsync and fdatasync of an open file EIO, stripe logged `1 of 3 durable; need 2`, OK after both return |
| Incarnation mismatch / uncertain recovery | `test_data_commit.c`: one such member → marked; two holding needed fragments → EIO for good | not run (an unclean stop is K36's) |
| Mark not written / object superseded | `test_data_commit.c`: mark refused → EIO, later written; NOT_FOUND → retired | step 2 (packed objects answer NOT_FOUND) |
| Packed small files | `test_data_commit.c`: eight one-chunk files, each marked | step 2: four small files packed with all up, then fsync with node 3 down |
| 4 and 5 members: available nodes | `tests/test_k35_placement.py`: real placement + reroute at 3/4/5 members, every member down in turn, 4,096 stripes each: never the down member on 4 and 5, every available member a share; away mark expires; `tests/test_put_retry_budget.py`: resend once, then placed again on a spare, or degraded without one | steps 7 and 8 (node 2 of 5 down), count stays 0 |
| W154 rebuild clears the mark | `tests/test_meta_apply.c` `test_degraded_mark_after_publish`: mark from the row (base and span), replay, second fragment refused, superseded → NOT_FOUND, rebuilt → cleared | step 6: count back to 0, `Protection: full`, every rebuild logged, files exact |
| Concurrency, bounds | `test_data_commit.c`: concurrent fsyncs share a held COMMIT; an acknowledgement after the COMMIT was sent is not covered; full-table sweep; unprovable table refuses the PUT | — |
| PUT records | `tests/test_put_reply_deadline.py`: sequence and verifier per acknowledged slot; a later attempt sends only the missing slot | every step |
| Added member down at six | — | `tests/test_k33_bookkeeping.py`: node 6 of 6 down, 12 files written and fsynced, 0 degraded acknowledgements and marks (5 objects placed again), counters 0 from a new mount |

## Gate

- **Tree:** `git archive 36ac1b7a6` under `~/k35/gate` on xorinox (16 CPUs),
  `make -j16 EFS_GIT_ID=36ac1b7a6 all`, then
  `make -k EFS_GIT_ID=36ac1b7a6 test-all` in `screen`, `TMPDIR=~/k35/gtmp`
  (both trees deleted after the run).
- **Environment:** xorinox, Rocky Linux kernel 6.12.0-211.67.1.el10_2
  x86_64, 16 CPUs, gcc 14.3.1, Python 3.12.15, FUSE (`/dev/fuse`,
  `fusermount3`), TCP only, buffered I/O (daemon default), loopback
  daemons on free ports, 1 GiB quotas in the daemon tests.
- **Result:** `EXIT 0` (16:40–17:05 UTC, 24 min). `test_k35_fsync_degraded.py`
  PASS (58.9 s, 39 files exact); `test_k33_bookkeeping.py` PASS (37.1 s).
  The first run at `817a71b1f` failed only `test_k33_bookkeeping.py`: it
  expected degraded acknowledgements at six members with node 6 down,
  which D42 now avoids (writes went to the available members, 0 marks).
  The test was changed to assert that, and the run exposed a misleading
  log (an object placed again was first announced as a degraded
  acknowledgement), fixed in `36ac1b7a6`. That run's logs are kept under
  `run1-817a71b1f/`.
- **Raw output** (outside Git): xorinox `~/efs-artifacts/k35-20261024/`
  (`env.txt`, `build.log`, `test-all.log`, `run1-817a71b1f/`; `SHA256SUMS`
  sha256 `06836fd2…6a1e`).
- **Docs gate:** `cd docs && python3 gen-architecture-full.py`, then
  `python3 docs/check-architecture.py`: all 5 checks pass.

Reproduce (on a Linux host with FUSE, from this commit):

```bash
make -j16 all && make test-all
```

```bash
python3 tests/test_k35_fsync_degraded.py
```

## NOTRUN and limits

- **Live multi-host verification: NOTRUN** (parent session): fsync with a
  member down on a real cluster (three members, and four or more), the
  degraded count and `Protection:` line in `status`, the rebuild on return
  across hosts, and over RDMA.
- **Unclean stop (F23):** stripes with a fragment on a recovery-uncertain
  store are marked degraded and never rebuilt there until K36 lands; a
  stripe that needs two such fragments fails EIO for good. Not exercised by
  a daemon test here.
- **Lost records stay:** a stripe whose fragments were lost on two
  re-incarnated members is EIO for that file for the life of the mount
  (sticky by design); its record is not retired.
- Scope is this client's writes to the file: peer clients' data is theirs
  to fsync (it was covered before only because every node was COMMITted).
- Three members cannot place around a down member: writes there are
  acknowledged degraded (W154) and marked; the protection debt is visible
  and repaid by the rebuild on return.
