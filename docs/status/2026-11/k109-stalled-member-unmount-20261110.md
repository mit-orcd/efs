# K109: a clean unmount past a stalled storage member (Task FO)

Task FO, Nov 10 2026. Purpose: fix the finding Task EZ's batch S soak recorded as
K109 (drafted on Task EZ's branch as K121; the coordinator rules at the merge): while
one of three members was alive and reachable but its apply was stalled, 6,832
clean client unmounts failed their session revoke after 15 s although the other
two members sealed. The task changes no K row and not the counts line.

**Verdict: PASS on xorinox (loopback, three private members).** Before the fix
20 of 20 clean unmounts with one stalled member failed after 15.2 to 15.3 s;
after it 20 of 20 exit 0 in 0.3 s (the revoke itself 35 to 53 ms). Two stalled
members still fail closed. **The multi-host check (Task EZ's S2 method with clean
unmounts during the stall) is still owed.** The K109 row stays the
coordinator's to rule.

## Source and build

* Base: devel `cff8eb424`. Branch `claude/sad-brahmagupta-5d44a8`, code commit
  `935c39e36` plus test commit `a8001b6f8` (the unit harness waits for its workers
  after TSan found a race in the harness; no product source differs). The live
  runs used the tree of `935c39e36`; the page and ledger commits that follow
  change no source.
  Binaries `efsd`, `tests/faults/efsd`, `efs-fuse`, `efs-mgmt`, `efs-bench` from
  `make all tests/faults/efsd` on the host; build id `unknown` (no `.git` there).
  The "before" runs used the unmodified `git archive cff8eb424` tree built the
  same way.
* Changes: `src/server/raft_host.c` (`host_storage_fence_members` and its
  helpers; `host_inode_rpc_peer` split into `host_inode_rpc_addr` plus the
  address lookup; `server_raft_host_storage_session_fence` answers BUSY, not
  INVAL, while the host is not running), `src/common/session_revoke.c`
  (comment), `tests/test_storage_fence_members.py`,
  `tests/test_readdir_reply_boundary.py` (extracts both halves of the split
  function), new `tests/live/k109_stalled_member_unmount.py` and
  `tests/test_k109_stalled_member_unmount.py`, `Makefile`, `tests/budgets.tsv`,
  `tests/sanitizer_check.sh` (both new tests join the gate).

## The fence path and its reply codes

Path: `efs_session_revoke` (`src/common/session_revoke.c`) sends BEGIN, then
`EFS_MD_SESS_STORAGE_FENCE` through `efs_session_storage_barrier`, which resends
for at most 15 s while the reply is BUSY or NOT_PRIMARY (any other code ends it),
then FENCE_LOC/ACK per touched shard, then FINISH (which repeats the fence in
`check_only` mode) and the cleanup. The group leader's `server_raft_host_submit`
reads the session authority, then calls `host_storage_fence_members`, which asks
every configured member with `EFS_MSG_STORAGE_SESSION_FENCE`; a member runs
`server_raft_host_storage_session_fence`.

**What "sealed" means.** The member's certificate
`meta/put-session/<uuid>` holds the floor (the new epoch), written, fsynced and
its parent directories synced (`storage_fence_seal`, `src/server/put_fence.c`).
From then on the member refuses a guarded PUT of an older epoch (STALE), and the
seal waits for guarded PUTs already in progress. In `check_only` mode (FINISH)
the member sealed nothing new: it proves the certificate already holds the floor
(a begin at floor-1 must be STALE).

**The rule kept.** A session is revoked only when a strict majority of the
configured members sealed it (K54, D46 item 6: `sealed * 2 > members`), the
membership is unchanged during the call, and the revoked epoch is committed in
the session shard log by BEGIN/FENCE_LOC/FINISH (the epoch-bound
`efs_session_finish_fence_epoch`, `src/meta/session.c`, rejects a session that has
moved on). The code and the K54 decision say strict majority, not every member;
the alpha guide's mount section, `operations.md` and release-note row L29 still
described the pre-K54 "every configured member" behaviour and are corrected.

| Reply | Source | Proves | Counts as |
| --- | --- | --- | --- |
| rc `0` | seal or proof done | sealed | **sealed** |
| exchange failed (no address, connect, send, recv; the receive bound is 30 s) | leader side | nothing | **unreached** |
| `NOT_PRIMARY` -15, `NO_QUORUM` -8, `NET` -6, `NOT_FOUND` -3 | the member's session read (`server_raft_host_submit`: ReadIndex or forward to a voter fails, or no record) | did not reach the seal | **not sealed** |
| `IO` -1 | certificate read, write, fsync or directory sync | may have written the file, not proven durable | **not sealed** |
| `BUSY` -13 | `check_only` and the certificate is below the floor; host not running; session shard busy | not sealed | **not sealed** |
| `STALE` -14 | the member's authoritative read shows another epoch than the floor, or a state other than ACTIVE or FENCING | the revoke is for a session that moved on | **refusal** |
| `INVAL` -5 | request invalid (floor below 2, `check_only` above 1, zero UUID, wrong payload length; no storage path configured) | the request is wrong | **refusal** |
| `PROTO` -7 | reply names another UUID or floor (leader-side check) | a protocol fault | **refusal** |

"Not sealed" and "unreached" never count toward the majority, so they cannot
weaken the rule; the only question is whether to fail the call. `NOT_FOUND` comes
after a successful read like `STALE` does, but it says the record is absent, not
that the epoch moved: the leader read the record a moment earlier, so it is a
member lagging a removal. Counting it as not sealed is the safe direction (a
majority is still needed), and below a majority it is returned. Unknown codes
default to "not sealed" for the same reason; only the three refusals fail early.

**A refusal is seen only if it arrives before a majority sealed.** The call
returns as soon as the majority is in, so a STALE that arrives after two seals is
not seen. That is acceptable: the client's next step, FINISH, is epoch-bound and
rejects a session that moved on, and sealing an older floor is harmless (a
certificate only moves up). The unit table makes the order deterministic (it
holds the sealing members back until the refusal is recorded); the product does
not promise it.

## Defect and fix

`host_storage_fence_members` walked the members in order and returned at the
first reply whose rc was not OK. A member that is alive but cannot read the
session shard (its apply is deferred; the read fails with NOT_PRIMARY after the
400 ms ReadIndex bound) therefore failed the call, and the client's barrier
retried the same fence for 15 s on NOT_PRIMARY or BUSY until the bound. The
`rc=-6` of the soak is the same failure: the 15 s bound expires inside a client
RPC and the submit returns NET (`ms=15003` to `15020` in every failing line).
A member that never answers (SIGSTOP) also held the serial loop for the 30 s
receive bound.

The fix (`src/server/raft_host.c`):

* every member is asked at once; a remote answer is awaited by a detached worker
  that owns a copy of the endpoint and shares only a refcounted round, so the
  call can return, and the host can stop, while a slow member's worker is still
  waiting (`host_inode_rpc_addr` touches no host state). If a thread cannot be
  created the member is asked inline, as before;
* the call returns when a majority sealed, when a refusal arrived, or when every
  member has answered; it never waits for the rest, and no new timeout, sleep
  or bound was added or changed (the 15 s barrier is untouched);
* below a majority it returns the first non-sealing reply code in member order,
  else BUSY: the error today's code returned for the same members;
* the success log line `sealed N of M members, unreached mask 0x..` is kept;
  its mask now means "not proven sealed when the majority was" (unreached, did not
  seal, or still answering);
* `server_raft_host_storage_session_fence` answers BUSY, with the UUID and floor
  echoed, when the host is not running. Before, it answered INVAL with a zero
  UUID, which the leader turned into PROTO: a restarting member would have been
  a refusal.

Callers: `host_storage_fence_members` has one caller (`server_raft_host_submit`,
STORAGE_FENCE and FINISH). `host_inode_rpc_peer` keeps its signature and its
other callers; `server_raft_host_storage_session_fence` is called by the daemon
handler (`EFS_MSG_STORAGE_SESSION_FENCE`) and by the fence itself for this node.

Safety for a member that is alive but stalled (K54's argument was written for a
member that is down): the epoch is committed in the session shard log without
that member, and a storage admission needs a fresh lane-leader STATUS proof; a
lagging storage member delegates the whole session and ticket read to the lane
leader instead of using its own frozen KV (`raft_host.c:15805`), so it cannot
admit a revoked epoch from stale state. **Not run:** a hand-built old-epoch PUT
against the stalled member after a revoke (K54's control, with stall instead of
kill). It is listed under "Not verified".

## Reproduction

Driver `tests/live/k109_stalled_member_unmount.py` (reproduction:
`--runs 20 --report-only`, one stalled member). Three private fault-build `efsd`
on 127.0.0.1 ports 18901 to 18903, group leaders on node 1 (`place_leaders`),
the client mounted on node 1. Node 3 starts with `EFS_FAULT_COMPACT_STALL=1`,
`EFS_FAULT_COMPACT_FILE`, `EFS_FAULT_L0_BYTES=1048576` and
`EFS_FAULT_MEMTABLE_BYTES=262144` (the K22 rehearsal caps; no hook was added).
With the marker file present, two `efs-bench --meta` loops fill its L0 until
`kv_l0_bytes >= kv_l0_cap` and `kv_apply_deferred > 0` (22 to 24 s), then stop.
Node 3 stays alive and reachable and answers the fence with `rc=-15`. Per run: mount, write
and fsync four files (64 KiB each), `scripts/client.sh stop`, read the client's
`session revoked at unmount` line. Raw output stays outside Git (below).

| Variant | Tree | Runs | Exit 0 | Wall per unmount | Revoke `ms=` | rc |
| --- | --- | ---: | ---: | --- | --- | --- |
| node 3 apply stalled | before (`cff8eb424`) | 20 | **0** | 15.2 to 15.3 s | 15002 to 15007 | -15 (20) |
| node 3 apply stalled | after (`935c39e36`) | 20 | **20** | 0.3 s | 35 to 53 | 0 (20) |
| node 3 `SIGSTOP`ped (never answers) | before | 3 | **0** | 15.2 to 15.3 s | 15010 to 15020 | -6 (3) |
| node 3 `SIGSTOP`ped | after | 5 | **5** | 0.3 s | 40 to 52 | 0 (5) |
| apply stalled, group 0 leader n1, group 2 leader n2, client on n2 | after | 5 | **5** | 0.3 to 0.5 s | 46 to 207 | 0 (5) |
| nodes 2 and 3 apply stalled (one of three sealed) | before | 1 | 0 | 15.3 s | 15003 | -6, `phase=storage-fence` |
| nodes 2 and 3 apply stalled | after | 1 | 0 (fails closed) | 15.3 s | 15005 | -6, `phase=storage-fence` |

Before, the leaders logged `storage-fence: member=3 floor=2 check=0 rc=-15` 340
times in the 20 runs (every 0.5 to 1 s per attempt). After, the leader still
logs the stalled member's answer (33 `check=0` and 22 `check=1` lines for
member 3 in the whole run, the rest from the two-stalled unmount) and the
unmount does not wait; the two-stalled unmount logs `reached 1 of 3 members, no
majority` 11 times.
The leaders-split run was added because the soak says "leaders"; the harness'
default puts both on node 1. The `SIGSTOP` variants stop node 3 after the
client's writes and `SIGCONT` it after the unmount, except the last run, where
node 1 is stopped cleanly with a worker still parked on the stopped node 3:
exit 0 in 30.1 s after the fix and 45.5 s before it (both include waits on the
stopped member elsewhere in shutdown; the 30 s is not attributed to the fence
worker, which no stop path joins).

## Tests

| Test | Result on devel `cff8eb424` | Result on the fix |
| --- | --- | --- |
| `tests/test_storage_fence_members.py` (fast, 33 rows by two modes: sealed, unreached, did-not-seal, refusal; 3, 4 and 5 members; five slow-member cases that must return without the slow answer; refusal while a slow member is pending; roster change, duplicate identity, short roster) | the nine did-not-seal rows with a majority sealed fail (`got -15 want 0`, `-13`, `-1`, `-3`); the adapter then hung (a deadlock of the gated refusal rows against the old serial code, not investigated), so the later rows were not run on devel | PASS; also under the TSan and ASan runs of the sanitizer gate (below) |
| `tests/test_k109_stalled_member_unmount.py` (live, `make test-long`, 65 s alone) | FAIL: 3 of 3 phase-1 unmounts fail after 15 s (`rc=-6` once, `-15` twice), the two-stalled case fails closed (the assertion reads `FAIL 1/4`) | PASS 4/4: three unmounts `ms` 40 to 45, one refused |
| `tests/test_k54_member_down_fence.py`, `tests/test_k54_member2_unmount.py` (unchanged) | not run here | PASS, 25.6 s and 9.4 s; the log assertion `reached 2 of 4 members, no majority` still holds |

Table-driven check and live test are in `tests/budgets.tsv` (`test`, 30 s ceiling,
and `test-long`, 120 s, slot 1: provisional, measured 65 s alone; the gate
measurement is owed). `tests/test_k54_member_down_fence.py` is unchanged; its
log assertion `reached 2 of 4 members, no majority` still holds because a failed
call waits for every answer before it logs.

## Verification on xorinox

All on the final tree (sources byte-identical to the commits; checked by sha256 of every
changed file), TMPDIR under `~/efs-taskFO/tmp`.

| Check | Result |
| --- | --- |
| `make test` | rc 0 (2 m 55 s) |
| `python3 scripts/tests_for.py common/session_revoke --run` | rc 0 (7 tests, 1 m 35 s) |
| `python3 scripts/tests_for.py server/raft_host --run` | rc 0 (145 tests, 10 m 28 s) |
| `bash tests/sanitizer_check.sh tsan` (tree built with `-fsanitize=thread`, no extra `-march`) | VERDICT FAIL, 33 run, 2 failed, 849 s. `k96_status_slots_live`: 51 TSan reports, the K107 failure, not this task's and not touched. `storage_fence_members`: a race in the harness of this task's new unit test (a late worker's log against the next row's reset), found by this run and fixed in `a8001b6f8`. Rerun of the two new entries on the same TSan tree with the fixed harness: `storage_fence_members` and `k109_stalled_member` PASS (63 s; the live test runs the fixed daemon under TSan with detached workers) |
| `bash tests/sanitizer_check.sh asan` (tree built by the script with `-march=x86-64-v3`) | VERDICT PASS, 59 run, 0 failed, 385 s. Both new entries were added to the ASan list after this run; a separate ASan run of just those two: PASS (the live test 101 s) |
| unit test repeated | 40 plain, 10 TSan and 5 ASan builds of `test_storage_fence_members.py` after the fix: 0 failures |
| `python3 scripts/check_doc_links.py`, `python3 docs/gen-architecture-full.py`, `make docs-check` | 0 dangling links; architecture pages regenerate with no tracked change; rc 0 |
| `python3 tests/test_code_graph.py` after `sh scripts/rebuild-code-graph.sh` | PASS (parser warnings as before this change) |

The 20-run "after" table row ran on a build made before the success log line was
restored and the comments reworded (no logic differs); the other "after" rows
and the live test ran on the final build.

## Not verified

* The multi-host check, Task EZ's S2 method with clean unmounts during the
  stall (production caps, real hosts, RDMA), is still owed. Everything here is
  loopback on one host with the K22 rehearsal caps.
* An old-epoch PUT against the stalled member after a successful revoke (the
  K54 control with a stall in place of a kill) was not run; the argument above
  rests on the code path.
* The `gc_reclamation` fixture `--storage-session-fence-fault` (a certificate
  fsync that fails on armed members) was not run. With a fault on every member
  nothing seals and the call fails as before; with the fault on fewer than a
  majority the revoke now succeeds with those members unsealed (K54's rule).
* Detached workers that outlive a stopped host: they hold no host state; the
  peer pool and the telemetry sink they use are process globals. A parked worker
  is bounded by the pool wait (5 s) plus the 30 s receive bound. Checked by the
  `SIGSTOP` stop above (exit 0) and by TSan on the unit table only.
* Gate timing of the new live test under 8-job contention.

## Reproduction and artifacts

```
git archive HEAD | ssh xorinox 'tar -x -C ~/efs-taskFO/src'      # then, there:
make -j8 all tests/faults/efsd
export TMPDIR=$HOME/efs-taskFO/tmp                               # work only under $HOME
python3 tests/live/k109_stalled_member_unmount.py --runs 20 --report-only --port-base 18900
python3 tests/live/k109_stalled_member_unmount.py --runs 5 --sigstop --port-base 18920
python3 tests/live/k109_stalled_member_unmount.py --runs 5 --split --client-node 2 --port-base 18940
python3 tests/test_k109_stalled_member_unmount.py
python3 tests/test_storage_fence_members.py
```

Expected: every line `clean=1` with `ms=` under 5000 for the one-stalled
variants; `FAIL`/`clean=0` at about 15 s on a tree before `935c39e36`.
Cleanup: the driver removes its work directory on PASS; remove `~/efs-taskFO`
with `~/bin/edelete --force --delete`.

Raw output (logs of every run above, the nodes' and clients' text logs of the
before and after runs, the devel unit adapter and its output, `make test` and the
sanitizer logs) is in `~maik/efs-artifacts/taskFO-20261110/` on xorinox,
`SHA256SUMS` beside it; sha256 of the `SHA256SUMS` file: `d842ed44e66d99994f630d63a292e0e9485a278316d5c23b2cb38a2b78ba6592`.
