# AB and AE: Raft pump logging, task BI — October 30, 2026

**AB: PASS in executed scope (amd). AE: stays OPEN, with a written disposition.**
Linked items: AB and AE in the Stage 1 observability group of the
[checklist](alpha-1.0-2plus1-checklist.md); K72/K73 (wave 39); new K79 (open) and
K80 (fixed). Tested source: branch `claude/bi-ab-ae-5b1f22`, base devel
`8b0d128cc`; the tested commit is recorded in the Results section.

## What was left after wave 39

Wave 39 already ported the AE branch's bounded reporter (`src/common/telemetry.c`,
K72): the pump captures a fixed-size numeric `struct host_periodic` under
`h->mu` and the outbox owner lock, submits it after unlocking, and the one
reporter thread renders `raft-obs`, I/O, KV and batch records. That is the
single AB/AE queue (4 slots, periodic records coalesce, every drop is counted in
a `telemetry: overflow=N sink_errors=M (cumulative)` line). AB's original
`host_obs_dump`-under-`h->mu` defect was therefore already gone on devel
`8b0d128cc`. Two lock-held writers remained, found by reading `host_pump` and
by the gate below:

1. `host_obs_roles` printed every term/role change with `fprintf(stderr)` while
   holding `h->mu` (the AE branch kept them "synchronous" on purpose).
2. The applier (it runs on the pump under `h->mu`) logged every session event
   except ACK with the synchronous critical path. `SESS_BEGIN` is one of them,
   and it is the first step of a client unmount's session revoke.

## Change (AB)

- `host_obs_roles` now only fills a bounded array (one slot per group) of
  numeric `struct host_role_ev` under `h->mu`; `host_obs_roles_report` formats and
  submits them after the unlock through `efs_telemetry_printf`, in group order and
  before that cycle's periodic record. Event identity (group, old/new term,
  old/new role, leader, commit, applied, timestamp captured under the lock) is
  unchanged. A full mailbox is counted by the reporter's overflow line, never
  silent; the role state itself (`g->obs_term/obs_role`) still advances, so the
  next change is reported relative to the real previous state.
- `apply_session_cmd` sends all session events, fence and failure lines included,
  through `efs_telemetry_log(0, ...)`. Before, `SESS_BEGIN` and the
  `apply session rc=` failure line wrote to a blocked pipe from the pump.
- No new product switch, wire or store format, and no second queue.

Evidence value of the change: with the sink blocked, the checked unmount failed
`revoke rc=-13 phase=begin` (BUSY after 402 ms) before the change, on both a
35 s and a 15 s window, and succeeds after it. That is K80.

## AE branch audit

`agent/devel-task-ae` (28 commits above base `714d7e6fc`) was read through git
objects only. `git patch-id` against the 135 devel commits touching the same
files finds **0 identical patches**: wave 39 took the branch as one squashed
3-way port (`68303f40f`), so no commit is individually in devel. Contents were
checked instead against the port page ([AE port](ae-product-port-20261030.md))
and spot checks in the tree (reporter, `efs_telemetry_log` call sites in
`rdma.c`/`packer.c`/`cluster.c`, atomic `rj.recovered`, `tcp_counts.h`,
`*_to(FILE *)` dumps all present).

| Commits | Disposition |
| --- | --- |
| c5f031c7f 06ad89231 6ace68613 82da821ab 89bcca216 0cc3e8761 19a646fd7 38ab684a3 65bd82f6f 327e25b18 10654cf73 80ed75c34 | **Already in devel** (wave 39, K72): reporter, shared timestamp state, wait diagnostics off the wake mutex, RDMA/packer/session-ACK routing, startup refusal, FIN accounting, test follow-ups |
| 6130bafa1 | **Already in devel** as K73 (`rj.recovered` atomic) |
| e32355568 0d1a9f686 9d7e93ef2 a2fe895c5 047bd8594 363a93498 0bb702207 d05ae96d4 370bc94c4 | **Already in devel** (measurement scripts and harness) |
| 4289216a3 3825ccb94 7359b40ad ae7867b82 | **Evidence only**, ported as pages; their checklist/architecture edits were not applied. **Dropped** here: superseded by this page |
| d904e6a59 bb57ef69b | **Drop**: fixture races for `dir-serve-corrupt`, which devel redesigned (staged, DIR held); not part of AB/AE and not a product defect |

Nothing is left to port. Undecided: none.

## Disposition

- **AB: ticked.** The pump no longer formats or writes diagnostics under `h->mu`
  (periodic: wave 39; term/role and session events: this change). Gate below.
- **AE: left open.** Gate item "clean shutdown with the sink blocked" fails:
  with stderr a pipe nobody reads, a SIGTERM'd `efsd` does not exit within 60 s.
  Threads other than the pump (heartbeat, GC, report paths) are blocked in
  synchronous `fprintf(stderr)` on the stdio lock and stop joins them. About 230
  `fprintf(stderr)` call sites remain in `raft_host.c`. Moving them all to the
  4-slot reporter would drop evidence on healthy sinks too, so the fix needs a
  design decision (separate critical-event ring with a bounded, interruptible
  stop, or a non-blocking stderr for the daemon). Registered as **K79**, OPEN,
  proposed not alpha-blocking (needs a sink that stays blocked through shutdown),
  reproduction `pump_logging_profile.py --sink blocked --stop-blocked`.
  Critical-event retention under an overflowing mailbox (events are counted, not
  kept) is part of the same decision.

## Gate and results

### Reproduction

```sh
make -j12 all tests/faults/efsd tests            # amd, gcc 15.2, -O3 -march=native
TMPDIR=$HOME/work/tmp python3 tests/test_pump_logging.py          # the gate test
sh scripts/ab_hold_matrix.sh BASE_TREE NEW_TREE OUT 3 45          # hold/wait matrix
python3 scripts/pump_logging_profile.py --work W --output O --sink blocked \
  --stop-blocked --obs 1 --seconds 15 --threads 2 --rate 2 --versioned  # K79, expected FAIL
EFS_SAN_ONLY="telemetry pump_logging" bash tests/sanitizer_check.sh tsan
EFS_SAN_ONLY="pump_logging" bash tests/sanitizer_check.sh asan
```

`TMPDIR` must be under `$HOME`; output paths must be new. For the matrix both trees
are archived non-Git copies (`git archive`) that went through
`scripts/pump_logging_measure.py instrument`, which adds the same two histograms
to both: actual `h->mu` hold through unlock, and the active pump cycle. `wait` is
the existing `pump_lock_us` histogram (time to acquire `h->mu`).

### Gate test (`tests/test_pump_logging.py`, `make test-long`, budget 85 s)

Fresh TCP/O_DIRECT three-member cluster, FUSE mount, two fixed-rate rewriting
writers and a 10 Hz stat probe (concurrent proposals and status/metadata
requests), 10 ms serialized fsync interposer, every daemon's stderr a 4 KiB pipe
that nobody reads. Phase 1 (25 s blocked window): every operation succeeds, no
5 s window without progress, exact readback, positive overflow accounting on every
node after recovery, whole-record `raft-obs` role lines on every node,
`iostats`/`batch-obs` records present, checked unmount. Phase 2 (10 s) keeps the
sink blocked through the checked unmount (session revoke through Raft), then drains
and stops. Plus the startup-refusal negative control (an unsupported stderr sink
must refuse startup). Without the K80 change phase 2 fails (`revoke rc=-13`).

- amd: PASS on 35 s/15 s windows (88 s) and again on the final windows.
- nuc, 8-job `make test-all`: the test FAILED once, `foreground failures 4`
  (two write `EIO`, two payload mismatches that follow from them), with one
  daemon `fsync-split flush_ms=3261` in the log. This is the AE branch's known
  "K72 foreground EIO under full-gate load" residue; three reruns of the test
  alone on idle nuc PASS (about 71 s each). Treated as load-dependent and
  **not closed**: it is part of the open AE disposition above (remaining
  synchronous writers and disk-bound fsync under 8 jobs), not proven to be one
  or the other.

### `h->mu` hold/wait and progress (amd, 3 runs each, medians [min, max])

Same host, same flags, 45 s of four fixed-rate (5 ops/s) writers per run, file
sink, `EFS_RAFT_OBS` = 0 or 1. Base is devel `8b0d128cc`, candidate is
`b753281fb`'s `raft_host.c`. Other tenants' daemons were running on amd
(load average 0.75 before, 2.7 after), so maxima are noisy.

| Metric | base OBS=0 | base OBS=1 | candidate OBS=0 | candidate OBS=1 |
| --- | --- | --- | --- | --- |
| Writes/s (acknowledged) | 20 [20, 20] | 20 [20, 20] | 20 [20, 20] | 20 [20, 20] |
| Raft applied entries, cluster sum | 25077 [24850, 30386] | 24891 [24783, 30510] | 24922 [24726, 24969] | 24912 [24792, 24939] |
| `h->mu` hold mean, µs | 56.5 [36.6, 57.9] | 62.1 [37.6, 66.7] | 59.8 [50.8, 63.9] | 52.2 [51.3, 71.2] |
| `h->mu` hold max, µs | 28786 [27034, 54690] | 34507 [32275, 34624] | 35226 [5821, 146519] | **8234** [7496, 34825] |
| `h->mu` wait mean, µs | 0.6 [0.5, 1.0] | 0.6 [0.6, 0.7] | 0.8 [0.6, 2.4] | 0.7 [0.7, 0.8] |
| `h->mu` wait max, µs | 3171 [2586, 41343] | 2697 [938, 3019] | 4772 [1384, 202982] | 2639 [2183, 13561] |
| stat p99, ms | 33.1 [25.6, 50.0] | 3.3 [2.3, 52.9] | 3.3 [2.7, 27.0] | 2.3 [2.3, 51.2] |
| Daemon CPU, s | 9.0 [8.4, 14.4] | 8.9 [8.5, 13.6] | 8.3 [8.2, 8.7] | 8.3 [8.1, 8.4] |

All 12 runs PASS (exact readback, no failed or late write). Reading: Raft
progress (acknowledged writes and applied entries) is the same with diagnostics on
and off and before and after. Mean hold and wait are unchanged within spread.
The one visible effect is the OBS=1 hold maximum, 34.5 ms to 8.2 ms median, which
is the role-change print leaving the critical section; with OBS=0 nothing was
printed under the lock before either, and the OBS=0 maxima are dominated by
unrelated noise (one 146 ms candidate outlier). This does not show a throughput
gain, and at this offered rate nothing could. The blocked-sink effect is the
gate test above (devel fails it; the fix passes), not this table.

### Sanitizers (amd, final tree)

- TSan (`detect_deadlocks=0`): `telemetry` PASS, `pump_logging` PASS (74 s);
  0 reports.
- ASan+UBSan+LSan (baseline ISA `-march=x86-64-v3`): `pump_logging` PASS (67 s).
  Three earlier attempts found, in order: the sanitizer build was broken on devel
  (`test_child_vec_index` lost its `--wrap` flags under the gate's `LDFLAGS`
  override; fixed with `override`), the slow-fsync `LD_PRELOAD` ahead of libasan
  (`verify_asan_link_order=0`), and a LeakSanitizer report on the expected
  startup-refusal exit (`h->fin` and storage roots are not freed when `efsd`
  refuses to start; the negative control now runs with `detect_leaks=0`).
  The refusal-exit leaks are real but harmless (process exits); not registered.

### Full gate

GATE_PLACEHOLDER

### Evidence locations

Raw outputs stay on the hosts and are not committed.

- amd `~/efs-artifacts/taskBI-20261030/` (matrix, controls, sanitizer logs, both
  builds' logs; 237 files, `SHA256SUMS` sha256 `fdf472621f02b9a2bb6fae3d8d2c9f43f521e33f17429be1107a756a6acd7f7c`).
- nuc `~/efs-artifacts/taskBI-20261030/gate/` (`run.log`, `gate1-summary.txt`, failure
  logs, reruns; `SHA256SUMS` hash in the gate section above).

## Limits and remaining gates

- The hold/wait matrix is a small fixed-rate workload on a shared host: it cannot
  show a capacity change and its maxima are noisy.
- No RDMA, no real HCA, no multi-host run. The same synchronous-writer inventory
  (about 230 `fprintf(stderr)` sites in `raft_host.c`, plus `cluster.c`, `store.c`,
  `efsd.c`) was not audited site by site; K79 owns it.
- TSan covers the test above and `test_telemetry`; the rest of the TSan list was
  not rerun for this change.
