# Task N — K15 / K29 append gates (October 27–28 UTC)

`agent/devel-task-n` (code head `0acc5f302`); final TOTAL 514 FAIL 0 NOTRUN 14, base 514/0/14, initial 514/1/14. `docs/status/k15-k29-append-gates-20261027.md`. Six append gates PASS; setup FAIL retained/fixed. K48 OPEN: 95/96 listing, failed stores kept. Multi-host/RDMA NOTRUN (scope). Product `f98578e697d3` = devel `063154e0e` C/header source; no product change; K48 blocks qualification.

## Verdict

**Requested append gates PASS in single-host TCP scope. Overall release
qualification remains blocked by K48**, an unexplained directory-listing
omission in the initial full gate. Later passing gates do not erase it.

Two W52 20,000-record runs, two W64 8,000-record step-down runs and two
2,000-record lost-reply runs completed. They verified **59,992 acknowledged
records and eight deliberately failed writes / explained zero holes** in
full warm-primary, warm-peer and cold-restart reads. No unexpected write,
fsync or close error, REPORT receive timeout, exhausted REPORT budget,
unexplained hole, missing/duplicated/corrupt/out-of-order record or visible
failed record occurred. All cold transitions and final cleanup were checked
orderly stops. The K29 near-zero-progress stall was not reproduced; slow
progress and placement sensitivity remain measured observations.

The original second lost-reply attempt failed **during setup**, before any
writer existed. It is retained as FAIL. After fixing that harness mistake,
an independently named fresh fixture completed the second lost-reply gate.
This is six completed gates plus one classified failed setup attempt.

## Source, build and environment

- Product source: `f98578e697d3ec910a2aa9543df800a9929342f9`; its C/header
  source equals devel `063154e0e81573d42af788936e0f63f5cf158f9f`. On-host
  comparison of 206 source files also found zero differences. **No product
  source, deadline, quorum, wire/store format or mode switch changed.**
- Normal build: `efsd f98578e697d3 (branch agent/devel-task-n, built
  2026-10-27T22:53:40Z on intel1)`. Separate fault build: same identity,
  built `2026-10-27T22:53:55Z`; `EFS_FAULTS=1` only for lost-reply runs.
  Exact binary hashes are retained for normal, fault and base builds.
- Measurement provenance: initial driver `f98578e697d3`; subsequent cases
  `42e436e21` (both client logs); corrected readiness driver `77556050e`
  / tests `f8ba69f36`; sequential follow-up `2d04cac23`.
- intel1: Intel Core Ultra 7 256V, 8 CPUs, 15 GiB RAM, Rocky Linux 10.2,
  kernel `6.12.0-211.67.1.el10_2.x86_64`, system FUSE 3.16.2, XFS `/home`.
- Three private loopback daemons, `127.0.0.1:19301–19303`, 8 GiB quota
  each, TCP, product O_DIRECT and ordinary background defaults. Writer and
  peer mounts each have their own client session. Each non-fault case has
  four persistent O_APPEND writers sharing one file; lost-reply cases add
  one writer on the peer mount to that same file.
- No unrelated `maik` workload existed at preflight. Two existing
  `efs`-owned production daemons and a client stayed untouched and running
  with their original PIDs. This is one test workload at a time, **not an
  assertion that the machine had no other processes or storage activity**.
  CPU iowait, load and disk counters were sampled every five seconds.

Private work was `/home/maik/efs-taskN-20261027/`; only the open K48 failed
fixture remains there. Closed raw evidence is
`/home/maik/efs-artifacts/taskN-20261027/` on intel1. No raw capture files, payloads or stores
were committed or exported to the Mac.

## Harness findings and corrections

1. **Checked cold restart.** The old helper ignored the client stop result,
   killed clients after 30 seconds and force-killed daemons after 30 seconds
   without checking their exit records. It now requires successful client
   drain/unmount/natural exit and each daemon's orderly exit record with
   durability barriers done. A failed stop remains FAIL with stores kept.
2. **Availability is separate from integrity.** The old overall verdict
   could accept explained real write errors. Accounting still distinguishes
   explained holes from corruption, but every non-injected write/fsync/close
   error now fails availability. Existing synthetic accounting tests and
   added negative controls verify this distinction.
3. **Actual failover and both sessions.** Controller exceptions or absent
   step-downs cannot escape the verdict: every event must succeed and show
   term advancement. Both client logs count toward REPORT latency/timeouts.
   Per-writer progress and full attempted-write latency arrays are retained.
4. **My readiness hardening initially omitted redirects.** Requiring
   `status=0` exposed a mistake in my new probe: it did not follow the seed's
   valid `status=7 primary=2`. Both groups had formed on node 2, but
   `w64-lost-2` waited to its existing readiness limit and failed before
   creating any writer or payload. The corrected helper follows exactly one
   configured hint; tests cover direct success, bad hints and a second
   redirect. It changes neither application retry policy nor deadlines.
   The original FAIL and its full log remain alongside `w64-lost-2-ready1`.
5. **Controller exit status.** The original controller's exit alone was not
   its aggregate verdict. Canonical per-command reports/summaries remain
   authoritative. The committed future controller (`4a9a4b1a0`) propagates
   FAIL versus host-limit INCOMPLETE; it does not relabel the original run.

## Live results

All times below are seconds. p50/p99 are ranges across individual writers;
latencies include attempted writes, including deliberate failures. Raw arrays
permit exact recomputation. Writer-loop wall includes final fsync/control
completion, while whole-command wall also includes full verification and
checked restart/cleanup.

| Case | Ack / deliberate failures | Writer-loop wall | p50 range | p99 range | Final fsync max | Events / holes |
|---|---:|---:|---|---|---:|---|
| W52-1 | 20,000 / 0 | 2,878.014 | 0.5556–0.5571 | 0.9580–0.9649 | 0.6765 | 0 / 0 |
| W52-2 | 20,000 / 0 | 554.108 | 0.0898–0.0900 | 0.2872–0.2885 | 0.1183 | 0 / 0 |
| W64 step-down 1 | 8,000 / 0 | 644.312 | 0.0931–0.0932 | 2.0474–2.1428 | 1.1241 | 128 / 0 |
| W64 step-down 2 | 8,000 / 0 | 700.529 | 0.0944–0.0946 | 2.1094–2.3049 | 1.3044 | 139 / 0 |
| Lost reply 1 | 1,996 / 4 | 228.927 | 0.0683–0.4846 | 1.1187–4.6495 | 0.4675 | 4 injections / 4 |
| Lost reply 2, corrected setup | 1,996 / 4 | 44.328 | 0.0149–0.0783 | 0.6356–0.6835 | 0.1138 | 4 injections / 4 |

**All six PASS**, including every exact record in all three views. Non-fault
files are 81,920,000 B (20k) or 32,768,000 B (8k); lost-reply files are
8,192,000 B including exactly four explained zero blocks. Each injected
loss was abandoned successfully with `rc=0 retired=1`; no BUSY abandon or
additional failure occurred, and each peer writer completed all 400 records.

REPORT receive timeouts and exhausted REPORT budgets were zero in every
case. The longest logged successful REPORTs were 2.071 s and 2.436 s in
the step-down cases, below the unchanged per-leader-change bound. Other
cases had no REPORT at or above the one-second slow-success logging
threshold: `report_max_ms=0` is **censored logging, not zero RPC latency**.
The slowest whole write was 6.023 s during step-down 2; that is a different
measurement from one REPORT's latency. No bound was increased.

The 20k case means 20,000 application records across four writers, **not
one REPORT carrying 20,000 reservations**. The production client publishes
in bounded batches/time windows and on foreign-writer handoff. Per-REPORT
reservation count was not instrumented; this qualifies the specified live
workload, not an unbounded synthetic completion transaction.

### K29 and placement sensitivity

W52-1's leaders were split (group 0 node 1, group 2 node 3); W52-2 elected
both on node 1. Their write-loop walls differ by **5.19×** despite identical
workload/settings/source. First-run apply waits commonly logged 45–55 ms.
This is correlation, **not a controlled placement A/B or a proven cause**.
These timings describe one contended shared inode; general metadata
scalability was not measured. No leader was forced into co-location and no
balance switch was restored.
Further performance work should trace cross-group waits rather than hide
this cost with a placement mode.

Mean/max host iowait over each entire case (including verification): W52-1
3.270/6.418%, W52-2 12.956/22.802%, step-down 1 5.599/15.532%, step-down 2
5.304/18.576%, lost 1 3.993/6.226%, corrected lost 2 11.912/25.259%.
Neither 20k run had K29's near-zero progress for minutes or application
errors. This supports the existing K29 fix in this scope; it does not
establish global performance acceptance or eliminate all stall causes.

## Full gates and open K48

| Tree / run | Exact summary |
|---|---|
| Initial Task N | `TOTAL 514 FAIL 1 NOTRUN 14 STATUS FAIL (723s) OVERBUDGET 5 BUILD 19s` |
| Untouched base `063154e0e`, same host / 8 jobs | `TOTAL 514 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (700s) OVERBUDGET 1 BUILD 30s` |
| Final Task N with corrected measurement/tests | `TOTAL 514 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (727s) OVERBUDGET 1 BUILD 0s` |

Initial failure: index 475, `python3 tests/test_w130_dir_fault.py`.
The preceding W165 serve-first halt/CREATE/resync scenario passed. In the
W130 scenario, node 3 halted group 0 about six seconds in; all 96 target
writes/fsyncs and an unrelated control read succeeded, but a listing through
the healthy quorum returned **95 names**, failing `partial listing 95 != 96`.

**K48 is OPEN, blocks alpha: yes.** Root cause is not established. No missing
payload or permanent data loss has been demonstrated. The unchanged base
and final gate did not reproduce it, so this is an unresolved intermittent
correctness candidate, not a demonstrated Task N product regression and
not a dismissed host flake. No listing assertion was relaxed, no wait for
migration completion was added, and W130 stays in the ordinary full gate.
The later passing invocations do not close K48 or erase the initial FAIL.

The same 14 NOTRUNs cover absent cluster lifecycle scripts, no usable RDMA
device (declaration frame, transport and seven segmented modes), missing
Valgrind/header, the fault-only stalled-compactor variant and unavailable
sanitizer runtime. Leak/sanitizer/RDMA acceptance is not claimed. Final
OVERBUDGET is K22 leader admission, 42.4 s versus its 30 s budget; total
727 s is below D44's 1,100 s target. No budget/target changed.

## Reproduction

Use a quiet test allocation on intel1, FUSE access, free loopback endpoints,
and new work/evidence directories. Never use the production 17xxx ports,
`/data1/efs`, or another agent's tree. Build on the host, not on the Mac.

The corrected current method is the committed
`scripts/task_n_append_gates.py`: export a current Task N commit into
`WORK/plain` and `WORK/fault`, then run detached with all descriptors closed
or redirected:

```sh
python3 scripts/task_n_append_gates.py --work WORK --out EVIDENCE --source COMMIT
```

It builds normal/fault binaries independently with eight jobs, then runs
these sequentially, twice each, each with fresh `--root`, absolute `--rel`,
`--port 19300` and unique label:

```sh
python3 tests/live/append_failover.py --rel BUILD --root NEW_ROOT --port 19300 --records 20000 --writers 4
python3 tests/live/append_failover.py --rel BUILD --root NEW_ROOT --port 19300 --records 8000 --writers 4 --failover step-down --interval 4 --first-event 3
python3 tests/live/append_failover.py --rel FAULT_BUILD --root NEW_ROOT --port 19300 --records 2000 --writers 4 --peer-writers 1 --lose-reserve-every 97 --lose-reserve-max 4
```

The 30 s final-fsync bound, zero REPORT receive-timeout criterion and 3 s
per-leader-change REPORT bound stay unchanged. Exact keyed 4 KiB records,
per-writer ordering, no visible failed record and only proven holes remain
mandatory. Gates use full reads, never samples.

Original execution used source `f98578e697d3` plus the separately retained
measurement overlays described above. After the classified setup error,
`scripts/task_n_followup.py` (`2d04cac23`) retained the initial full gate,
ran the corrected second lost-reply fixture, an untouched base full gate,
then the final full gate, without overlap. Its two staging files can be
recreated with `git show 77556050e:tests/live/append_failover.py` and
`git show f8ba69f36:tests/test_append_failover_accounting.py`; no private
capture is a reproduction prerequisite. The exact command manifests and
source/binary hashes are retained. Code gates use
`EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 scripts/make_test_summary.sh`.

K48's reproduction is the unchanged pooled full gate's W130 test. A
standalone deterministic reproduction and root-cause fix remain NOTRUN.
Multi-host/RDMA, ORCD and SIGKILL/combined lost-reply-plus-failover append
acceptance remain NOTRUN under Task N's scope; W52 historical performance
A/B is not supplied by these current-source repetitions.

## Retention, cleanup and handoff

Closed evidence: **intel1 `/home/maik/efs-artifacts/taskN-20261027/`**.
The verified `SHA256SUMS` covers **212 files**; its SHA256 is:

`3c3436f691a9849b6ff13d23cb07a3057c3b9988f1c65f5785ccace3c9665700`.

`initial-gate/`, `base-gate/` and `final-gate/` retain independent original
verdicts, logs, timings and command inventories. Per-case full reports,
client/daemon/clean-stop logs, raw attempted-write latencies, source/build
identities, both controller manifests, hardware/I/O samples and
`qualification-record.json` remain. Original FAILs are not overwritten.
The verification receipt is outside the closed tree:
`/home/maik/efs-artifacts/taskN-20261027-manifest-verify.log`.

**Keep the open K48 stores** at
`/home/maik/efs-taskN-20261027/open-w130-initial/efs-w130-7jltagai`.
Before any subsequent gate reused its original `plain/tmp/case-475` scratch,
all **677 files / 55,139,383 bytes** were copied and checked by full SHA256.
`open-w130-initial-files.json` records the original path and every file hash;
`OPEN-STORES.json` states the open investigation. These stores are a closed
forensic copy, not a running cluster. Their original endpoints/storage paths
are in the logs/manifests; inspect a working copy when investigating.

`scripts/task_n_retain.py` (`040901fd6`) checked no private owner/mount
remained, copied and hashed every retained source/result, kept this open
fixture and used `edelete` to prune only completed synthetic stores/payloads,
normal/fault/base source/build trees and other gate scratch. `copy-check.json`,
`PRUNED.txt`, `prune.log` and `final-state.json` record the guards and cleanup.
No runtime WAL/store bulk is copied into the closed evidence tree or Git.
The two production daemons/client remain running with their preflight PIDs;
intel1 is released from Task N compute/mounts, with only passive K48 stores
kept. No devops/cluster repository or user cluster was changed.

The parent can pick up `agent/devel-task-n`; it still owes merged-tree and
real-cluster acceptance. K15's requested append scope passed; K29's stall
was not seen here; **overall qualification is not a clean PASS while K48
remains open**.
