# `test_pump_logging.py` write floor: what the count measures (Task FD)

Task FD, Nov 10 2026, devel `9bb50fe79` (product unchanged since wave 67), fstor007 and xorinox. Purpose: `tests/test_pump_logging.py` failed three times with 48, 44 and 44 writes against its floor of 50 (verdict PASS, no errors) and the task was to attribute the shortfall and fix what the measurement shows. Linked row: K72 (FIXED, [Task AE port](../2026-10/ae-product-port-20261030.md)); no new K row. Rules kept: no floor lowered, no budget raised, no sleep or timeout added.

**Attribution (written before any change):** case B, the test's shape, not a product wait. With the 40 ms fsync interposer the three daemons take turns on one lock, so each gets one 40 ms slot every 120.4 ms; a write is a chain of about 9 to 10 slots (1.1 to 1.3 s) of dependent Raft log syncs, and two writers in lockstep give `writes = 70 / seconds per write`. The floor of 50 means at most 1.40 s per write: 34 runs of the unchanged test on two hosts at load 1 to 50 counted 52 to 62 (mean 56.5, sd 2.8), and the shortfalls come from the rare run where hops miss their slot (6.0 slots per write at load 50, against 4.6 to 5.7 otherwise). The slot length itself barely moves (120.4 to 122.4 ms). No fixed back-off, retry sleep or lock queue is on the write path; every second of it is injected fsync.

## What the floor was meant to prove

Nothing about K72 that the rest of the test does not already prove. The unfixed daemons (devel `714d7e6fc`, [AE page](../2026-10/ae-pump-logging-20261028.md)) completed 32 to 60 writes in 35 s *with* four writes over 10 s and, in two runs, four `EBUSY` errors: the verdict (no error, no write over 10 s) and the five-second progress windows (`require_progress`: at least one write in every rolling 5 s after t = 10 s) catch all of it, and a count of 60 would have passed the floor. The floor was a coarse "most of the offered load got through" guard. The offered load is 140 writes; the injected fsync caps it near 57 on a healthy tree, so 50 is 12 % below the healthy mean on a quiet host.

## Measurements (the unchanged test, devel `9bb50fe79`)

`writes` counts the whole 35 s blocked window (`scripts/evidence/pump-logging/pump_logging_floor_runs.py` wraps the test and records each run; load is `/proc/loadavg` at the end of the run):

| Host and condition | Runs | `writes` | p50 write | Load (1 min) |
| --- | ---: | --- | --- | --- |
| fstor007, solo (other tasks idle) | 5 | 54 60 56 56 58 | 1.12 to 1.28 s | 2 to 12 |
| fstor007, beside the 8-job fast-suite loop (`EFS_TEST_JOBS=8`, `make_test_summary.sh`, target `test`, 31 s per pass) | 10 | 55 55 62 56 57 58 60 60 56 58 | 1.12 to 1.28 s | 4 to 6 |
| xorinox (16 CPUs), solo | 3 | 58 52 56 | 1.17 to 1.33 s | 1 to 2 |
| xorinox, beside the 8-job full gate loop (`test-all`, 15 min per pass, 22 to 25 OVERBUDGET rows) | 10 | 60 56 54 56 54 60 60 52 60 54 | 1.12 to 1.44 s | 4 to 22 |
| xorinox, beside 48 busy loops (load 47 to 50 on 16 CPUs) | 6 | 52 60 56 55 54 52 | 1.17 to 1.34 s | 47 to 50 |
| all of the above | 34 | min 52, mean 56.5, sd 2.8 | | |
| Failures seen in gates (not reproduced here) | | 48 (F1 run 1, fstor007, load about 70); 44 (wave 68 gate, fstor007, p50 1.52 s); 44 (Task FC full TSan lane, node9900, p50 1.56 s) | 1.38 to 1.56 s | 46 to 200 |

The runs never reached 50 or fewer, so the failures are not reproduced on demand; the three kept failures sit 3 to 4.5 sd below the mean. The failed fixture of Task EX (`~erbmi1/efs-artifacts/taskEX-20261109/f1-run1/pump-logging-fixture/`): writes complete in lockstep pairs, per 5 s window 4 6 6 8 6 10 6 2, p50 1.38 s, p99 2.08 s, 36 `report-split` lines with REPORT finish 198 to 737 ms, all rc 0. Most daemon log lines are dropped by design while stderr is blocked (`telemetry: overflow=607`), so the REPORT count of the log is not the REPORT count of the run.

## Where one write's time goes

`pump_logging_fsync_breakdown.sh` runs the test's first phase (the profile with the test's arguments, fixture kept) and attaches `strace -f -y -tt -e trace=fsync,fdatasync,sync_file_range` to the three daemons for 20 s in the middle of the window; `pump_logging_slots.py` summarises it. The interposer (`tests/live/w159_slow_fsync.c`) holds one `flock` per process while it sleeps 40 ms, so processes take turns and the threads of one process share a slot:

| Run (strace window, 20 s) | Writes in window | Slot period | Slots per write (n1 / n2 / n3) | `raft.log` sync in slots (n1 / n2 / n3 of about 168) | fsync-class calls per write (n1) |
| --- | ---: | ---: | --- | --- | ---: |
| fstor007 solo | 32 | 120.5 ms | 5.2 / 5.2 / 5.2 | 154 / 165 / 164 | 15.8 |
| fstor007, fast-suite loop | 36 | 120.5 ms | 4.6 / 4.6 / 4.6 | 155 / 166 / 166 | 14.4 |
| xorinox, full gate loop | 30 | 120.4 ms | 5.7 / 5.7 / 5.6 | 157 / 167 / 166 | 16.7 |
| xorinox, load 50 | 28 | 122.4 ms | 6.0 / 5.9 / 6.0 | 147 / 163 / 163 | 18.1 |

Arithmetic, fstor007 solo: the leader made 507 fsync-class calls in 19.9 s, 164 `kv/wal`, 154 `raft.log`, 189 others; 168 slots of 120.5 ms, so a slot per daemon holds up to 3 to 4 concurrent calls (a `raft.log` `fdatasync`, the `kv/wal` barrier, a background file). 5.2 slots per write times 120.5 ms is 0.63 s of leader slots per write; the chain from the client's open to the close visits the three daemons' rotation about twice, 9 to 10 slots = 1.1 to 1.2 s = the measured p50. Writes per second = 2 writers / seconds per write = 2 / 1.2 = 1.7, times 35 s is 58. At load 50 the slot is 2 ms longer but a write needs 6.0 instead of 5.2 slots: a proposal that reaches the daemon after its slot waits a whole rotation, so a few ms of scheduling delay costs 120 ms. That is the amplifier, and it is the harness's rotation, not a wait in the product.

Raft commands per file written (leader `batch-obs` counters over the 35 s window, 60 writes): `TRUNCATE` 60, `PUT_TICKET_ADMIT` 62, `REPORT_PUBLISH` 63, `PUT_TICKET_SETTLE` 62, `SESSION` 128, packer (`PACK_ADMIT/SWITCH/UNINDEX/INDEX/RETIRE`) 92, `GC_ACK` 14, `TRUNC_SWEEP` 6: about 8.3 proposals per write in 5 leader log syncs (301 syncs in the 38 s between two surviving `batch-obs` lines).

## Which syncs the durability contract requires

Required (invariant I10, [architecture §4](../../how-it-works/architecture.md#4-invariants)): the majority-durable `raft.log` append of each metadata op the client waits for. A written file has three of them in sequence, each depending on the previous reply: `TRUNCATE` (the `O_TRUNC` open), `PUT_TICKET_ADMIT` and `REPORT_PUBLISH`. Not on the client's path: `PUT_TICKET_SETTLE` (a batched pass in `src/client/put_ticket_owner.c`), the packer and GC proposals (the profile sets `EFS_PACK_INTERVAL_MS=200`, `EFS_PACK_MIN_AGE_MS=1000` on purpose), and the `kv/wal` barrier, which `src/server/raft_host.c` calls the derived barrier ("Raft is already durable") and runs on a worker; it shares the `raft.log` slot in 152 to 164 of the leader's 168 slots and adds none. The background files (GC journal, reclaim state, durable epoch, query-cache views, segment directories) are 18 % of the calls and ride in slots that already hold a `raft.log` sync (3 to 4 slots per daemon hold only background syncs). Not established: whether `SESSION` (2 per write) and the `ADMIT` proposal are on the write's critical path, and whether the leader's own `raft.log` sync overlaps the follower append; either would shorten a real write by a slot or more. That needs a per-proposal trace of one write (client RPC timestamps next to the daemons' log syncs), which this task did not do; it is a product-optimisation question, not a K72 defect, and nothing found here is a wait that is not an fsync.

## Decision

Case B. Every second of a write is the 40 ms fsync slots of dependent Raft proposals; scheduling noise is amplified by the 120 ms rotation; no back-off, retry sleep or queue is present. The floor of 50 therefore measures the host's scheduling against a hard-coded rotation, not K72. The test changes; the product does not.
