# A11 finding 2 · REPORT across a metadata leader step-down (Oct 24, 2026)

**Purpose.** Finding 2 of the
[A11 qualification matrix](a11-qualification-matrix-20261024.md): on nuc
loopback-3, `tests/live/append_failover.py` kept 8,000 of 8,000 records
exact, but one REPORT (message type 67) logged `inode-rpc: retry type=67
attempt=11 … recv_ms=30001` under leader step-downs. A stall of that size on
a leader change misses the foreground deadline that alpha needs: C3/C4 and
K15 W52 in [alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md)
(earlier evidence: [K15 live gates](k15-live-gates-20261022.md)). Trace the
stall, fix the cause, bound REPORT latency across a step-down.

**Verdict: FIXED, PASS (scoped: loopback, TCP, one host).** A REPORT now
completes within **3 s per leader change it crosses** (three maximum election
timeouts). Daemon test 5 of 5 PASS, its control (fix disabled) FAIL;
`append_failover.py` with step-downs: 0 REPORT receive timeouts and 0
recovery-scan stalls in 5 of 5 runs, every REPORT within the bound, records
exact; the untouched base FAILs the bound in 2 of 5 (4 of 5 in the previous wave) and leaves transactions to the 5 s recovery scan in 4 of 5. amd `make test-all` at
`04562f14a`: 468 commands, 12 FAIL, all also failing on the untouched base
(13 FAIL there), 6 NOTRUN host limits. Multi-host, RDMA and orcd-mini NOTRUN (parent session).

## What the stalled REPORT waited on

Raw logs: nuc `~/efs-artifacts/a11-20261024/loopback-3-r1/gates/append-failover/harness/run/logs/`
(listed in that run's `SHA256SUMS`, `11ae424f…d4e1`). REPORT id
`4d76a107/1/18e18e35834b77d1:507`, inode 4097, two records:

| Time (UTC) | Where | Line |
| --- | --- | --- |
| 19:34:47.157 | client | REPORT sent (the 8 s sync budget starts; `report-loop … ms=8001` below) |
| 19:34:47.180 | n1 | `group=0 operator step-down term=82` (harness, group 0 then group 2 three seconds later) |
| 19:34:47.588 | n1 | `report resolve ino=4097 rc=-15 (retry)`, `NOT_PRIMARY without hint` |
| 19:34:48.042 – 19:34:54.331 | n1, n2 | 10 more attempts, each answered within 17–436 ms: `report resolve ino=4097 rc=-13 (retry)` (BUSY), receipt `outcome=unknown` |
| 19:34:50.189 | n1 | `group=2 operator step-down` |
| 19:34:54.654 | n1 | `txn-recover shard=1 coord=2841 parts=64 age=7.5s -> ABORT` |
| 19:34:55.158 | client | `retry type=67 attempt=11 why=recv rc=-6 … recv_ms=30001`, `report-loop … busy=1 ms=8001 rc=-13` |
| 19:34:55.215 | n1 | attempt 7 on n1 runs: `outcome=applied ms=84` |
| 19:34:55.308 | client | `report-resolve … outcome=applied … ms=150` (W155 receipt): the REPORT succeeded |

- **No 30 s wait and no hung leader.** `recv_ms` printed the configured
  receive bound (`EFS_IO_TIMEOUT_MS`, 30 s); the last receive was cut by the
  REPORT's 8 s budget (`rpc_past_deadline`). Every attempt was answered
  promptly, by a node that still had the authority to run it.
- **What it waited on:** BUSY from `host_resolve_caught_up` (append
  completion and barrier retirement for the inode) for 7.5 s, until the
  recovery scan aborted transaction `coord=2841`: 64 participants, born at
  the step-down (age 7.5 s at 19:34:54.654). Both a W77 publication unit and
  an append reservation or barrier retirement on a 64-lane file span the 63
  lane shards and the inode shard; the base's lines do not say which it
  was. Its PREPAREs held the inode's append state; the next attempt after
  the abort applied. The client kept retrying BUSY with backoff
  and then resolved the ambiguous attempt through its W155 receipt, as
  designed; nothing was lost or duplicated.
- **Why nothing finished it:** its coordinator proposed DECIDE COMMIT on
  the coordinator group just as that group's leader stepped down and got
  NOT_PRIMARY. In `host_txn_commit` (W77 units, namespace transactions) only
  a BUSY DECIDE was handed to the finisher; any other failure was treated
  as "never proposed, nothing to finish". `host_resize_decide` (append
  reservation, barrier retirement, mtime) did the same for COMMIT by design
  ("COMMIT never converted"); W64 had closed the hole for a failed DECIDE
  ABORT only. A failed DROP after a failed PREPARE also left its records to
  the scan. So the PREPAREs waited for `HOST_REC_AGE_NS` (5 s) plus the
  scan period. On amd both paths were seen: before `host_resize_decide`
  was covered, a fix build that covered only `host_txn_commit` still left
  two transactions born at step-downs to the scan (`fix-r1`).

## Fix (`src/server/raft_host.c`, commit `6654d5f15`)

- `host_txn_commit` and `host_resize_decide`: a DECIDE COMMIT that fails
  with anything but BUSY, PROTO (decided otherwise) or STALE (retired) is
  handed to the finisher as `HOST_FIN_ABORT`, which proposes DECIDE ABORT at
  the group's next leader and RESOLVEs every participant. This is safe
  because DECIDE is decide-once in log order: a COMMIT that survived the
  leader change is ahead of the ABORT, answers it PROTO, and the finisher
  resolves COMMIT; a COMMIT lost with the deposed leader's log is aborted,
  which is what recovery would have decided 5 s later. Line:
  `txn-commit coord=C parts=N decide rc=R (finisher abort)`.
- `host_drop_parts`: a failed DROP hands the transaction off the same way
  (`txn-drop … (finisher abort)`).
- The read-only finish of a BUSY DECIDE now decides ABORT itself when no
  decision appeared within `HOST_FIN_DECIDE_NS`, one maximum election
  timeout (1 s), instead of polling until `HOST_FIN_MAX_NS` (10 s) and
  dropping to recovery (`txn-finish coord=C no decision after N ms`).
- Diagnostics: `report resolve … why=state|authority|inode|complete|catchup|retire`
  names the stage that refused; the client's REPORT receive retry logs
  `wait_ms` and `cut=deadline|bound|conn` next to `recv_ms`
  (`scripts/w155_correlate.py` reads both).
- Exactly-once is unchanged: the hand-off proposes exactly what the recovery
  scan proposes, earlier. A retry of the same publication id or append
  identity meets the decision as before (twin attempts, W155 receipts).
- No KV kind, Raft tag or wire message type (`test_kv_kind_unique.py`
  PASS). New fault hook `EFS_FAULT_DECIDE_LOST` (inside `#if EFS_FAULTS`;
  A6 inventory row added): the next multi-part DECIDE COMMIT steps its
  coordinator-group leader down and is not proposed, one-shot per
  `<file>.ready`.

The brief's suspicion, a deposed leader that never answers or a client that
waits on a connection to a former leader, did not match the trace: no
request was held, and the REPORT path's NOT_PRIMARY hint following and BUSY
retry are unchanged.

## The bound

**A REPORT completes within 3 s of each metadata leader change it crosses.**
Election timeouts are `HOST_ELECT_BASE` 100 ticks of `HOST_TICK_US` 5 ms,
randomized to [0.5, 1) s; a REPORT needs ReadIndex on both groups
(`host_append_authority`), so it waits for the new leader (≤ 1 s), the
finisher's ABORT and RESOLVEs (tens of ms after the election) and the
client's BUSY backoff (50 ms doubling to 800 ms). 3 s is three maximum
election timeouts. The harness's step-down mode issues both groups'
step-downs 3 s apart, so one REPORT can cross two; the harness and the
summary allow 3 s × max(1, n), n = step-downs issued from 3 s before the
REPORT started to its end.

## Tests

| Test | Where | Result |
| --- | --- | --- |
| `tests/test_report_stepdown.py` (new, `make test-long`): three `tests/faults/efsd` daemons, one mount, 4 writers × 400 O_APPEND records with fsync every 8; arm `EFS_FAULT_DECIDE_LOST`, then 6 operator step-downs 4 s apart; checks every write/fsync ≤ 3 s, no REPORT recv retry, no exhausted budget, no RPC `slow-ok` over 3 s, the fault's hand-off and finish, no `txn-recover` resolution, the exact record set | amd, `584ce426a` | 5 of 5 PASS (53–184 s each); write max 1.35–2.09 s, fsync max 0.86–1.97 s, slowest RPC ≤ 1.81 s, 1,600 records exact each |
| same test, fix disabled (`ctl5`: the four hand-off conditions set to 0, fault hook kept) | amd | FAIL as expected: `write max 6.06 s, fsync max 5.90 s > 3.0 s`; the lost DECIDE sat until `txn-recover shard=1 coord=868 parts=2 age=5.4s -> ABORT`. An earlier control (`22e3044ac` tree) reproduced the exact A11 signature: `retry type=67 attempt=7 why=recv rc=-6 … recv_ms=30000`, `report-loop … ms=8000`, `txn-recover … age=9.3s` |
| `tests/test_fin_abort_handoff.py` (W64 unit, extended) | Mac, amd (`make test-all`) | PASS: COMMIT NOT_PRIMARY/IO → `HOST_FIN_ABORT`, BUSY → read-only finish, PROTO/STALE/OK → nothing; the read-only finish decides ABORT after the grace (in 1–3 s), never before it, stops on STALE |
| `tests/test_prepare_cohort.py` (extended) | Mac, amd (`make test-all`) | PASS: `host_txn_commit` DECIDE NOT_PRIMARY → ABORT hand-off, PROTO → none |
| `tests/test_w155_report_resolve.py`, `tests/test_w125_harness.py` (log format, `w155_correlate.py`), `tests/test_append_catchup_host.py` (refusing stage) | amd (`make test-all`); w125 and catchup also Mac | PASS |

## Live reruns: `append_failover.py` with step-downs

Command (each run its own private loopback cluster on free ports 186xx,
TCP, buffered I/O, 8 GiB quota):

```bash
python3 tests/live/append_failover.py --rel REL --root RUN --port PORT \
  --records 8000 --writers 4 --failover step-down --interval 4 --first-event 3
```

The harness and summary are from `584ce426a` for both builds; `REL` is the
base tree (`646a43041`, `merge-wave6`) or the fix tree. Two base and two fix
runs at a time on amd. Table: `python3 scripts/report_stepdown_summary.py RUN…`
(columns trimmed; `per change` = slowest REPORT divided by the leader
changes it crossed).

| Run | Build | Verdict | REPORT recv retries | 8 s budget exhausted | Slowest REPORT | Per change | Over bound | Recovery-scan resolutions | Finisher hand-offs | Records (missing/dup) | Views identical | Step-downs |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: |
| base-c1 | base | PASS | 0 | 0 | 1.01 s | 1.01 s | 0 | 2 | 0 | 8,000 (0/0) | yes | 59 |
| base-c2 | base | PASS | 0 | 0 | 2.93 s | 2.93 s | 0 | 0 | 0 | 8,000 (0/0) | yes | 64 |
| base-c3 | base | FAIL | 0 | 0 | 6.49 s | 3.24 s | 1 | 1 | 0 | 8,000 (0/0) | yes | 50 |
| base-c4 | base | FAIL | 0 | 0 | 4.51 s | 3.46 s | 1 | 5 | 0 | 8,000 (0/0) | yes | 80 |
| base-c5 | base | PASS | 0 | 0 | 3.73 s | 1.86 s | 0 | 2 | 0 | 8,000 (0/0) | yes | 60 |
| fix-c1 | fix | PASS | 0 | 0 | 1.80 s | 1.80 s | 0 | 0 | 3 | 8,000 (0/0) | yes | 56 |
| fix-c2 | fix | PASS | 0 | 0 | 1.57 s | 1.57 s | 0 | 0 | 4 | 8,000 (0/0) | yes | 64 |
| fix-c3 | fix | PASS | 0 | 0 | 1.23 s | 1.23 s | 0 | 0 | 1 | 8,000 (0/0) | yes | 38 |
| fix-c4 | fix | PASS | 0 | 0 | 1.28 s | 1.13 s | 0 | 0 | 1 | 8,000 (0/0) | yes | 43 |
| fix-c5 | fix | PASS | 0 | 0 | 1.68 s | 1.68 s | 0 | 0 | 1 | 8,000 (0/0) | yes | 48 |

Every run: integrity PASS, final fsync 0 within 1.5 s, warm primary, warm
peer and cold views identical. Base: 10 recovery-scan resolutions in 4 of 5
runs (each a transaction left for 5 s or more), 2 runs over the bound.
Fix: none, every REPORT within 1.8 s, 10 DECIDEs lost to a step-down
finished by the finisher. Neither build hit the 8 s budget here; the nuc
run did (finding 2), and the daemon test's control reproduces it on demand.
In the base the stall happens only when a step-down lands on a DECIDE, so
its size varies run to run (up to 7.9 s in the previous wave).

Earlier waves from intermediate commits (same harness command; full table
in the raw evidence): at `6b844619e` (fix code identical to the final),
base 5 runs: slowest REPORT 3.8–7.9 s, 3–7 recovery-scan resolutions each,
4 of 5 over the per-change bound; fix 5 runs: 0 resolutions, 0 over the
bound, worst 2.38 s per change. Six runs at once (`491625f1a`): fix worst
2.88 s per change (a slow log commit while six clusters shared the disk).
Before `host_resize_decide` was covered (`22e3044ac` tree, first wave) the
fix run still showed two recovery-scan aborts born at step-downs, which led
to that part of the fix.

## Tested source and build

- Base: `merge-wave6` = `646a43041`. Fix: commit `6654d5f15`
  (tree `e9358621`), built as `584ce426a` (the same tree before the WIP
  commits were squashed; `EFS_GIT_ID=584ce426a`) for every live and daemon
  run; `04562f14a` adds only a unit-test fix and is the `make test-all`
  commit. This page and the status updates are the commit after it.
- `git archive` trees under `~/efs-rsd/` on amd, `make -j32 all
  tests/faults/efsd`, gcc, x86_64, `-O3 -march=native`. sha256: fix `efsd`
  `cbaa78b9…21e7b`, `efs-fuse` `ea1c6231…8327`, `tests/faults/efsd`
  `cf57ee99…2b83`; base `efsd` `bac40ebc…80a5`, `efs-fuse` `b2c307ba…45d2`
  (base built without git metadata, `build=unknown`).

## Environment

amd (`amd-395`): 32 CPUs, 119 GB, Ubuntu, Python 3.14, `/dev/fuse`. Private
loopback daemons only (ports 18600–18699, three members, both metadata
groups on all three), TCP, buffered I/O. No running cluster, mount, the
`efs` user or ports 17432–17435/27432/27433 were touched.

## `make test-all`

`scripts/make_test_summary.sh` (`EFS_TEST_JOBS=8`, target `test-all`) in
fresh `git archive` trees on amd:

| Tree | Commands | FAIL | NOTRUN | Time |
| --- | ---: | ---: | ---: | ---: |
| fix `04562f14a` (alone) | 468 | 12 | 6 | 757 s |
| fix `6654d5f15` (beside the base run) | 468 | 14 | 6 | 458 s |
| base `646a43041` (beside the fix run) | 467 | 13 | 6 | 400 s |

- At `6654d5f15`, `tests/test_append_catchup_host.py` did not compile: it
  extracts `host_resolve_caught_up`, which now names its refusing stage.
  Fixed in `04562f14a` (declares the thread-local, checks four stages);
  PASS there. The other 13 FAILs at `6654d5f15` are the base's.
- The 12 FAILs at `04562f14a` all fail on the base too: RDMA on `rxe0`
  (`test_rdma_xprt`, seven `test_rdma_segmented` cases,
  `test_declaration_gate`), `tests/faults/test_create_retry_fault.py`
  (30 s timeout), `tests/test_get_integrity.py` (does not compile on the
  merge wave, known since A11) and `tests/test_b6_unavailable.py`. The
  base's 13th, `tests/test_k33_payload.py` (EIO while two runs shared the
  host), passed on the fix run that had amd to itself.
- NOTRUN (host limits, both trees): no `~/git/cluster`, `test_follower_bound`
  needs an `EFS_FAULTS=1` build, one UNFORMED-race case, no valgrind,
  `TMPDIR` not XFS, and the runner's own NOTRUN self-test.
- New and changed tests in the run: `test_report_stepdown.py` PASS (136 s),
  `test_fin_abort_handoff.py`, `test_prepare_cohort.py`,
  `test_append_catchup_host.py`, `test_w155_report_resolve.py`,
  `test_w125_harness.py`, `test_env_inventory.py`, `test_kv_kind_unique.py`
  PASS.

## Raw evidence

amd `~/efs-artifacts/report-stepdown-20261024/` (outside Git): every run's
`harness.log`, `report.json` and daemon/client logs, the daemon-test outputs,
the `make test-all` summaries and timings. `SHA256SUMS` there:
312 files, sha256 `3857346d…41a6d6`
(`INDEX.txt` there names every run directory and its build). The run trees, stores and build trees under `~/efs-rsd/` were
deleted after the summary (method above recreates them).

## Limitations and remaining gates

- Loopback only, one host, TCP, three members. Multi-host, RDMA, 4–5
  members and orcd-mini are NOTRUN (parent session); the A11 Stage 2 matrix
  runs `append-failover` there.
- The DROP hand-off and the 1 s ABORT of a BUSY DECIDE with no decision did
  not occur in any live run; they are covered by the unit tests only.
- A REPORT can still take up to about 2.4 s per leader change on an idle
  host and about 2.9 s with six clusters sharing it: most of that is the
  election and the new leader's catch-up. The bound has little headroom
  under heavy host load.
- `txn-commit … (finisher abort)` is logged for both `host_txn_commit` and
  `host_resize_decide`; the line does not say which kind of transaction it
  was.
