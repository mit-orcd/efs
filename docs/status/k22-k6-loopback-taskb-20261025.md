# K22/K6 loopback verification — snapshot import, leader admission, W130 DIR fault (2026-10-25)

Branch `agent/devel-taskb`, head `82ad8364e` (gate run at that tree state, `EFS_BUILD_TIME` of the run's binaries; `efsd --version` prints the embedded build time — version.o is not rebuilt per commit). Host xorinox, loopback clusters of the fault build (`tests/faults/efsd`), TCP transport, FUSE clients via `tests/k33_daemons.py`, `TMPDIR=$HOME/efs-taskB/tmp`. Gate: see the last section — `make test-all` via `scripts/make_test_summary.sh`, `EFS_TEST_JOBS=8`.

## What this closes

Three VERIFY items from `docs/status/alpha-1.0-2plus1-checklist.md` rows K22 and K6, each now a `make test-long` test with a `tests/budgets.tsv` row:

1. **K22 snapshot import into a stalled follower** — `tests/test_k22_snapshot_import.py`; production-scale driver `scripts/k22_snapshot_import.sh` for the coordinator (normal build, real hosts, `K22_SNAP_BYTES=0`).
2. **K22 live leader admission at 3/4 of the unapplied-log bound** — `tests/test_k22_leader_admission.py`.
3. **K6/W130 one-replica DIR fault** — `tests/test_w130_dir_fault.py`.

## 1. Snapshot import into a stalled follower — PASS (loopback)

Node 3 is SIGSTOPped while efs-bench metadata loops and FUSE payload writers drive both groups (`EFS_FAULT_SNAP_BYTES=1 MiB` shrinks the retained window; `EFS_RAFT_DBG=1` makes the install visible — `send_snap`/`on_snap_req INSTALLED` print only under it). The gate drives until both groups' leaders have snapshotted past node 3's match four times.

Findings that shaped the gate:

- The leader **pre-sends a bounded backlog** into the stopped peer's socket (measured ~2–2.5 snapshot windows; the outbox fills, then sends stop), so one snapshot past the match is not enough — the retained window must march past the backlog before the peer can only rejoin through InstallSnapshot. Four snapshots past the match puts the log floor ~3 windows past it, beyond the backlog.
- **Product defect found and fixed** (`5df86f843`): with a peer below the floor, every pump wake paid a fresh malloc+pread+frame for the same snapshot chunk when the outbox was full — ~300 full 4 MiB chunk attempts/s per leader, ~1.2 GB/s of memcpy churn, starving client RPCs into flush-meta timeouts. `send_snap` now marks the chunk's window in flight for one heartbeat on `EFS_ERR_AGAIN`, the same suppression a queued chunk already gets.

Measured (run 15, xorinox): drive to the floor condition 228 s; **INSTALLED 4 s after resume; caught up 2 s later**; node-3 RSS peak 129.6 MiB vs 61.4 MiB warm (bound 3× + 512 MiB slack); drain immediate; **271 files read back byte-exact through a mount seeded at node 3**; 30 post-recovery writes through node 3 clean. During the stall, six big-file closes failed with explicit EIO — allowed by the B6 member-fault contract (explicit failure, never silent corruption); those files are outside the readback manifest by construction and are reported here as findings (see W163).

NOTRUN here: the 1 GiB production-window lag on real hosts (the driver above), RDMA, multi-host.

## 2. Leader admission at 3/4 of the unapplied bound — PASS (loopback)

Node 1 (the forming node, group 0's first leader) runs a small KV budget (`EFS_FAULT_L0_BYTES=2 MiB`, `EFS_FAULT_MEMTABLE_BYTES=1 MiB` → `max_unapplied=16384`) with its compactor parked (`EFS_FAULT_COMPACT_STALL` + per-node marker file); efs-bench drives the log. Followers keep default budgets: with every node on a small budget the group throttles to its slowest memtable flush (~34 entries/s) and the fill outruns any sane deadline.

Measured (runs 5–8, xorinox): the leader's backlog crosses 3/4 of the bound at ~190–220 s with the host `admit_busy` counter climbing; the pressured node logs `election held: low capacity` and leadership moves to a member with capacity (observed on nodes 2 and 3); releasing the marker drains the backlog in ~3 s. Clients mounted on a healthy node: **443 writes, zero errors after the move**.

Two findings:

- The 3/4 refusal is the **host-level** admission (`raft_host.c`); the group-status `admit_busy=` counter is the raft-level refusal at the **full** bound, which the host's earlier refusal keeps from firing. Watch the `bound` line's `admit_busy` for the 3/4 gate.
- Inside the pressure window some client closes fail EIO at their 30 s flush deadline (reported by the test as findings). At this compressed budget scale the pressured leader's apply churn produces those well before the admission point; the production-scale window belongs to the multi-host drivers (W163).

## 3. W130 one-replica DIR fault — PASS (loopback), recovery implemented

`EFS_FAULT_DIR_CORRUPT_FILE` (new fault hook) damages the target directory's stored row on one replica at `apply_dir_cmd` entry, so the split's own DIR step meets a row the decoder refuses. The test fills a directory past `EFS_DIR_SPREAD_MIN=64`, quiesces writers across the split's apply window, then asserts: the halt diagnostic (`INCOMPATIBLE entry ... apply halted, group not served`), the frozen applied count on the halted replica while the leaders advance, healthy replicas and other groups serving, the damaged directory reading complete-and-exact or cleanly refused, and `efs-mgmt raft-resync` recovery with an exact re-read.

This verification **found and fixed two product defects**:

- `apply_dir_cmd` swallowed a decoder refusal (`EFS_ERR_PROTO`) as a successful apply — a replica would have diverged past a split step it never performed. Now mapped to `EFS_ERR_UNSUPPORTED`, halting the group through the W127 route (`0d3b031b2`; NOT_FOUND/INVAL/BUSY stay swallowed, IO/NOMEM keep their pre-existing retry).
- `efs-mgmt raft-resync` **could not recover a halted replica** (`e6bd940d3`): the trigger waited on a log conflict that never exists when the halted replica's log matches the leader's; the leader had no image below the snapshot cadence; the halt flag was sticky past any import. Now: a resyncing follower refuses every AppendEntries with match 0; the leader reads match=0 from a proven peer as a resync pull, exports an image outside the byte cadence (`snap_want`), and ships it; when the imported diff has replaced the group's keyspace past the halt index the halt clears and the group serves again. An image the build cannot decode fails the import and the halt stands.

Measured: halt 7–14 s after the fault arms; **raft-resync returns the replica to service in 14–20 s**; directory re-read exact. 6/6 PASS on the final test form (runs 16, 27–31).

Open finding **W162** (new backlog entry): if the damaged row meets the *serve* path before any DIR apply (a create into the splitting directory whose lane's leader holds the damage), client opens fail EIO persistently with no halt — the fail-closed route covers only the apply path. Kept forensics from runs 11/13/14/21; the gate avoids that window by quiescing writers across the split apply.

## Environment / reproduction

- Host xorinox (shared test host; ran beside the owner's measurement sessions without contention findings), loopback only, TCP, fault build via `make tests/faults/efsd`.
- `python3 tests/test_k22_snapshot_import.py` (env knobs `K22_SNAP_BYTES`, `K22_STOP_DEADLINE`, `K22_CATCH_BOUND`, `K22_DRAIN_BOUND`, `K22_RSS_FACTOR`, `K22_RSS_SLACK`); `python3 tests/test_k22_leader_admission.py` (`K22_L0_BYTES`, `K22_MEMTABLE_BYTES`, deadlines); `python3 tests/test_w130_dir_fault.py` (`W130_SPREAD_MIN`, deadlines). All three are in `make test-long` with budget rows.
- Raw run logs on xorinox under `~/efs-taskB/` (`snap-run15.log`, `admit-run8.log`, `w130-run*.log`, probe dirs); the kept failure forensics are the `efs-w130-*` work dirs named in those logs. Per AGENTS.md these stay out of git.

## Product defects (one line each)

- `5df86f843` send_snap retry storm into a saturated peer lane starved client RPCs — fixed, one-heartbeat backoff.
- `0d3b031b2` W130: DIR apply swallowed decoder refusals (would diverge replicas) — fixed, halts through W127.
- `e6bd940d3` raft-resync could not recover a W127-halted replica at all — fixed, image pull + heal.
- W162 (open): serve-path decode of a damaged row does not halt the group; client opens fail EIO persistently.
- W163 (open): close(2) EIO at the 30 s flush deadline while a leader's apply lags (stalled-follower window; compressed-budget churn) — needs a design decision (faster fencing vs deadline ride-out vs earlier admission shielding).

## Gate

(make test-all result is recorded here when the run completes; see the commit log for the tree state.)
