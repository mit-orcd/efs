# Gate speed: `make test-all` under 15 minutes at 8 jobs (W160, Oct 25, 2026)

**Purpose.** In 24 hours the merged-tree code gate on intel2 (8 CPUs) grew from
298 s to 2,641 s (467 commands), driven by the daemon tests added to
`make test-long` (K33 growth and acceptance, K34, K35, K36, K37, B6, D4,
client view, E2, report step-down); every merge wave paid it twice. Bring
`make test-all` through `scripts/make_test_summary.sh` at `EFS_TEST_JOBS=8`
on an 8-CPU host from about 44 minutes to under 15 minutes with the same
commands and assertions, and add a check that stops the gate from drifting
again. Item W160 ([work queue](work-queue.md)).

**Verdict.** **PARTIAL PASS.** On the nuc at `EFS_TEST_JOBS=8` the gate went from 1,182–1,217 s with 3–6 load failures per run (old runner, merge-wave7) to 811 s and 839 s with 0 FAIL in two of three runs of the committed configuration, and 989 s with two pre-existing intermittent failures in the third; serial, from 2,754 s to 2,311 s (0 FAIL before, 1 intermittent after). Same assertions, 478 commands for the old 471 (three tests split into independent commands). The drift check is in. **Not met:** three consecutive clean runs under 900 s; the nuc's single NVMe makes the fsync-heavy tests two to three times slower whenever two overlap, and two latency-bound tests (report step-down's 3 s bound, D4's packer PUT deadlines) fail beside them, so the pool holds the gate at 13.5–16.5 min with a tail of load-sensitive intermittents (details below).

**Source.** Branch `claude/devel-gate-speed-8aee30` on merge-wave7
(`0b01a2a1e`), test code `4550fdabc`. No production source changes: the
changes are in the runner, the Makefile recipe, the loopback test helpers,
the tests and the two qualification drivers (`scripts/b_checks.py`,
`scripts/d4_progress.py`). Build: `make -j8 test-bins` of that tree
(`gcc 14.3.1`, no git metadata in the staged copy).

**Environment.** nuc: 8 CPUs (x86_64), one NVMe (`/home`, LVM), Rocky Linux
kernel 6.12, Python 3.12, fuse3, loopback only; no RDMA device, no valgrind,
no `~/git/cluster` (the 14 NOTRUN host limits below). Tests run from a
private copy under `~/efs-gatespeed/`, each command with its own `TMPDIR`
under the tree's `tmp/`; private loopback ports (never 17xxx), no running
cluster touched. Dry run on intel1 (8 CPUs, other sessions' load) and test
development on amd (32 CPUs).

## Where the 44 minutes went

`timings.tsv` of the last merged-tree gate (intel2, `merge-wave6`): the sum
of the command seconds (2,643) equals the wall time (2,641), so that gate
ran serially. The 30 slowest commands and where their time goes (profiled
on amd with a per-statement wall-time tracer, `phase_time.py`, kept outside
Git):

| # | s | share | command | where the time goes |
| --- | --- | --- | --- | --- |
| 1 | 787 | 29.8% | `python3 tests/test_k33_payload.py` | single long test: one cluster, writer thread; serial verification of thousands of writer files with Python sha256 payloads (a third), packer and rebuild waits |
| 2 | 548 | 20.7% | `python3 tests/test_b_geometry.py` | single long test: B1-B5 driver at 6 sizes (serial fsync writes and read-backs), full phase; two 6 s settle sleeps; two 30 s unmounts after corrupt reads |
| 3 | 225 | 8.5% | `python3 tests/test_k33_seed_loss.py` | fixed sleep 12 s, then 3 x 32 s `efs-mgmt status` with a frozen member (serial) |
| 4 | 182 | 6.9% | `python3 tests/test_k37_segment_rebuild.py` | daemon timers: 2 x 30 s member-down detection, 9 unmounts with a member down (15 s fence each), 3 s settle sleep |
| 5 | 108 | 4.1% | `python3 tests/test_d4_progress.py` | real work: 45 s load, drain, serial verification of ~600 files, two 15 s unmounts |
| 6 | 98 | 3.7% | `python3 tests/test_query_first_catalog.py` | 12 serial formations, each with 3 x 1 s start gaps (by design) and formation waits |
| 7 | 96 | 3.6% | `python3 tests/test_k33_growth.py` | Python sha256 payloads (23 s), two 15 s unmounts, writes |
| 8 | 67 | 2.5% | `python3 tests/test_k36_recovery.py` | daemon timers: recovery and rebuild waits (polled), one 15 s unmount |
| 9 | 56 | 2.1% | `python3 tests/test_k35_fsync_degraded.py` | daemon rebuild waits, one 15 s unmount, two 3 s settle sleeps (negative checks) |
| 10 | 44 | 1.6% | `python3 tests/test_d39_model.py` | CPU-bound model search (pure Python), one process |
| 11 | 39 | 1.5% | `python3 tests/test_b6_unavailable.py` | 4 independent modes in series; deliberate slow-member delays (8/45/65 s); unmount fences |
| 12 | 38 | 1.4% | `python3 tests/test_k33_interrupt.py` | real work: 3 clusters formed, 9 interrupted additions |
| 13 | 37 | 1.4% | `python3 tests/test_k34_stop_put.py` | two 15 s unmount fences (30 s of 37 s) |
| 14 | 36 | 1.3% | `python3 tests/test_k33_bookkeeping.py` | client statfs refresh polls (12 s), settle sleeps 1+1+3 s, writes |
| 15 | 30 | 1.1% | `python3 tests/test_k33_autoid.py` | formation waits, one 3 s sleep (negative check: joiner waits) |
| 16 | 22 | 0.8% | `python3 tests/test_k33_client_view.py` | real work: writer thread, fsyncs |
| 17 | 15 | 0.6% | `python3 tests/test_member_limit.py` | formation of 16 members |
| 18 | 12 | 0.5% | `./tests/test_reclaim` | C unit test, real work |
| 19 | 12 | 0.4% | `python3 tests/test_opid_admission.py` | exclusive (serial after the pool) |
| 20 | 11 | 0.4% | `python3 tests/test_shutdown_blocked_io.py` | exclusive (serial after the pool) |
| 21 | 11 | 0.4% | `python3 tests/test_w125_harness.py` | real work |
| 22 | 10 | 0.4% | `python3 tests/test_operator_workflows.py` | real work |
| 23 | 9 | 0.3% | `python3 tests/test_w153_formation.py` | formation waits |
| 24 | 7 | 0.3% | `python3 tests/test_raft_ack_window.py` | real work |
| 25 | 7 | 0.3% | `python3 tests/test_shutdown_stuck_worker.py` | exclusive (serial after the pool) |
| 26 | 6 | 0.2% | `EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta` | C test, real work |
| 27 | 6 | 0.2% | `python3 tests/test_server_processes.py` | exclusive (serial after the pool) |
| 28 | 5 | 0.2% | `./tests/test_kv_lsm` | C test, real work |
| 29 | 5 | 0.2% | `python3 tests/test_orcd_verify_resume.py` | real work |
| 30 | 5 | 0.2% | `./tests/test_append_transaction` | C test, real work |

Three kinds of time, in that order of size:

1. **Serial work inside one long test** that the pool cannot split: the
   payload test's verification of every file written so far (thousands of
   writer files, each payload regenerated by a Python sha256 loop at 6 ms per
   file), the B driver's fsync writes and read-backs one file at a time at
   six cluster sizes, D4's final verification, twelve formations in series.
2. **Fixed waits**: `time.sleep(12)` before three 32 s status calls,
   6 s and 3 s settle sleeps before asserting a status, 1 s start gaps.
3. **Product timers the tests wait on**, unchanged here: the 30 s
   member-down detection (`EFS_HB_DOWN_MS`), the 15 s storage fence a client
   pays at unmount for every member that is down (`session revoked at unmount
   ... rc=-13 ms=15002 phase=storage-fence`), 30 s to close a mount that
   read a lost or corrupt fragment, `efs-mgmt status` taking 32 s while a
   member is frozen, and the deliberate 8/45/65 s delays of B6's slow mode.

The pool itself was the other half of the problem: at `EFS_TEST_JOBS=8` the
old runner started eight daemon tests at once. They use 0.15–0.3 CPU each
(`/usr/bin/time`, children included) but their thirty-odd efsd stores fsync
on one disk: load average 50, every command 5–10x slower (a 6 s C test took
187 s) and six commands failed on deadlines in each of the three baseline
runs.

## What changed

Nothing was relaxed: every command that ran before runs now, with the same
assertions; four tests gained a check (seed loss now also requires the frozen
member to be reported down; the B geometry and K37 polls assert what the
sleep they replace assumed).

- **Runner** (`scripts/make_test_summary.sh`, tested by
  `tests/test_make_test_summary_runner.py`): `tests/budgets.tsv` gives every
  `test-long` command a wall-time budget and the pool **slots** it occupies.
  Slots weigh disk use, not CPU: 6 for the report step-down test (a 3 s
  latency bound that needs a quiet disk; it starts first, with D4, whose
  packer PUT deadlines need the same, as the first two-slot command beside
  it), 5 for a test whose driver fsyncs for most of its run (B growth, stop
  progress: 58–84 s alone, three times that beside another such test, so two
  never overlap), 2 for a daemon test that writes a moderate set, 1 for a
  test that mostly waits on daemon timers or is CPU-bound. The pool starts
  the commands with the most slots first, then the largest budgets, admits a
  command only while its slots are free and skips to the next that fits; a
  command past its budget is listed `OVERBUDGET` in `summary.txt` and
  counted on its first line (verdict unchanged). `timings.tsv` gained the
  budget column; a run starts by removing the previous run's `tmp/fail-*.log`.
  The EXCLUSIVE set is unchanged.
- **Drift check** (`tests/test_make_test_recipe.py`): every `test-long`
  command needs a budget row and no row may be stale; a budget above 300 s
  needs a recorded reason; the gate the budgets add up to (list schedule of
  the pool at 8 slots, largest first, plus the exclusive commands in series,
  `make test` commands at 1 s each) must stay under 900 s. A new or slower
  test must raise its budget in the same change or fail this test.
- **Independent commands** (`Makefile`): B6 runs one mode per command
  (`EFS_B6_MODES=down|refuse|slow|full`, each its own cluster; artifact file
  named per mode); B geometry as three (`EFS_B_PHASES=grow-a`: three members
  with the driver and B2 on the stores, then four and five with the driver;
  `grow-b`: a fresh cluster grown to five without the driver, then six to
  eight with it, so every size is driven once between the two; `full`); the
  D39 model in three shards (`--shard i/3`: every third test and every third
  search seed; together every test and seed once).
- **Inside tests**: `tests/k33_daemons.py` `payload()` is SHAKE-256 in one
  call (was a Python sha256 loop); the payload test reads back through four
  threads while every member is up (one at a time while a member is down, as
  before); the first-catalog test forms four clusters at a time (the 1 s
  start gaps inside each formation stay); seed loss polls the three members'
  tables concurrently until they report the seed, the frozen node and the
  stopped node down; B geometry and K37 poll the status they assert instead
  of sleeping 6 s and 3 s; the B driver samples df against status until they
  agree (bounded by `--df-wait`) instead of sleeping, writes and reads back
  through four threads (`--parallel`, default 4) and uses the SHAKE payload;
  the D4 driver verifies through eight threads.
- **Port reservation** (`tests/k33_daemons.py`): a cluster's ports were
  picked at creation and bound minutes later; in a parallel pool another
  test took one (B geometry node 8: `bind failed ... Address already in
  use`). The helper now holds every reserved port with a bound SO_REUSEADDR
  socket for the life of the cluster; efsd binds beside it.

## Method

```bash
# stage (no git metadata in the copy), build, run; raw output stays outside Git
git ls-files -c -o --exclude-standard -z | COPYFILE_DISABLE=1 tar --null -T - -cf - | ssh nuc 'tar -x -C ~/efs-gatespeed/after'
ssh nuc 'cd ~/efs-gatespeed/after && EFS_TEST_JOBS=8 EFS_TEST_CMD_TIMEOUT=3600 scripts/make_test_summary.sh; head -1 summary.txt'
ssh nuc 'cd ~/efs-gatespeed/after && EFS_TEST_JOBS=1 EFS_TEST_CMD_TIMEOUT=3600 scripts/make_test_summary.sh; head -1 summary.txt'
```

The before tree is merge-wave7 `0b01a2a1e` unchanged, the after tree this
branch; `EFS_TEST_CMD_TIMEOUT=3600` so no baseline command is cut short at
the default 900 s. Each run's `summary.txt`, `timings.tsv`, `build.log` and
`tmp/fail-*.log` are kept under nuc `~/efs-artifacts/gate-speed-20261025/`
(`before-j8-1..3`, `before-j1-1`, `after-j8-1..3`, `after-j1-1`, the
calibration run `cal-j8-1`, the intel1 dry run `intel1-dry/`, `SHA256SUMS`).
The driver is `gate-driver.sh` there (also in the evidence directory).

## Results

All runs: nuc, `scripts/make_test_summary.sh`, `EFS_TEST_CMD_TIMEOUT=3600`,
14 NOTRUN host limits in every run (no RDMA device, no valgrind, no
`~/git/cluster`; one more, the runner's own NOTRUN self-test line, where the
runner test ran). Build time is excluded from the wall time on line 1 of
`summary.txt` and listed separately.

**Before** (merge-wave7 `0b01a2a1e`, old runner; 471 commands):

| Run | Jobs | Wall | Verdict | Failures (all load: deadlines and unmount revokes under 8 daemon tests at once) |
| --- | --- | --- | --- | --- |
| before-j8-1 | 8 | 1,209 s | FAIL 6 | stop_progress (unmount revoke rc=-13), b6_unavailable, d4_progress (fsync EIO), report_stepdown (writer queue timeout), test_reclaim (not drained), test_store_durability (abort) |
| before-j8-2 | 8 | 1,217 s | FAIL 6 | stop_progress, b6_unavailable, d4_progress, report_stepdown, test_reclaim, raft_rotate_stall |
| before-j8-3 | 8 | 1,182 s | FAIL 3 | stop_progress, d4_progress, report_stepdown (write max 33 s, fsync max 40 s > 3 s) |
| before-j1-1 | 1 | 2,754 s | PASS 0 (15 NOTRUN) | serial: no load failures |

The intel2 merged-tree gate this item started from ran serially: 2,641 s
(467 commands, 2 FAIL, 15 NOTRUN).

**Calibration** (this branch while the budgets and slots were being set; not
counted as after-runs; all at 8 jobs):

| Run | Configuration | Wall | Verdict |
| --- | --- | --- | --- |
| cal-j8-1 | first slot table (3/2/1), serial B driver | 964 s | FAIL 2: d4_progress `packer errors: put_errors=1` under load; the recipe test rejecting the placeholder budgets |
| cal2-j8-1 | paced payload writer, parallel B driver, timer-bound tests at one slot | 833 s | FAIL 2: the same |
| cal3-j8-1 | D4 at six slots (starts alone) | 858 s | FAIL 1: report_stepdown `write max 3.34 s > 3.0 s` beside two fsync-heavy tests |
| cal4-j8-1 | report_stepdown at six slots, first, D4 beside it | 1,059 s | **PASS** (0 FAIL): report_stepdown needs 341 s even with a quiet disk, and the three fsync-heavy tests then ran in series behind it (grow 552 s) |
| cal5-j8-1 | the paced payload test beside report_stepdown too; budgets from cal4 | 904 s | **PASS** (0 FAIL): the B growth phase (454 s) started when report_stepdown ended and tailed the run |
| after-j8-1..3 | B growth split in two (grow-a, grow-b), fsync-heavy tests at three slots | 811 s, 839 s, 989 s | PASS, PASS, FAIL 2 (report_stepdown `write max 5.79 s > 3 s`; test_k36_recovery step 11 `unrecoverable=1`, the placement-dependent case noted on the B6/K37 page) |
| after2-j8-1 | payload test at one slot | 926 s | FAIL 1 (stop_progress: client stop refused the unmount while grow-a, grow-b and it ran together) |

The step-down test and D4 assert latency bounds (3 s per append, packer
PUT deadlines) that an fsync-heavy neighbour breaks on the nuc's NVMe, so
they run first beside each other; the paced payload test (two files a
second) is light enough to join them, and the fsync-heavy tests start when
the step-down test ends. After cal5 the B growth phase was split in two
(grow-a 3..5 with B2 on the stores, grow-b 6..8) so no single command
tails the pool; after after2 the three fsync-heavy commands got five slots
each, because paired they ran three times slower than alone and broke the
stop-progress unmount.

**After** (this branch, `4550fdabc`; 478 commands: B6 as four, B geometry
as three, the D39 model as three):

| Run | Jobs | Wall | Verdict | Over budget | Failures |
| --- | --- | --- | --- | --- | --- |
| after-j8-1 (committed table) | 8 | 811 s | INCOMPLETE 0 (14 NOTRUN host limits) | 7 | none |
| after-j8-2 (committed table) | 8 | 839 s | INCOMPLETE 0 (14 NOTRUN host limits) | 6 | none |
| after-j8-3 (committed table) | 8 | 989 s | FAIL 2 (14 NOTRUN host limits) | 6 | python3 tests/test_report_stepdown.py, python3 tests/test_k36_recovery.py |
| after2-j8-1 (payload test at one slot; not committed) | 8 | 926 s | FAIL 1 (14 NOTRUN host limits) | 7 | python3 tests/test_stop_progress.py |
| after3-j8-1 (fsync-heavy tests at five slots; not committed) | 8 | 1,027 s | INCOMPLETE 0 (14 NOTRUN host limits) | 2 | none |
| after-j1-1 (serial) | 1 | 2,311 s | FAIL 1 (15 NOTRUN host limits) | 1 | python3 tests/test_k35_fsync_degraded.py |

The three runs of the committed table: 811 s and 839 s clean, 989 s with
two failures that are not this branch's: the step-down test's 3 s bound
(`write max 5.79 s`) in the first wave where only D4 and the paced payload
test ran beside it, and `test_k36_recovery.py` step 11 (`unrecoverable=1`,
the placement-dependent case recorded on the B6/K37 page). The two later
variants (payload test at one slot; the three fsync-heavy commands at five
slots so no two overlap) were slower, 926 s and 1,027 s, because the
lighter tests beside a lone heavy one still keep the disk busy; they are
not the committed table. The serial after-run failed K35 step 7 once
(`four members, node 4 down: degraded count 15`, a test and daemon path
this branch does not touch; the same step passed in every 8-job run and in
the serial baseline). The budgets in `tests/budgets.tsv` are 1.25 x the
medians of the three committed-table runs; those runs used the previous,
slightly larger budget numbers with the same slots.


**Per command, before and after** (8 jobs; seconds):

| Command (before) | Before, 8 jobs (median of 3) | Before, serial | After, 8 jobs (median of 3) | After, serial | After commands |
| --- | --- | --- | --- | --- | --- |
| `python3 tests/test_b_geometry.py` | 1118 | 381 | 361 | 199 | 3 |
| `python3 tests/test_k33_payload.py` | 808 | 273 | 190 | 162 |  |
| `python3 tests/test_report_stepdown.py` | 671 | 225 | 347 | 239 |  |
| `python3 tests/test_k37_segment_rebuild.py` | 364 | 203 | 266 | 196 |  |
| `python3 tests/test_stop_progress.py` | 349 | 52 | 130 | 66 |  |
| `python3 tests/test_b6_unavailable.py` | 330 | 358 | 425 | 382 | 4 |
| `python3 tests/test_k36_recovery.py` | 255 | 71 | 93 | 73 |  |
| `python3 tests/test_k33_seed_loss.py` | 246 | 222 | 55 | 51 |  |
| `python3 tests/test_k36_segment_recovery.py` | 227 | 67 | 78 | 126 |  |
| `EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta` | 187 | 11 | 31 | 11 |  |
| `python3 tests/test_d4_progress.py` | 178 | 102 | 75 | 77 |  |
| `python3 tests/test_member_limit.py` | 136 | 15 | 20 | 14 |  |
| `python3 tests/test_k33_growth.py` | 134 | 92 | 107 | 100 |  |
| `python3 tests/test_d39_model.py` | 127 | 79 | 99 | 50 | 3 |
| `python3 tests/test_query_first_catalog.py` | 92 | 102 | 26 | 26 |  |
| `./tests/test_append_transaction` | 75 | 7 | 29 | 6 |  |
| `python3 tests/test_k33_interrupt.py` | 70 | 68 | 54 | 69 |  |
| `python3 tests/test_k35_fsync_degraded.py` | 61 | 62 | 75 | 125 |  |
| `./tests/test_kv_lsm` | 60 | 8 | 28 | 8 |  |
| `python3 tests/test_k34_stop_put.py` | 54 | 37 | 41 | 13 |  |
| `./tests/test_reclaim` | 41 | 13 | 24 | 13 |  |
| `python3 tests/test_query_subtree_negative.py` | 40 | 4 | 15 | 4 |  |
| `python3 tests/test_query_sort_peak_negative.py` | 38 | 4 | 12 | 4 |  |
| `python3 tests/test_k33_bookkeeping.py` | 37 | 37 | 50 | 36 |  |
| `EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta` | 34 | 4 | 11 | 4 |  |
| `python3 tests/test_k33_autoid.py` | 32 | 30 | 35 | 31 |  |
| `./tests/test_query_disk_io` | 30 | 2 | 6 | 2 |  |
| `./tests/test_kv_insertion_buffer` | 25 | 2 | 7 | 2 |  |
| `python3 tests/test_k33_client_view.py` | 23 | 20 | 52 | 17 |  |
| `python3 tests/test_operator_workflows.py` | 18 | 12 | 21 | 9 |  |

"Before, 8 jobs" times are inflated by the old pool's overload (a 6 s C test took 187 s); the serial columns compare like with like. The B6 modes add up to more after than before because each mode now forms and restarts its own cluster.


## Findings for follow-up (not fixed here)

- Unmount with a member down costs 15 s per mount (storage fence to the down
  member, `efs_session_storage_barrier`); a mount that read a lost or corrupt
  fragment takes 30 s to close. K37 pays nine fences, K36 segment recovery
  four, B6 several per mode. A client that already holds the member in its
  down-cooldown could skip or shorten that fence.
- `efs-mgmt status` takes 32 s while a member is frozen (SIGSTOP) and 2 s
  while one is stopped.
- `tests/test_k33_payload.py` intermittent EIO on a degraded read (a member
  down, reads one at a time as always) under host load: once on amd with six
  heavy tests at once, once in the intel1 dry run. Known on devel (intel2 at
  `335bfb5a0`, [B6/K37 merge tests](b6-k37-merge-tests-20261024.md)).
  A capture loop on intel1 ran the payload test six times alone with
  `EFS_K33_KEEP=1`: all passed (130–242 s), so the EIO needs the load.
- The old pool at 8 jobs failed six commands per run on the nuc
  (stop_progress unmount revoke, B6, D4 fsync EIO, report step-down writer
  timeout, test_reclaim, test_store_durability / raft_rotate_stall): load,
  not code; the slots keep the pool below that load.
