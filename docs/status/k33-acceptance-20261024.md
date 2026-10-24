# K33 — node addition against the alpha acceptance checklist (Oct 24, 2026)

**Verdict: code gate PASS with host limits (`make test-all` on xorinox at `2ebd4fd85`: 446 commands, 0 FAIL, 13 NOTRUN host limits, 555 s). Four acceptance items of the
[contract](alpha-1.0-node-addition-20261023.md#alpha-acceptance-checklist)
PASS as single-host loopback daemon tests (interrupted additions, exact
payloads, seed loss, growth bookkeeping); three defects found and fixed.
Live multi-host, RDMA and orcd-mini gates NOTRUN (the parent session's).
Branch `claude/devel-k33-acceptance-75e95e` on `merge-wave2`
(`3e01ba95c`), not pushed.**

Purpose: qualify [K33](alpha-1.0-2plus1-checklist.md) /
[W157](work-queue.md) node addition against the remaining items of the
"Alpha acceptance checklist" as deterministic daemon tests, and fix what
they find. Protocol: [M1–M9](k33-membership-protocol-20261023.md).
Earlier evidence: [increment 1](k33-w157-formation-addition-20261023.md),
[increment 2](k33-w157-autoid-discovery-20261024.md),
[client view](k33-client-view-20261024.md).

## Acceptance items → tests → results

| Checklist item | Test (`make test-long`) | Result on xorinox |
| --- | --- | --- |
| Interrupt additions at every durable handoff; restart/retry and concurrent joins converge to one membership without duplicate identity or orphaned data | `tests/test_k33_interrupt.py` (new), `tests/test_k33_growth.py` (existing) | PASS, 67.7 s standalone; in every gate run |
| Exact existing and new payloads, including packed files, across additions, concurrent I/O, orderly restart and each single-member outage | `tests/test_k33_payload.py` (new) | PASS in gate 3 (after fix 2; it FAILED the orderly restart in gate 1). Standalone r1: 2,822 files exact (56 small, packed; 2,766 large), writer 2,742 files, 1 retried write, 295 s |
| Loss of the original seed does not block operation or joining through another member; unreachable members remain members | `tests/test_k33_seed_loss.py` (new) | PASS, 16 files exact, 221.9 s alone; gate 3 PASS |
| Quota/headroom defaults, readiness, protection reporting, reclaim and W154 repair stay correct as capacity and membership grow | `tests/test_k33_bookkeeping.py` (new) | PASS after fix 1 (FAIL before: no reclaim), 41.5 s alone, 43.3 s in gate 3 |

Items not covered here: formation and discovery (increments 1–2), live
3→4→5+ on real hosts, and the driver/launch-script conversion (parent).

### What each test does

- **Interrupted additions** (`test_k33_interrupt.py`, three clusters).
  Packing off and GC off so the data check is exact. After every
  interruption the ended daemon restarts (or the join is retried) and the
  test requires: every member reports the same table of ids 1..N with
  distinct endpoints; each daemon holds the id its endpoint has there;
  voters {1,2,3} (group 0) and {2,3,4} (group 2 from four); files written
  before read back exactly; the fragment files on all stores are exactly
  three per chunk written (no orphan on any store, none on a joiner).
  - A: group-2 leader ends with node 4 a learner (`learner-added`); node 5
    ends after persisting the record naming it (joiner `after-learn`); node
    4 ends after persisting revision 4 while node 6 joins (member
    `after-learn`); two joins at once, both interrupted: node 7 through node
    2, which ends right after committing (`after-members-commit`), and node 8
    forwarded by storage-only node 5, ending when its record arrives
    (`before-learn`). Result: ids 7 and 8, one membership of 8.
  - B: group-2 leader ends with JOINT appended, not committed
    (`joint-started`): the next leader finishes JOINT and COLD.
  - C: group-0 leader ends after the revision is on group 2, before noting
    the copy (`after-g2-copy`): the next leader copies again (history =
    success).
  The existing growth test covers `after-members-commit` at 3→4,
  `before-learn` at 4→5, concurrent joins 6–8 with the seed down and a
  wiped store refused.
- **Payloads** (`test_k33_payload.py`): GC on, packer minimum age 500 ms;
  small batches (1 B … 16 KiB) wait until the packers' `packed_total`
  grows by the batch; a writer thread writes large files (64 KiB + 1 …
  1 MiB + 3) and reads each back for the whole run. 3 → 4 → 5 under the
  writer; SIGTERM of all five and a simultaneous restart (same ids, n=5);
  then each member 1…5 in turn stopped while a batch and large files are
  written and everything is read, restarted, read again; a fresh mount
  through storage-only node 5 reads every file.
- **Seed loss** (`test_k33_seed_loss.py`): seed stopped; mount through
  node 2 keeps reading and writing; node 4 joins through node 3 (group 2
  moves {1,2,3} → {2,3,4} with node 1 away), node 5 through node 4, node 6
  through storage-only node 5; node 1 is listed DOWN, never dropped; node 5
  frozen (SIGSTOP) and node 6 stopped: six members everywhere; node 5
  resumes, node 6 restarts, new files written; the seed restarts and learns
  revisions 2–4 without an operator step; a mount through node 1 reads all.
- **Bookkeeping** (`test_k33_bookkeeping.py`): at 3, 4, 5, 6 members,
  through every member: `Cluster state: OK (N up, 0 down)`, `state:
  active`, `Degraded chunks: 0 (group 0: 0, group 2: 0)`, `Abandoned
  uploads: pending=0`, and `Usable bytes capacity` equal to a Python twin
  of `efs_capacity_logical` (2 GiB, 2.67 GiB, 3.33 GiB, then 5 GiB with
  node 6 on the automatic quota, 553 GiB); the mounted client's `df`
  follows without a remount. At six: 24 × 300 KiB written (35–37 fragment
  files on every node, the added ones included) and deleted: every store
  returns to its fragment count before. W154: node 6 stopped, 12 × 1 MiB
  written at once: 8 degraded acknowledgements, 8 marks counted by status
  while it is away; after its return 8 rebuilt, count 0, all files exact.

## Defects found and fixed

1. **Reclaim stalled after growth to four members** (`9fd93627a`). The
   group-0 leader's GC reap pass forwards group-2 lane sweeps to the
   group-2 leader and then waited for its own group-2 replica to apply
   them (`host_bg_propose` → `host_wait_applied`). On node 1 that replica is
   retired from four members and may never receive the entry: every pass
   waited the full apply budget per lane (`gc-pass ms≈9750
   reap_sweep≈9680 reap_done=0`, `apply-sleep … group=2 applied=30`), and
   deleted files' fragments were never removed on any node. Fix: a retired
   replica takes the leader's verdict like a node without a replica
   (`host_group_retired`, `82b6e3127`). Regression: the reclaim step of
   `test_k33_bookkeeping.py` (FAIL before: fragments unchanged after 90 s;
   PASS after) and a unit case in `tests/test_gc_verdict.py` (forwarded
   verdict, no local wait).
2. **A whole-cluster orderly restart took over a minute** (`82b6e3127`). A
   formed member with persisted peers said HELLO to each of them
   (`server_rejoin_cluster`) before it accepted connections; in a
   simultaneous restart each peer was doing the same, so every HELLO waited
   30 s for an answer. Node 1 was not serving 60 s after the restart
   (gate run 1, `test_k33_payload.py`: `Cannot connect to peer …:50949`
   after 30 s). The member restarts from its committed record; the HELLO
   only refreshes peers' tables, so it now runs on the rejoin thread until
   one peer answers (`rejoin_announce`). Regression: the orderly-restart
   step of `test_k33_payload.py` (60 s bound; passed in every run after).
3. **Test-side races in the existing client-view test** (`82b6e3127`):
   a joiner logs either `formed: N members` or `up raft_id=… n=N formed`,
   depending on whether it learned before or after its Raft host started;
   `test_k33_client_view.py` waited for only the first (gate run 1 FAIL).

Test-design corrections (not product defects): `efs-mgmt status` exits
non-zero while a member is down (the helpers parse the table anyway); node
1's retired group-2 replica may keep the old voter view (M6; the checks
skip it); with three of six members unreachable a new stripe can have two
fragments on them, beyond the one failure 2+1 keeps for writes, so the
seed-loss test writes only with the seed alone away (`no quorum` there is
correct).

## Test hooks

`EFS_TEST_MEMBER_EXIT=<point>[,<point>…]` ends efsd with status 86 at a
durable handoff (`server_test_member_exit`, `src/server/cluster.c`); with
`EFS_TEST_MEMBER_EXIT_ONCE=<prefix>` only the first process that creates
`<prefix>.<point>` ends, so the hook can be given to every member and hits
whichever node leads that step. Points: `after-members-commit`,
`before-learn`, `after-learn` (new; joiner and member), `learner-added`,
`joint-started`, `after-g2-copy` (new). Unset in every deployment. No new
KV kind, Raft tag or wire message type.

## Gate

Host xorinox (16 CPU, Rocky Linux 10, gcc 14.3.1, x86_64, `-O3
-march=native`), private trees under `~/efs-k33acc/` from `git archive`,
`TMPDIR` inside the tree; daemons on 127.0.0.1 with ports the tests
choose (never 17xxx), discovery off, TCP, buffered I/O, `--quota 1G
--writers 2` (node 6 of the bookkeeping test: automatic quota), stores on
`/home` (XFS on LVM). The intel cluster's mount and the `efs` user were not
touched.

```sh
git archive 2ebd4fd85 | ssh xorinox 'mkdir -p ~/efs-k33acc/gate/src && tar -x -C ~/efs-k33acc/gate/src'
ssh xorinox 'cd ~/efs-k33acc/gate/src && TMPDIR=~/efs-k33acc/gate/tmp EFS_GIT_ID=2ebd4fd85 \
  EFS_GIT_BRANCH=claude/devel-k33-acceptance-75e95e EFS_GIT_DESCRIBE=2ebd4fd85 \
  EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh'
# one test alone (keeps its work tree with EFS_K33_KEEP=1; delete it after):
TMPDIR=$PWD/tmp python3 tests/test_k33_interrupt.py   # likewise _payload, _seed_loss, _bookkeeping
```

| Run | Commit | Result |
| --- | --- | --- |
| standalone r1 | `2014a9207` | payload PASS; bookkeeping FAIL (reclaim, defect 1); interrupt and seed loss FAIL (test checks) |
| standalone r2 | `9fd93627a` | interrupt PASS; bookkeeping, seed loss FAIL (test checks) |
| standalone r3 | `f095fc081` tests | bookkeeping PASS, seed loss PASS |
| gate 1 | `f095fc081` | 446 commands, 3 FAIL (`test_gc_verdict` harness, `test_k33_client_view` race, `test_k33_payload` restart = defect 2), 13 NOTRUN |
| gate 2 | `82b6e3127` | 446 commands, 2 FAIL, 13 NOTRUN: `test_k32_entry_count` part B counted 65 admissions for 64 (8 real threads per cohort under 8-way gate load; K32 client ticket slice, not K33) and `test_k33_growth` (formation saw two of three nodes; the third daemon's log was not in the message). Each passed 3/3 when repeated in parallel on the same tree |
| gate 3 | `2ebd4fd85` | **446 commands, 0 FAIL, 13 NOTRUN (host limits), 555 s** (`STATUS INCOMPLETE`, exit 3, the runner's code for NOTRUN only). K33 tests: payload 494.8 s, seed loss 226.1 s, growth 108.3 s, bookkeeping 43.3 s, interrupt 39.0 s, autoid 32.1 s, client view 23.6 s |

`2ebd4fd85` adds only a diagnostic to `tests/k33_daemons.py` over
`82b6e3127` (a timed-out wait reports every other daemon).
Build identity: `efsd 2ebd4fd85 (branch claude/devel-k33-acceptance-75e95e, built 2026-10-24T05:39:03Z on xorinox)`; sha256 efsd `e1ac70b90f9b2678755d09dea490c9e96bc836de42d55eee459c6235ffdc5b67`, efs-mgmt `b18fd85970095292300285de8fbc678c37e72157415352c6f175e9434e8e5a58`, efs-fuse `64df7ea566ee8ab7b7d15786f964b22d750b7d17a8d229b18cd9c5605434a867`.

The 13 NOTRUN are host limits, the same as increments 1–2: no RDMA device
(9: `test_declaration_gate` frame path, `test_rdma_xprt`, 7 ×
`test_rdma_segmented`), `~/git/cluster` absent (`test_cluster_stop`),
valgrind absent (`test_leak_gate`), the `EFS_FAULTS` build case
(`test_follower_bound`) and the runner's own NOTRUN fixture
(`test_make_test_summary_runner`). The payload test is now the longest
command of the gate (495 s under 8-way load, 295 s alone).

## Raw evidence

xorinox `~/efs-artifacts/k33-acceptance-20261024/` (`SHA256SUMS`
sha256 `0bd134f3871a53a67acae41897def8b9fa409a9eda50df916a9e6dcb5ef06c41`): `runs/` (standalone outputs r1–r3 with the failing tracebacks
and log tails), `r3-daemon-logs/` (daemon and mount logs of the passing r3
runs), `gate1-*` and `gate2-*` (summaries, failure logs, the repeats),
`gate3/` (summary, timings, binary hashes). Runtime stores were not kept; work trees `~/efs-k33acc` deleted
after the run.

## NOTRUN (parent session)

- Live: additions 3 → 4 → 5+ on real hosts under client load; the four
  items above across hosts (seed host loss, member outages, packed payloads,
  orderly restart of a real cluster, W154 on a returning host).
- RDMA during addition; orcd-mini; driver and launch-script conversion
  (checklist item 8). `tests/live/w154_rebuild_on_return.py` still starts
  daemons with the removed `--node-id/--addr/--port` CLI.

## Findings left open

- `test_k32_entry_count.py` part B is scheduling-sensitive (exact admission
  count from 8 threads; one extra entry under load). K32 owner.
- The client logs `refused or did not answer the protocol 5 declaration
  (rc=-6): incompatible server` for a member that is merely unreachable
  (frozen); the wording points at a version problem.
- A write needs its three placement nodes reachable minus one; with two
  unreachable members a new stripe placed on both fails (`no quorum`)
  instead of being placed on reachable members. Within 2+1's one-failure
  scope; noted for the parent.
