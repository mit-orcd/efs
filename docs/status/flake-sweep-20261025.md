# Flake sweep: every intermittent failure of the gate (Oct 25)

**Verdict: FIXED; final gate NOTRUN (stopped at the parent session's request after the branch was merged; it is now gated on the merge branch).** The gate (`make test-all` through
`scripts/make_test_summary.sh`, `EFS_TEST_JOBS=8`, and `make test-long`
serially) was run on intel1 against `merge-wave7` at `0b01a2a1e`: ten pool
runs of 471 commands and three serial runs of 62 commands. Six commands
failed at least once, and the final-gate attempts surfaced one more. Each is
explained below with its rate, cause, class and fix: three product defects
(client, packer, server forwarding), five test defects, one test made
deterministic. Two single occurrences stay open, not reproduced with logs
(K36 case 8, K33 payload read-back). The runner's own self-test was also
miscounted as NOTRUN on every run (deterministic noise, fixed).

- **Purpose:** Stage 2 qualification cannot start on a gate that fails at
  random. Find every intermittent failure in the gate, reproduce it alone
  and under the 8-job pool, classify it (product defect, test defect,
  environment) and fix it as its class demands.
- **Base:** `merge-wave7` = `0b01a2a1e` (devel plus B6/K37 per segment).
- **Tested source:** `0b01a2a1e` for the sweep and the reproduction;
  the fixes on branch `claude/devel-flake-sweep-1505b6`: `fc79fe086`
  (product, tests, drivers) and `36f2e3874` (the two tests that compile
  slices of `write.c`, the A6 settings inventory). Copied to intel1 with
  `git archive` plus the changed files (no `.git`; build id `unknown`). A
  first final-gate run at `fc79fe086` failed three commands
  deterministically, all consequences of the change set
  (`test_env_inventory.py`, `test_k35_placement.py`,
  `test_put_retry_budget.py`); `36f2e3874` fixes them and the final gate
  was restarted from scratch. That second gate failed
  `test_b6_unavailable.py` once in eight pool runs (a test race between
  two log reads, below); `e69eb4404` fixes it and the final gate was
  restarted again from scratch. The third gate failed
  `test_report_stepdown.py` in its first pool run (5.99 s write);
  `c11ef971e` instruments it and `19e02ba06` fixes the cause (below). A
  pool run beside the loaded step-down loops then failed eleven tests that
  compile or match slices of `raft_host.c`, deterministic consequences of
  how `19e02ba06` restructured it; `8f1d50ed8` keeps only the fix and
  drops its temporary traces. The final gate ran from scratch at
  `8f1d50ed8`.
- **Host:** intel1, 8 CPUs, Rocky Linux 10, TCP only (no RDMA device), no
  valgrind, no `~/git/cluster`; private trees `~/efs-flake` (base) and
  `~/efs-flake2` (fixes). Only loopback ports the tests choose and stores
  under the tree's `tmp/` were used; the user's intel cluster (ports
  17432–17435, `/data1/efs`) was not touched.
- **Raw:** intel1 `~/efs-artifacts/flake-sweep-20261025/` (per-run
  `summary.txt`, `timings.tsv`, failure logs, kept daemon and client logs of
  every reproduced failure, the host copies of the drivers; 616 files,
  555 MB; `SHA256SUMS` sha256 `8fc83340…5d04`). Not committed. The private
  trees `~/efs-flake`, `~/efs-flake2` and `~/efs-flake3` are deleted.
- **Merge note:** at the merge (merge-wave8b) `EFS_FAULT_AWAY_MARK_MS` was
  renamed `EFS_FAULT_AWAY_MARK_MS` (A12: a test override is an
  `EFS_FAULT_*` hook); this page keeps the name the tested commits used.

## Baseline: 10 pool runs, 3 serial runs

| Command | Pool (8 jobs, 10 runs) | Serial `test-long` (3 runs) | Alone (5) | Beside a pool run (3) |
|---|---|---|---|---|
| `tests/test_k37_segment_rebuild.py` case 7 "second loss" | 2/10 | 1/3 | 0/5 | 0/3 |
| `tests/test_k35_fsync_degraded.py` case 7 "four members, node 4 down" | 2/10 | 1/3 | 1/5 | 1/3 |
| `tests/test_b_geometry.py` automatic quota lower bound | 2/10 | 0/3 | — | 1/1 (the pool run beside the loops) |
| `tests/test_d4_progress.py` "packer errors: put_errors=1" | 1/10 | 0/3 | 0/5 | 1/3 |
| `tests/test_report_stepdown.py` "write max 5.68 s > 3.0 s" | 1/10 | 0/3 | 0/5 | 0/3 |
| `tests/test_k36_recovery.py` case 11 (and once case 8) | 1/10 | 0/3 | 1/5 (case 8) + 0/5 | 0/3 |
| `tests/test_query_first_catalog.py` NOTRUN "race did not occur" | 8/10 NOTRUN | 3/3 NOTRUN | | |
| `tests/test_b6_unavailable.py` refuse "3 files packed, no segment written degraded" | 0/10 (1/8 in the first final gate) | 0/3 | 1/8 | — |

Pool runs took 494–799 s each (build 22 s); serial `test-long` runs 2488,
2739 and 2578 s. 14–15 NOTRUN per pool run are host limits (RDMA, valgrind,
cluster scripts, the `EFS_FAULTS` follower-bound case) plus the two
mentioned above.

## Causes, classes, fixes

| Case | Cause | Class | Fix |
|---|---|---|---|
| K35 case 7 | The client's 10 s "away" mark (`PUT_AWAY_MARK_MS`, set on node 3 by the settle write at 09:50:19.58 while node 3 restarted in case 5) was still live when node 4 stopped at 09:50:24.68. Spares are chosen among members neither down nor away: with 3 away and 4 stopped there was none, so every stripe touching node 4 was acknowledged on two fragments (`degraded acknowledgement`, 14 marks), although node 3 was up and acknowledging. Fails whenever node 4 joins and stops within 10 s of node 3's last hard failure. | product (client) | `src/client/write.c`: an away member is a spare of last resort before a degraded acknowledgement; an acknowledged fragment clears the member's away mark; `EFS_FAULT_AWAY_MARK_MS` overrides the window. Case 7 now forces the interleaving (node 3 marked away, window 600 s, then node 4 stops) and asserts the place-around onto node 3. |
| D4 put error | The packer sent each fragment once. Node 3, a follower under the pool's load, refused one segment PUT with `NOT_PRIMARY` (its linearizable read of the object admitted a moment ago); the packer took that for a missing fragment, wrote the segment degraded (`put_errors=1`) and the rebuild restored it 0.8 s later. The packer-side twin of K34. | product (packer) | `src/server/packer.c`: a fragment refused BUSY, NOT_PRIMARY or AGAIN is sent again (4 times, 50 ms apart) before the segment is written degraded; `EFS_FAULT_SEGMENT_PUT_TRANSIENT` (EFS_FAULTS builds) refuses one PUT NOT_PRIMARY; K37 case 1b forces it. |
| K37 case 7 | `degraded_writes()` concatenated the three daemon logs and sliced the combined list by a count taken earlier. After case 6's outages the packer leads from another node, whose new line sorts before the other members' older ones, so the "new" entry was an old, already rebuilt segment; the test deleted a healthy fragment of it and the audit simply rebuilt it (`Degraded segments: 0`). | test | The helper returns the entries not in an earlier result, by segment identity. |
| B geometry | The automatic quota is a function of the `statvfs` sample efsd took at startup; the test re-sampled free space after the member's B checks (45–53 s later) and allowed 256 MiB of drift. The other seven pool jobs move more than that (1.1–1.3 GB in the three failures). | test | efsd logs its exact budget inputs (`storage-budget: fs= total= available= used= budget=`, `efs_storage_safe_quota_detail`); the test recomputes the budget from that sample and asserts equality, plus the deterministic ceiling. |
| K36 case 11 | The segment the test chose for "one fragment lost" could already be marked degraded: the packer writes a segment degraded when a member is still in its peer cooldown after case 10's kill and restart. Deleting a second fragment of it is a correct `UNRECOVERABLE` (`mask=3`), not this case. | test | Case 11 waits for `Degraded segments: 0` before choosing the fragment. |
| K36 case 8 (once, alone) | Read EIO right after node 1's recovery from the compaction crash. The kept tree was lost (loop keep variable missing); it did not recur in 5 + 3 further runs. | open | Noted; not reproduced with logs. |
| report step-down (1/10 baseline; 1/8 in the third final gate; 5/28 beside a pool run) | PUT ticket admissions (`PUT_TICKET_ADMIT`, tag 39) forwarded between two followers of the group whose leader had just stepped down (node 1 to the old leader, the old leader on), each hop holding a peer-pool connection, until both pools were full and every queued forward waited out the 5 s pool checkout and failed BUSY: `propose-slow group=2 tag=39 ... ms=5000 rc=-13`, 65 times on two nodes, all ending in the same millisecond. The writers' write()/fsync() sat behind those admissions for 5.2-6.0 s although the new leader was elected 0.5 s after the step-down. The submit path has answered a forwarded generic command NOT_PRIMARY on a follower since Sep 21; the ticket admission branches propose before that check, and `host_propose` forwarded on. | product (server) | `host_rpc_submit` marks a member's forward in the group byte; `server_raft_host_submit` serves a marked command without a further forward (`host_serving_forward`, DIR_MIGRATE exempt); `host_propose` refuses NOT_PRIMARY with the leader it knows (`8f1d50ed8`; fresh wire format for the group byte). `tests/test_forward_once.py` pins the contract (fails with the refusal removed); `test_host_read_deadline.py` expects the marked byte; `test_propose_hold_ownership.py` compiles the production body with the refusal. Kept for diagnosis: `EFS_FAULT_RAFT_ELECT_LOG` (every election line, fault builds), which the step-down test sets with `EFS_RPC_PROF` and uses to print the stall window on a timing failure. Loaded rounds: 5/28 before, 0/40 after. |
| K33 payload read-back EIO (once, the pool run beside the first fixed-tree step-down loops) | `w-212 read back ... OSError(5)`; the run kept no daemon logs. It never failed in the 10 baseline pool runs, 3 serial runs or the clean final-gate runs, nor in 12 runs with kept trees beside a pool run at `8f1d50ed8`. | open | Noted; not reproduced with logs. |
| B6 refuse mode (final gate only, 1 of 8 pool runs at `36f2e3874`; then 1 of 8 alone) | `test_b6_unavailable.py` read `packed_total` and `degraded_writes` in two log reads 4 ms apart, between the packer's `written degraded` line (13:07:28.459) and its pass-end `packed_total=` line (13:07:28.463): three files counted as packed during the fault while their segment's degraded line was already in the "before" set. | test | Both counts come from one log snapshot at each sample point. |
| query-first NOTRUN | The UNFORMED refusal at formation is a race the test waited for in up to 12 formations; it fired in 2 of 13 runs, so the gate's NOTRUN count varied. | test (determinism) | `EFS_FAULT_QUERY_FIRST_UNFORMED` refuses every member's first capture once; the test runs a forced cluster and a natural one and never reports NOTRUN for the case. |
| runner self-test | Its PASS line said "NOTRUN 3", which the runner's `\bNOTRUN\b` scan counts. | test | The word is now "incomplete". |

## How the step-down stall was found

The baseline failure printed only the bound. Four rounds of the same
loaded setup (`test_report_stepdown.py` looped 10 times in four parallel
loops beside one 8-job pool run of the gate; driver
`~/efs-artifacts/rsd-load.sh` on intel1, method in `scripts/flake_repro.sh`)
narrowed it, each round keeping the failing trees:

| Round | Build | Runs | Failures | What it showed |
|---|---|---|---|---|
| rsd | `e69eb4404` | 28 | 5 | Every stall starts at one operator step-down and ends about a second after the next one, 4 s later; the election log (one line per 10 s per group) hides the leader timeline. |
| rsdi | + full election log, client RPC timing (`c11ef971e`) | 27 | 1 | The new leader is elected 0.5 s after the step-down; one APPEND RPC waits 4.76 s in a single receive, no retry. |
| rsdj | + `append-slow` phases | 7 | 1 | The reservation's `execute` phase (its PREPARE/DECIDE/RESOLVE proposals) took 5.15 s and ended BUSY. |
| rsdk | + `propose-slow`, `remote-cmd-slow` | 7 | 1 | 65 forwarded group-2 ticket admissions took exactly 5000 ms and returned BUSY on two nodes: a forward chain filling the peer pools. |
| rsdf | fixed (`19e02ba06`, same forwarding change as `8f1d50ed8`) | 40 | 0 | |

The traces of rounds rsdj and rsdk (`append-slow`, `propose-slow`,
`remote-cmd-slow`) were temporary and are not in the final source. Later
rsdk iterations ran on rebuilt binaries and are excluded; their results
are kept apart on the host under `superseded/`.

A deterministic daemon reproduction was attempted and dropped: holding a
group leaderless (fault hooks for a follower election timer and for
parking admissions before their proposal) made the client fail the writes
EIO on both builds before any chain formed, so that test did not
distinguish the defect and is not committed. The contract test above and
the loaded rounds are the regression evidence.

## Verification of the fixes

- The reworked K35 case 7 against the base binaries (`~/efs-flake`, base
  client): FAIL, `degraded count 13` (the defect, now forced); against the
  fixed client: PASS, `fragments placed around on node 3`.
- K37 case 1b against a daemon with the hook and the retry disabled
  (`PK_PUT_RETRIES 0`, `~/efs-flake3`): FAIL (the refusal fired once, no
  `resent` line, the segment written degraded); against the base daemon
  (no hook): FAIL "the refusal never fired"; against the fixed daemon: see
  the final gate.
- `tests/test_forward_once.py` with the refusal removed from
  `host_propose`: FAIL (the wiring check); fixed: PASS.
- K33 payload beside a pool run at `8f1d50ed8` (4 loops x 3, trees
  kept): 12/12 PASS; the pool run beside them 472 commands, 0 FAIL.
- Every changed test once on the fixed tree: `test_storage_capacity.py`,
  `test_make_test_summary_runner.py`, `test_query_first_catalog.py` (forced
  and natural clusters, no NOTRUN), `test_k35_fsync_degraded.py`,
  `test_k36_recovery.py`, `test_k37_segment_rebuild.py`,
  `test_b_geometry.py`, `test_d4_progress.py`, `test_report_stepdown.py`:
  PASS.

## Final gate

NOTRUN. The final gate at `8f1d50ed8` (ten 8-job pool runs, three serial
`test-long` runs) was stopped at the parent session's request during its
first pool run, at 434 of 472 commands with 0 FAIL, because the branch had
been merged and is gated on the merge branch (merge-wave8b on intel2).
Evidence for the fixes on this page: the per-case reproduction counts
before and after, the forced regression cases failing against the unfixed
builds, the loaded step-down rounds (5/28 before, 0/40 after) and K33
beside a pool run (12/12, pool 0 FAIL). Earlier final-gate attempts at
intermediate commits: at `36f2e3874` 7 of 8 pool runs 0 FAIL (the eighth
failed B6, fixed in `e69eb4404`); at `e69eb4404` the first pool run failed
the step-down bound (fixed in `19e02ba06`/`8f1d50ed8`). Remaining gate: the
merge branch's gate run (ten pool and three serial runs on the merged tree
would repeat this sweep's criterion).

## Reproduction

Copy a tree with `git archive <commit> | ssh intel1 'tar -C ~/TREE -xf -'`
(touch synced sources before `make` when copying single files from a Mac:
tar keeps their old mtimes), then on the host, in `screen`:

```
# the gate: N pool runs at 8 jobs, then M serial test-long runs
bash scripts/flake_sweep.sh ~/TREE ~/efs-artifacts/flake-sweep-20261025 LABEL 10 3
# the intermittent daemon tests alone, then beside one pool run
bash scripts/flake_repro.sh ~/TREE ~/efs-artifacts/flake-sweep-20261025/repro 5
# one test in parallel loops beside a pool run (the step-down rounds)
bash scripts/flake_load.sh ~/TREE ~/efs-artifacts/flake-sweep-20261025/repro rsd test_report_stepdown.py 4 10
```
Prerequisites: fuse3 and `/dev/fuse`, `~/bin/edelete`, about 2 GB free under
the tree. Each run cleans its own `tmp/`; a stopped run is cleaned by
stopping the drivers and tests, unmounting the tree's mounts and deleting
its `tmp/`. Expected checks: `summary.txt` line 1 with FAIL 0, NOTRUN only
for host limits.
