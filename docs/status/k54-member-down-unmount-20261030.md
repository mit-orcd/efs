# K54: clean unmount with one storage member down

Purpose: fix K54 / finding U4 (D46 item 6, blocks alpha: yes). A clean client stop
with one member down must complete accepted flush, settle and a safe durable
revocation, and a failed revoke must never become a clean shutdown.

**Verdict: PASS on intel1 and node9901 (TCP and RDMA) for a clean unmount with any one member (2, 3 or 4) down, plus the retained base failure; `make test-all` on the final tree 569 / 0 FAIL. Real-host stage on orcd-mini: NOTRUN (no allowance reached this session). `test_k37_segment_rebuild` failed in two earlier gates and is unattributed (see Gate).**

## Source and build

* Base: devel `7514b2145`. Branch `claude/devel-bd-k54-505e84`; code commits
  `8bd4029f8` (fence), `c953d5254` (learn on orphan delete), `2369e0d8b` (client routing); final head `a53bd111e`. Tested trees are
  `git archive` of the branch head, built with `make all tests/faults/efsd`
  (build id `unknown`, no `.git` on the hosts).
* Changes: `src/server/raft_host.c` (`host_storage_fence_members`,
  `server_raft_host_storage_session_learn`), `src/server/put_upload.c` (seal on a
  FENCED verdict), `src/server/put_orphan.c` (learn before the floor check),
  `src/server/server_internal.h`, `src/client/inode_rpc.c` (`session_revoke_submit`).

## Base failure (retained)

intel1, 4 members (loopback, ports 18710+), client on node 1, four fsynced files,
member 3 stopped, `scripts/client.sh stop`:

```
efs: session revoked at unmount: epoch=1 touched=0 rc=-13 ms=15001 fence_ms=0 cleanup_ms=0 cleaned=0 probe_failed=0 phase=storage-fence   (SIGTERM, exit 1, 15.3 s)
efs: session revoked at unmount: epoch=1 touched=0 rc=-13 ms=15001 ... phase=storage-fence                                                (SIGKILL, exit 1, 15.3 s)
```

Daemon log, every 50-800 ms for 15 s: `storage-fence: member=3 floor=2 check=0 transport unavailable`.
Reproduce: `python3 tests/live/k54_member_down_unmount.py --runs 1 --mode stop|kill`
on an unfixed build. Raw logs: intel1 `~/efs-artifacts/k54-member-down-20261030/base/`
(`SHA256SUMS` beside it; sha256 of the SUMS file `813d2982…a64bc`).

## Design: shape (a), the sealed epoch is committed in the session shard log

Before: `host_storage_fence_members` returned BUSY the moment one configured
member's transport failed, in both modes (seal, and the `check_only` proof FINISH
repeats).

Now: a member that cannot be reached is counted as unreached and skipped; every
member that answers still seals (or proves) its certificate exactly as before; the
call is OK only if answering members are a strict majority of the configured
members (`reached*2 > count`: 3 of 4, 3 of 5, 2 of 3) and the membership is
unchanged. Otherwise BUSY, so the client reports `rc=-13 phase=storage-fence`. The
log names the unreached members (`sealed 3 of 4 members, unreached mask 0x4`, or
`reached 2 of 4 members, no majority`). `EFS_SESSION_BITMAP` fencing of touched
shards (FENCE_LOC/ACK), FINISH and the K42 certified cleanup are unchanged.

Invariant argument (a member that missed the seal never accepts or settles under
the revoked epoch):

1. The revoked epoch is committed in the session shard log before the unmount
   returns OK: BEGIN (FENCING, epoch E+1), FENCE_LOC per touched shard (raises the
   durable `reject_below` floor on the shard's replicas) and the epoch-bound
   FINISH. None of these depends on the storage member being up.
2. Settle is a metadata operation: `PUT_TICKET_SETTLE` and publication check
   `efs_session_accept` on the committed floor at the lane leader, so a returned
   member cannot settle the old epoch whatever its local state.
3. Storage admission: every PUT upload on a member first obtains a fresh
   ReadIndex `PUT_TICKET_STATUS` proof from the lane leader (`put_upload.c`),
   which fails with FENCED/STALE for a revoked epoch. The returning member
   therefore reads the sealed epoch from the log before it admits anything for
   that uuid. New in this change: on a FENCED verdict the member also seals
   `epoch+1` in its own certificate, so later admissions stop locally.
4. Reclaiming bodies is gated by the member's own certificate
   (`server_put_orphan_delete`: floor above the ticket's epoch, "the seal waits
   for every guarded PUT in progress"). A member that never received the seal
   used to answer BUSY forever. New in this change: it asks the session shard
   (`server_raft_host_storage_session_learn`), and only when the committed epoch
   is above the ticket's does it seal it and continue; otherwise BUSY. The D31
   collection already retries a member that refuses "without end" and never
   completes a record from absence or a quorum, so it is the resolver that
   brings the returned member to the sealed epoch before any body is deleted.

Residual window, stated plainly: a member that is alive but unreachable from the
leader, and has an upload in flight that passed STATUS before BEGIN committed,
can still store that fragment after the revoke returned. It cannot be settled
(2), it is an unsettled ticket for the D31 orphan ledger, and (4) keeps its
collection blocked until that member holds the certificate (the seal waits for
the guarded PUT). There is no timeout-as-success and no "assume the member is
dead": below a storage majority the revoke stays BUSY with its phase.

Shape (b) (a durable "unsealed members" record plus a leader resolver) was not
built: (1)-(4) already give the same guarantee with the existing log and the
existing collection retry, with no new log record.

## Negative controls

`tests/test_k54_member_down_fence.py` (`make test-long`, row in
`tests/budgets.tsv`, 50 s, slot 1; about 27 s alone on intel1). Four formed
daemons, production client ledger, hand-built PUT frames, revoke by
`efs-mgmt raft-session fence`:

| Control | What it checks | Base |
|---|---|---|
| control | PUT accepted before the revoke, STALE on every member after | passes |
| (i) | member SIGKILLed, revoke exits 0; returned member answers an orphan-delete frame without BUSY and seals the epoch, refuses the old-epoch PUT (STALE) | fails: revoke `phase=storage-fence` |
| (ii) | member stopped cleanly, revoke exits 0; replay at once, before any resync wait, is STALE, certificate sealed, nothing stored | not reached |
| (iii) | members 1 and 4 down (2 of 4): revoke fails closed at `storage-fence` and the log says `reached 2 of 4 members, no majority` | not reached; base fails closed too |
| (iv) | one of them back (3 of 4): the same `raft-session fence` resumes and exits 0; both back: STALE | not reached |

(v) `tests/test_k54_member2_unmount.py` (new, `make test-long`, 60 s slot 1, 10 s alone): two production unmounts (`scripts/client.sh stop`) with member 2 down, SIGKILL then clean stop, exit 0 and `rc=0`, files exact after it returns. On the tree with the fence fix but without the client routing it fails (`phase=begin`/`cleanup`); on the untouched base it fails at `phase=storage-fence`. `tests/test_revoke_redirect.py` now also checks that a transport failure and a hint naming a down member move on to another voter.

Honest limits: (iii) and (iv) are regression guards; the base also refuses with
two members down and `raft-session fence` already resumed. Only (i)/(ii) fail on
the base, and the test stops at (i) there. `tests/test_put_orphan_server.c` now
mocks the learn and checks a committed epoch not above the ticket's stays BUSY.
Check of the harness against the unfixed binaries (basetree): fails at (i) with
`phase=storage-fence`.

## Repeats (final build)

Driver `tests/live/k54_member_down_unmount.py` (reproduction: `--runs N --mode
alternate --victim 0`, victim rotating 2, 3, 4; signal alternating SIGKILL and
SIGTERM, so each member gets both): per run mount, write and fsync four files,
stop one member, `scripts/client.sh stop`, require exit 0 and `rc=0`, restart the
member, wait for `Cluster state: OK (4 up, 0 down)` with `Protection: full` and no
degraded segments, remount and compare every file byte for byte.

| Host | Transport | Runs | Clean | Read-back bad |
|---|---|---|---|---|
| intel1 | TCP | 30 (10 per down member) | 30 | 0 |
| node9901 | TCP | 12 (4 per down member) | 12 | 0 |
| node9901 | RDMA (`EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0`) | 12 (4 per down member) | 12 | 0 |

Unmount 0.5-2.3 s. The RDMA loop used the same environment as the earlier RDMA loop
whose daemon log showed "RDMA transport up"; this loop's logs were not kept (a clean
loop deletes its tree), so the transport is by configuration. Earlier builds: member
3 only, 40/40 SIGKILL intel1, 10 TCP + 10 RDMA node9901 (before the client routing);
19/20 once (`phase=begin rc=-6`), which led to the routing change. Raw output:
`~/efs-artifacts/k54-member-down-20261030/` on intel1 (SHA256SUMS sha `339da4c9ff7e0937…`)
and node9901 (`5f2500643b3d9e8f…`).

## Environment and limitations

* intel1 (8 CPU, Rocky 10) and node9901 (private libfuse
  `/data1/efs/deps/libfuse`, clusters under `/tmp`). Loopback 4 members, quota
  1 GiB, buffered I/O, default configuration, files of 64 KiB-80 KiB.
* orcd-mini real-host stage (node9900 over RDMA, stop node9901 daemon cleanly,
  unmount, restart, read back): NOTRUN, no explicit user OK.
* The gate result is below.

## Gate

`make test-all` through `scripts/make_test_summary.sh`, `EFS_TEST_JOBS=8`, intel1,
`git archive` of the commit; 11 NOTRUN are host limits (no cluster scripts, no
valgrind headers, no usable IB device).

| Tree | Result |
|---|---|
| `c953d5254` | 568 / FAIL 2 / NOTRUN 11 (883 s): `test_storage_fence_members` (old offline-member expectation) and my e2e test (picked member 2); both fixed |
| `c74788bf7` | 568 / FAIL 1 (923 s): my e2e test's orphan-delete assertion too strict; fixed |
| `cdcf16b72` | 568 / FAIL 1 (761 s): `test_k37_segment_rebuild` |
| `e76d07fed` | 568 / FAIL 1 (764 s): `test_k37_segment_rebuild` |
| **base devel `7514b2145`** | **567 / FAIL 0 / NOTRUN 11 (1033 s)**, same host and command |
| `2369e0d8b` | 569 / FAIL 1 (727 s): `test_revoke_redirect` (mock asserted the old no-resend rule); test updated |
| **`a53bd111e` (final)** | **569 / FAIL 0 / NOTRUN 11 (939 s), OVERBUDGET 4** (`test_k33_payload`, `test_b_geometry`, `test_b6_unavailable`, `test_pack_dup_candidate`; budget overruns, not failures) |

`test_k37_segment_rebuild` (fails at its step-7 wait, after steps 1-6 PASS) failed in
the two gates on `cdcf16b72` and `e76d07fed` and once alone on `e76d07fed`; it passed
in the base gate, in both later gates, 3 times alone before, and 2/2 as concurrent
copies on both the base and the final tree. It involves no revoke or unmount with a
member down, and the later code only changed client routing, so I believe it is a
load-dependent flake, but it failed on my tree and not on the base in gate runs
(base 1 of 1 passed, mine 2 of 4), so it is unattributed, not cleared. If it recurs
on the merged tree it is a K row candidate.

Raw gate output and timings stay on intel1 (`~/efs-bd/*/`, copies of summary and
timings in `~/efs-artifacts/k54-member-down-20261030/gate-*`).
