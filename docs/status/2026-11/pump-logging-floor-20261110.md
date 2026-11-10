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

## The change

`tests/test_pump_logging.py`: the blocked phase runs after a drained phase on the same cluster (`drain_seconds=10`, `seconds=25`; the total stays 35 s, so the 95 s `tests/budgets.tsv` row is untouched), and the absolute `r['writes'] >= 50` is replaced by `blocked ops_s >= 0.5 * drained ops_s` with `drained writes > 0`. Both phases pay the same 40 ms fsync rotation and the same host load, so load cancels; the progress windows (`require_progress`, after t = 10 s, that is exactly the blocked phase), the zero-error and no-write-over-10-s verdict, the exact payload readback, the overflow accounting, the role events and the 10 s unmount phase (`writes >= 20` with no fsync delay) are unchanged. No floor lowered, no budget raised, no sleep or timeout added, no product code touched.

## It still catches K72

Parent of the K72 fix: `git log -1 68303f40f^1` is `db4e74cd0` ("Merge eio-port: ORCD EIO stress repairs K66-K71"). Built separately on xorinox (`~/efs-taskFD/unfixed`, `efsd` fault build sha256 `50832dfa1ff3eec0…`, no `.git`), with the new test and `scripts/evidence/pump-logging/` copied in:

| Run on the unfixed tree | Result |
| --- | --- |
| the new test, 3 runs | FAIL 3 of 3 in 10 s: `daemon served without its required telemetry reporter` (the startup-refusal step, which the unfixed daemon lacks) |
| the same test with that one step skipped (`--skip-startup-refusal`), 3 runs | FAIL 3 of 3 (about 73 s), foreground failures: 0, 4 and 2 failed writes, late writes (over 10 s): 2, 2 and 1 |
| the drained and blocked phases of those three runs | drained 16, 14, 16 writes (1.6, 1.4, 1.6 per s); blocked 6, 4, 1 writes (0.24, 0.16, 0.04 per s); ratios 0.15, 0.11, 0.03, far below the 0.5 the new assertion requires |

So the unfixed daemons fail the verdict and the progress windows as before, and the new ratio assertion alone would also fail them by a factor of 3 to 12. The new test needs the startup-refusal step to pass first on a fixed tree, as the old one did.

## Results after

Fixed tree `9bb50fe79` plus the new test, xorinox (16 CPUs), 20 runs beside the 8-job full gate loop (`EFS_TEST_JOBS=8`, `test-all`, about 15 min per pass; of the two passes that ran beside these runs one printed `TOTAL 594 FAIL 1` and the failing test was not retained, see Not done), host load 4 to 26 at the end of each run:

- **20 of 20 PASS.** `writes` 45 to 62; blocked/drained rate ratio 0.89 to 1.32 (median 1.1); p50 write 1.12 to 1.52 s.
- **Run 6 counted 45 writes** (p50 1.52 s, load 8, drained 10 writes at 1.0 per s, blocked 33 at 1.32 per s, ratio 1.32): the old floor would have failed it; the new test passed it, because the drained phase of the same run was slow as well.
- Wall time 85.4 to 96.0 s per run including the startup-refusal step; two runs (96.0 s, 95.9 s, both beside the 8-job gate on 16 CPUs) ran past the 95 s row. The old test ran 88.5 to 94.3 s on the same host in the same conditions (x-heavy and x-hog rows above), and 84 to 89 s on fstor007; the row stays (rule: budgets only move down).
- `make test` rc 0 (xorinox 2 min 10 s, fstor007 2 min 24 s).
- Two further full 8-job gate passes of the changed tree on xorinox with nothing beside them (`gate-xorinox/` in the raw output): `TOTAL 594 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (967s)` and `(899s)`; `test_pump_logging.py` PASS in both (90.9 s and 93.1 s of the 95 s row).
- `bash tests/sanitizer_check.sh tsan`: fstor007 (the reference host, tree `9bb50fe79` plus the new test, nothing else running): `VERDICT: PASS (31 run, 0 failed, 620s)`, rc 0, `PASS pump_logging (94s)`, `make test` rc 0 there too. On xorinox the same lane ended `FAIL (31 run, 1 failed, 1189s)`: `k96_status_slots_live` reported 6 ThreadSanitizer reports (660 s) on that host (gcc 14.3.1, kernel 6.12); `pump_logging` PASS (98 s) and `stop_blocked_stderr` PASS. The k96 test and its code are untouched here and the fstor007 lane passes it; the xorinox reports were not investigated (the log is kept; the host was removed from use afterwards).

## Reproduce

```sh
# on a Linux build host, tree of 9bb50fe79 (git archive) extracted, TMPDIR on a disk with headroom, make -j16 && make tests/faults/efsd
python3 scripts/evidence/pump-logging/pump_logging_floor_runs.py --runs 20 --tag new --out DIR   # runs tests/test_pump_logging.py N times, one FDRUN line each
bash scripts/evidence/pump-logging/pump_logging_fsync_breakdown.sh TAG DIR                        # strace the three daemons for 20 s in a 35 s blocked run
python3 scripts/evidence/pump-logging/pump_logging_slots.py DIR/TAG                               # slots, calls and slots per write
# load: EFS_TEST_JOBS=8 EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh   (in a second copy of the tree, in a loop)
# unfixed tree: git archive $(git rev-parse 68303f40f^1), add the new test and scripts/evidence/pump-logging/, build, run with --skip-startup-refusal
```

Cleanup: stop the load loop by pid (the loop's process group and every process whose working directory is the load tree), unmount stale `efs-fuse` mounts with `fusermount3 -uz`, remove the work trees.

## Raw output

Outside Git, on two hosts: fstor007 `~erbmi1/efs-artifacts/taskFD-20261110/` (solo, fast-suite-loop and strace-breakdown runs of the unchanged test, the fstor007 TSan and `make test` logs), `SHA256SUMS` sha256 `f8ddd2b5468061743332c01436903a597f41cffb0a5a10153e7c02f0bc1e126b (61 files)`; xorinox `~maik/efs-artifacts/taskFD-20261110/` (the xorinox series, the unfixed-tree runs, the 20 runs of the new test, the gate-loop logs, `make test` and the xorinox TSan lane), `SHA256SUMS` sha256 `6c279d7bff412ecf59ee5199763eff708d6c10f8b4a4c06e9ac0e6deef50b463 (56 files)`. Other tasks' fixtures cited: `~erbmi1/efs-artifacts/taskEX-20261109/f1-run1/pump-logging-fixture/`, `~erbmi1/efs-artifacts/merge-wave68-20261110-fstor007-b-gate1/fail-525.log`, `~erbmi1/efs-artifacts/taskFC-20261110/merged2/tsan-1-pump_logging.out`.

Builds: `9bb50fe79` from `git archive` (tar sha256 `618aa538…` uncompressed stream, `4e191134…` gzip -n used on fstor007); fstor007 gcc 11.5.0, Python 3.9.25, kernel 5.14.0-687.36.1, private libfuse 3, `efsd` fault build sha256 `e4c38e41…`; xorinox (Rocky 10.2, 16 CPUs, 60 GB, kernel 6.12.0-211.67.1, gcc 14.3.1, Python 3.12.15, system fuse3), fault build sha256 `cd5d7937…`. Binaries carry no git metadata. The test chooses kernel-assigned ephemeral ports (32768 to 60999), not 18000 to 19999 or 19000 to 19199; no port of the live clusters is in that range.

## Proposed K72 clause

For the coordinator to apply to the K72 row (not edited here): "Nov 10: `tests/test_pump_logging.py` no longer asserts an absolute write count; it requires the blocked phase to write at least half the drained phase's rate on the same cluster (Task FD, `docs/status/2026-11/pump-logging-floor-20261110.md`: the old floor of 50 sat 7 below the healthy count and failed on load at 48, 44 and 44; the new check fails the unfixed tree at ratios 0.03 to 0.15)."

## Not done

- The shortfall (a count under 50) was **not reproduced on demand**: 34 runs of the unchanged test at load up to 50 never counted fewer than 52. The attribution rests on the strace breakdown (the slot rotation and its amplification) and on the three kept failures; the loads that produced them (46 to 200 on 96 CPUs, a TSan lane) were not recreated.
- Which of the 5 leader log syncs per write could be removed is **not established** (see "Which syncs the durability contract requires"); no product change was made or attempted. A per-proposal trace of one write is the next measurement if that optimisation is wanted.
- Remaining gates are unchanged: this page closes no K row; `tests/budgets.tsv` is untouched (the 95 s row, two runs past it on a 16-CPU host under the gate).
- One of the two gate passes that ran beside the 20 runs ended `FAIL 1`; its summary was overwritten by the next pass, so the failing test is unknown (the 20 direct runs and the two later passes all passed `test_pump_logging.py`). Not run on fstor007: the full `test-all` gate with the change (the wave gates do that).
- The xorinox TSan lane failed `k96_status_slots_live` (6 reports); not investigated, the fstor007 lane is the one that counts for the gate.
