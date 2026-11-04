# Gate profile: where the slow `make test-all` rows spend their time (Task DA, Nov 4 2026)

**Verdict: PASS.** The budgeted gate in `tests/budgets.tsv` (the recipe test's own model at 8 jobs, `GATE_BOUND_S` 1750 unchanged): **1750 s before, 1305 s after**, from faster tests and two product fixes, with 13 rows re-measured on fstor007 and node9900 and every other row's budget kept. Measured gates on the final tree: 651 to 722 s (the Nov 3 and 4 gates ran 686 to 894 s).
Two product defects found and fixed (K89 Raft commit propagation, blocks alpha: no; the packer's 30 s failure pause is now a setting); one product defect recorded, not fixed (K90: unmount drain after ENOSPC on a full member spins on BUSY for the 60 s stop bound).

## Method
Every row in scope ran solo on fstor007 and node9900 with a harness patch (host copy of `tests/k33_daemons.py`, not committed) printing the wall time of every daemon start, formation wait, mount, unmount and stop, and the tests' own step lines unbuffered. Where the harness-visible waits did not explain the wall time, the daemon and client logs of a kept run gave the split, and `strace -f -tt` plus temporary Raft trace lines (scratch tree only) gave the mechanism. Before/after numbers in the commit messages are solo runs; budgets come from three 8-job gates per host on the final tree (below).

## Findings

1. **Raft: a committed entry reached its second acker only with the next heartbeat (K89, product, fixed in `5e16017a8`).** `test_stop_progress` took 12 s or 82 s solo with nothing else different; the slow runs spent 70 s creating 1,500 directories. A probe over fresh clusters tied it to leader placement: with both metadata groups led by the mount's node, mkdir p50 was 7 ms; with the two leaders on different nodes, 220 to 520 ms (14 to 28 directories/s against 600). The daemons logged `raft-host: apply-sleep us=50000` once per forwarded command and `strace` showed the waiters in condition waits, not sleeps. Temporary trace lines in the Raft core (scratch tree only) gave the mechanism: the first follower ack commits the entry and `on_ae_rep` pushes the commit, but the follower whose ack is still in flight gets a commit probe built from its old match index, which `on_ae_req` caps (`commit <= prev_index + nentries`), so that follower learned the commit from the 50 ms heartbeat; the leader's own late fsync (`efs_raft_durable`) advanced the commit with no push at all. Fix: the second acker's reply sends it a probe at its new match; `efs_raft_durable` broadcasts when it moved the commit. Two `test_raft` checks fail on the old code. After: mkdir p50 7 to 12 ms in every placement; `test_stop_progress` 11.5 to 12.5 s in every run. Every daemon test that forwards metadata commands was bimodal in the earlier gates for this reason (`report_stepdown` 34 vs 122/190 s, `k38_protection_now` 23 vs 53 s, `k33_interrupt` 46 vs 80 s, `k36_recovery` 27 vs 60 s, `k22_snapshot_import` 111 vs 206 s).
2. **Packer: a fixed 30 s pause per group after any failed or degraded upload (`PK_FAIL_BACKOFF_NS`, product behavior, now the setting `EFS_PACK_FAIL_BACKOFF_MS`, `5aa9d659e`).** In `test_k37_segment_rebuild` each member-down step wrote its degraded segments within a second and then waited 26 to 31 s for the packer's next pass (`lag_s=30` in the pass line that followed), 90 s of 165 s. The default stays 30 s; K37 and B6, which run with a member unavailable by design, set 2 s. K37 solo: 127 to 165 s before, 65.5 s after, assertions unchanged.
3. **`test_report_stepdown`: a fixed workload that the faster appends outran (test design, `030e35e19`).** Its 315 s budget came from the withdrawn intel1 (250.8 s median); the ORCD hosts ran it in 34 to 48 s. With the Raft fix, 4 x 400 records drained in 17 s, before the third step-down round, and the test failed on its step-down count (1 of 2 solo runs), the failure Task CZ met after `628f0d069` and answered by doubling `EFS_RSD_RECORDS`. The writers now keep appending until the schedule has run (`EFS_RSD_RECORDS` is a floor), so the schedule, not the append speed, sets the test's time: 44 to 48 s solo on both hosts, 3,300 to 3,600 records. Wave 56's `EFS_RSD_RECORDS=800` composes with this.
4. **`test_pump_logging`: 28 s of fixture setup behind the deliberate fsync interposer (test design, `ed44f9daa`).** The 40 ms serialized fsync is in force from daemon start; the 32 seed files, written one by one, took 28 s before the 35 s window began. Written by 16 threads they share group commits; the fixed 6 s settle is a poll with the same bound. 108 to 114 s before, 88 s after.
5. **`test_k22_snapshot_import`: a serial read-back (test design, `3b21b6f5c`).** 800+ files, most of them 4 MiB, hashed one at a time through node 3: about 40 s on node9900 (8 s on fstor007). Eight readers: 2 s. The stop phase (four leader snapshots past the stopped follower, 57 to 91 s) and the catch-up (15 to 36 s) are product time under the test's load mix and were left alone, as was the 26 s warm window that sets the RSS bound.
6. **Unmount drain after `ENOSPC` on a full member spins on BUSY (K90, product, not fixed).** In B6 full mode the writing mount's close took 30 s on both hosts until the harness killed `efs-fuse`: `fusermount3 -u` returned at once, then `efs_fuse_destroy`'s 60 s drain retried a whole-cache REPORT that answered BUSY (`report-sync-fail ino=0 stage=report-retry rc=-13`) every 215 ms. The same close followed `test_k22_snapshot_import` on node9900 when its big writers' close had returned `EIO` during the stall (2 of 2 runs after the K89 fix, 0 of 1 before, 0 of 1 on fstor007 after; too few runs to say whether K89 changed its odds). Which dirty units stay held is not traced; recorded as K90 (OPEN, blocks alpha: no).

## Per-test table
Solo wall seconds (one run each unless slashed), `EFS_TEST_JOBS=8` in the environment, nothing else on the host: devel `42ee15ab8` ("before"), after the K89 fix alone ("K89"), and the final tree where a test or its product path changed ("final"). The split column says where the before time went, from the harness timings (every mount, close, stop and formation wait) plus the tests' own step lines and the kept daemon logs.

| Row (budget before) | node9900 before / K89 / final | fstor007 before / K89 | Where the time went (before) | What changed |
|---|---|---|---|---|
| `test_k22_snapshot_import` (425) | 197.6 / 178.8 / 158.0, 125.8 | 110.3 / 158.3 | 26 s warm window (sets the RSS bound), 57 to 91 s until four leader snapshots pass the stopped follower, 15 to 36 s catch-up, serial read-back of 830+ files (about 40 s on node9900, 8 s on fstor007); on node9900 after K89 a 30 s unmount drain after `EIO` closes (K90) | read-back by eight readers (`3b21b6f5c`) |
| `EFS_B6_MODES=full` (400) | 232.4 / 78.8 FAIL (K53 room scenario, the known intermittent) / 208.5 | 227.9 / 209.6 | fill and settle rounds (`FULL_QUIET_S` 3 s each), packer waits with node 3 at quota (30 s backoff per failed upload), the 30 s unmount drain of the writing mount (K90), restore and rebuild | packer backoff 2 s in the test (`5aa9d659e`); K90 left |
| `test_report_stepdown` (315, exclusive) | 39.4 / 44.4 / 44.7 to 46.3 | 36.4 / 48.4 | 1600 appends with fsync every 8 plus a 28 s step-down schedule; budget from the withdrawn intel1 | writers run until the schedule has run (`030e35e19`) |
| `test_k37_segment_rebuild` (290) | 127.3 / 154.6 / 65.5, 65.1 | 165.3 / 164.7 | four member-down steps each waiting 26 to 31 s for the packer's next pass after a degraded write (90 s); stops, mounts and closes under 15 s in all | packer backoff 2 s in the test (`5aa9d659e`) |
| `EFS_B6_MODES=slow` (260) | 207.8 / 174.9 | 212.9 / 201.5 | the deliberate 8 s, 45 s and 65 s member delays and the client deadlines they cross: the test's content | packer backoff 2 s in the test; nothing else |
| `test_k33_growth` (225) | 59.6 / 62.5 | 123.6 / 117.4 | membership growth 3 to 8 with fault exits; fstor007 twice node9900 in every run (formation and join waits, 7.7 s of `wait_log` on fstor007), not traced further | none |
| `test_k35_fsync_degraded` (205) | 146.5 / 131.1 | 147.7 / 131.0 | member stops and restarts (12 stops), fsync with members down, W154 rebuild waits: design | none |
| `test_d4_progress` (195) | 98.6 / 98.5 | 94.1 / 89.8 | `LOAD_S` 45 s plus drain and verification: design | none |
| `test_d4_plateau` (185) | 100.2 / 94.5 | 95.3 / 93.4 | `LOAD_S` 75 s plus drain: design | none |
| `posix_expiry.py --private` (185) | 114.7 / 111.7 | 118.5 / 107.5 | about 25 expiry phases, each bounded by a 3 to 12 s TTL and a reclaim pass: design | none |
| `test_stop_progress` (175) | 82.2 / 16.2 | 12.8 / 12.5 | 70 s of the 82 s run was 1,500 mkdir+create through FUSE with the metadata leaders split across nodes (K89); the stop steps themselves 3 s | K89 |
| `test_pump_logging` (95) | 108.3 / 109.3 / 88.4 | 113.7 / 109.5 | 28 s fixture setup behind the 40 ms serialized fsync, two windows (35 s and 10 s), two 6 s settles, two formations (7 s each) | seed files written at once, settle polled (`ed44f9daa`) |
| `test_k38_protection_now` (30) | 22.9 / 23.5 | 52.3 / 25.0 | the 52 s runs were the split-leader mode (K89) | K89 |
| `test_k38_shared_chunk_pressure` (70) | 18.3 / 18.2 | 19.7 / 17.1 | 18 s solo; the 60 s gate outliers were under load | none |
| `test_pack_dup_candidate` (75, exclusive) | 40.0 / 46.3 | 51.8 / 40.1 | waits for a wall-clock pack-epoch boundary (up to 60 s) by design | none |

## What was left and why
- **K90**, the unmount drain spin after a refused write: 30 s in every B6 full run and in some k22 runs on node9900; a client-side held-unit question, not traced in this task.
- **`k33_interrupt` is still bimodal** in the gates (node9900 31.7 / 75.8 / 75.6 s, fstor007 30.0 / 48.4 / 75.7 s) after the K89 fix; its budget follows the rule (95 s, was 120) and the second mode is untraced.
- **`k33_growth` on fstor007** runs twice its node9900 time in every run (117 to 124 s against 60 to 63 s; 7.7 s of it in formation `wait_log`s); untraced, budget kept at 225.
- **`b6 slow`, `k35`, the D4 pair, `posix_expiry`, `b_geometry`**: their time is deliberate delays, load windows, TTLs or member restarts; nothing in the harness waits slower than the condition changes. Budgets kept, although several measure at half their budget (the rule applied to the six gates would give `k35` 170, `d4_progress` 120, `d4_plateau` 165, `posix_expiry` 145, `grow-a` 35, `grow-b` 55, `k36_segment_recovery` 45); re-budgeting an untouched row is the user's call.
- **`pack_dup_candidate` (exclusive, 75)** waits for a wall-clock pack-epoch boundary; the 60 s `EFS_PACK_EPOCH_S` is a constant, not a setting. Making it one would cut up to 60 s off the exclusive tail; left as a follow-up. **`w166_authority` (exclusive, 50)** measures 5 to 7 s on both hosts; its budget came from the withdrawn amd and is kept because the test did not change.
- **The 26 s warm window in k22** sets the RSS bound and was left; the stop phase (57 to 91 s until four leader snapshots) is the product under the test's load mix.
- **`test_k35_fsync_degraded` step 7b** returned `EIO` to its writer once in seven gates on fstor007 (gate 4; `dcache-flush-fail ino=... stage=put rc=-13`, then `flush-meta ... report_rc=-13`, while the group-2 leader was stopped): the K38/K39 family of a PUT refused BUSY across a leader change; 5 of 5 solo runs on fstor007 passed after (131 to 151 s). Whether K89 changed its odds cannot be told from one occurrence.
- **One gate failure per host over the six measurement gates**, both known intermittents under load, both 3 of 3 solo after: fstor007 gate 1 `test_put_quota_reclaim` (`efs-mgmt status` answered with no group leader reporting, `Protection` not full; the fixture was not kept by the gate), node9900 gate 3 `test_k41_packer_placement` (no `cancelled, placed again on live spares` line; on the known load list since wave 53). Gate 3 on fstor007 ran `test_k33_payload` at 68.4 s against its 55 s budget (untouched row; 24 to 26 s in the other five gates).

## Gates and budgets
Three `make test-all` gates per host at `EFS_TEST_JOBS=8` on the final tree (`3b21b6f5c`, a clean `git archive`, no harness instrumentation), one after another, nothing else of mine on the host (the foreign `tiny-files*` screens on node9900 were idle; load averages at each gate's start are in the artifact `uptime-before.txt`):

| Host | Gate | Result | Wall |
|---|---|---|---|
| fstor007 | 1 | TOTAL 585 FAIL 1 (`test_put_quota_reclaim`, above) NOTRUN 3 | 652 s |
| fstor007 | 2 | TOTAL 585 FAIL 0 NOTRUN 3 INCOMPLETE | 657 s |
| fstor007 | 3 | TOTAL 585 FAIL 0 NOTRUN 3 INCOMPLETE, OVERBUDGET `k33_payload` 68.4 vs 55 | 722 s |
| node9900 | 1 | TOTAL 585 FAIL 0 NOTRUN 4 INCOMPLETE | 651 s |
| node9900 | 2 | TOTAL 585 FAIL 0 NOTRUN 4 INCOMPLETE | 661 s |
| node9900 | 3 | TOTAL 585 FAIL 1 (`test_k41_packer_placement`, above) NOTRUN 4 | 660 s |
| fstor007 | 4 (new budgets) | TOTAL 585 FAIL 1 (`test_k35_fsync_degraded` step 7b: the writer's fsync returned `EIO` while the group-2 leader was being stopped, client `dcache-flush-fail stage=put rc=-13`; 1 of 7 gates, solo runs below) NOTRUN 3, OVERBUDGET `k38_shared_chunk_pressure` 25.6 vs 25 | 669 s |
| node9900 | 4 (new budgets) | TOTAL 585 FAIL 1 (`test_pump_logging`: the profile's wchan sampler read a thread that had just ended, `ProcessLookupError`; harness race, fixed in `d0cf0a658`) NOTRUN 4 | 660 s |
| fstor007 | 5 (final tree) | TOTAL 585 FAIL 1 (`test_server_processes`, an EXCLUSIVE real-process test, while five solo `k35` runs of mine shared the host; 3 of 3 solo after, 5 to 6 s) NOTRUN 3 | 671 s |
| node9900 | 5 (final tree) | **TOTAL 585 FAIL 0 NOTRUN 4 INCOMPLETE** | 649 s |
| fstor007 | 6 (final tree, nothing else on the host) | **TOTAL 585 FAIL 0 NOTRUN 3 INCOMPLETE** | 647 s |

NOTRUN rows are the host limits (cluster scripts, the Memcheck header on fstor007, valgrind on node9900, RDMA live qualification). Passing runs only count toward a median.

Rows re-measured (the file's rule: max of the two host medians x 1.25, rounded up to 5 s; the row note carries the six runs). Touched means the test changed, or its product path changed (the packer pause for the four B6 modes), or the earlier bimodality was K89 and the medians moved on both hosts:

| Row | Before | After | Host medians (fstor007 / node9900) |
|---|---|---|---|
| `test_k22_snapshot_import` | 425 | 185 | 134.5 / 144.7 |
| `EFS_B6_MODES=full` | 400 | 270 | 214.0 / 214.3 |
| `test_report_stepdown` (exclusive) | 315 | 65 | 49.0 / 45.6 |
| `test_k37_segment_rebuild` | 290 | 90 | 68.3 / 65.1 |
| `EFS_B6_MODES=slow` | 260 | 255 | 201.3 / 199.0 |
| `test_stop_progress` | 175 | 20 | 12.6 / 13.0 |
| `test_k33_interrupt` | 120 | 95 | 48.4 / 75.6 |
| `test_pump_logging` | 95 | 110 | 87.8 / 87.6 (was 108 to 115 s measured against 95) |
| `test_k36_recovery` | 70 | 55 | 43.1 / 38.7 |
| `test_k38_shared_chunk_pressure` | 70 | 25 | 17.8 / 16.5 |
| `EFS_B6_MODES=down` | 60 | 30 | 23.7 / 23.4 |
| `EFS_B6_MODES=refuse` | 55 | 30 | 20.9 / 20.7 |
| `test_k38_protection_now` | 30 | 30 | 22.5 / 21.4 |

The three largest savings in the model: `test_report_stepdown` (exclusive, counts 1:1: 250 s), `test_k22_snapshot_import` (2 slots: 60 s of pool time) and `EFS_B6_MODES=full` (2 slots: 33 s). `python3 tests/test_make_test_recipe.py` passes at every commit; the bound stays 1750.

## Artifacts
`~/efs-artifacts/taskDA-20261104/` on the ORCD hosts (NFS home): `before-solo-node9900/`, `before-solo-fstor007-instrumented/`, `after1-solo-*`, `fix2-solo-node9900/`, `probes/` (the Raft trace of a split-leader mkdir, the k22 unmount-drain log), `gate-<host>-g1..g5/` with `summary.txt`, `timings.tsv`, `progress.txt`, `gate.log` and any `fail-*.log`, `solo-checks-*/`; `SHA256SUMS` (sha256 d8d5be0c7ef664eb8f7ca0d0bb34c2cc665b1fdd9f963ec8750b02644d6a763f). The final gate on each host is FAIL 0 on the final tree: node9900 gate 5 (649 s) and fstor007 gate 6 (647 s).
