# Task N — quiet-host K15 / K29 append gates (October 27)

`agent/devel-task-n`, code `040901fd6`; product `f98578e697d3` = devel `063154e0e` src/include. Initial TOTAL 514 FAIL 1 NOTRUN 14; base/final pending. `docs/status/k15-k29-append-gates-20261027.md`. Six completed append gates PASS; original lost-2 setup FAIL retained/fixed. K48 OPEN: healthy-quorum listing 95/96 after one replica halt; stores kept. Multi-host/RDMA NOTRUN.

## Purpose and current verdict

**IN PROGRESS, no acceptance PASS claimed.** K15 (W52 20,000-record
REPORT bound and W64 append replay/lost reservation replies) and K29
(no-failover append stall) need complete, sequential live runs after the
merged fixes. The previous intel2 run was concurrent with two other
clusters and failed under a no-failover stall. See
[K15 historical gates](k15-live-gates-20261022.md),
[K29 implementation](k29-append-stall-20261023.md) and
[append accounting](k15-k29-accounting-rule-20261024.md).

The assigned host is intel1, Rocky/RHEL-family Linux x86_64,
8 CPUs, 15 GiB RAM, system FUSE 3.16.2. No unrelated `maik` workload was
running at the preflight. Two existing `efs`-owned production daemons and
a client remain untouched: this is an isolated task workload, not a claim
that the whole machine has no other processes or storage activity.
Their initial commands/load are recorded in `environment.txt`; CPU iowait,
load and disk counters are sampled every five seconds during each stage.
A host-load observation never exempts a product failure from the verdict.

Private trees: `/home/maik/efs-taskN-20261027/{plain,fault,runs}`.
Raw evidence: `/home/maik/efs-artifacts/taskN-20261027/`.
Three loopback daemons use `127.0.0.1:19301–19303`, 8 GiB quota each,
TCP, product O_DIRECT and ordinary default background behavior. One writer
mount and one peer mount use unique fixture paths for each case.
The plain build has no fault hooks; the separate fault build uses
`EFS_FAULTS=1` only for the injected lost-reply gate. Both identify source
`f98578e697d3`; source `src/` and `include/` equal devel `063154e0e`.

## Harness findings corrected before relying on the gate

1. **Cold verification did not prove a clean stop.** The old harness
   ignored the client stop result, killed it after 30 seconds, and killed
   a daemon after 30 seconds without checking its exit record. It could
   label reads “cold” after an unknown/forced shutdown. The harness now
   checks client drain/unmount/natural exit and each daemon's orderly exit
   record, including durability barriers. Failure remains FAIL with stores
   retained; no force-kill manufactures a clean restart.
2. **Accounting was being used as availability acceptance.** Explained
   real failed writes did not fail the overall verdict. Integrity stays
   separately reported, but any non-injected write/fsync/close error now
   fails availability. Existing accounting tests plus negative controls
   cover this distinction.
3. **Failover execution and peer evidence.** Controller exceptions or no
   actual step-down could escape the old verdict, and REPORT timing only
   counted the primary writer mount. The updated gate requires successful
   events and observed election-term advance; both active client logs count
   toward REPORT latency/timeouts. Per-writer progress and raw latencies
   remain outside Git for diagnosing stalls.

No product timeout, retry budget, quorum, wire/store format or switch changed.
The initial W52 run loaded driver `f98578e697d3`. Later runs use the
measurement-only peer-log correction `42e436e21`, retained alongside the
original driver. Product binaries do not change between these measurements.

## Committed reproduction

After verifying the assigned host has no unrelated test workload:

```sh
git archive f98578e697d3 | ssh intel1 'mkdir -p ~/efs-taskN-20261027/plain ~/efs-taskN-20261027/fault ~/efs-taskN-20261027/runs; tar -xf - -C ~/efs-taskN-20261027/plain; cp -a ~/efs-taskN-20261027/plain/. ~/efs-taskN-20261027/fault/'
# Run the current committed measurement driver; retain its source separately
# when overlaying it onto a previously built product source.
python3 scripts/task_n_append_gates.py \
  --work /home/maik/efs-taskN-20261027 \
  --out /home/maik/efs-artifacts/taskN-20261027 \
  --source f98578e697d3
```

Run the controller on intel1 in detached `screen`, redirecting every file
descriptor to the external evidence path. It builds plain and fault trees
independently with eight jobs, then runs these **in order, twice each**:

- W52: `append_failover.py --records 20000 --writers 4`, no faults.
- W64: `--records 8000 --writers 4 --failover step-down --interval 4
  --first-event 3`.
- Lost reply: fault build, `--records 2000 --writers 4 --peer-writers 1
  --lose-reserve-every 97 --lose-reserve-max 4`, as the K15 page's gate.

Every command also names its fresh fixture `--root`, absolute `--rel`,
`--port 19300` and unique label. Records are uniquely keyed 4 KiB blocks;
warm primary, warm peer and fresh-client/whole-cluster-restart reads must
match every acknowledged record exactly once, preserve writer order and
have only proven holes. The 30-second final-fsync bound, zero REPORT
receive-timeout requirement and 3-second-per-leader-change REPORT bound
remain unchanged. Each case is attempted once, not retried into PASS.

The final `make test-all` uses `EFS_TEST_JOBS=8` through
`scripts/make_test_summary.sh`. Its actual private scratch is `plain/tmp`;
retain `fail-*.log` there along with summary/timings/build output. A failing
command needs an unchanged-base comparison before being labelled a host
condition. Multi-host, RDMA and orcd-mini append acceptance remain NOTRUN
under Task N's explicit scope; the running user clusters are not touched.

## Results, retention and cleanup

First completed case (the remaining five live cases and full gate are still
pending):

| Case | Verdict | Writes / failures | Writer-loop wall | Final fsync max | Per-writer p50 / p99 | Exact views |
|---|---|---|---:|---:|---|---|
| W52-1, split leaders (g0 node 1 / g2 node 3) | PASS | 20,000 / 0 | 2,878.0 s | 0.6765 s | p50 0.5556–0.5571 s; p99 0.9580–0.9649 s | All three full 81,920,000-byte views identical, zero holes/missing/duplicates/order errors. |

W52-1 had zero REPORT receive timeouts, exhausted REPORT budgets or abandon
failures. No REPORT crossed the one-second slow-success logging threshold;
`report_max_ms=0` is a censored log-derived field, **not zero RPC latency**.
The whole command including verification and checked restart/cleanup took
3,225.7 s. Every stop was orderly. Mean sampled host iowait was 3.270%,
maximum 6.418%. This is one complete positive run, not complete K15 acceptance.

W52-2 elected both groups on node 1 and is progressing substantially faster.
Leader placement correlates with the variation; no settings were changed,
no controlled placement A/B has run, and no causal or performance acceptance
claim follows from the correlation alone.

`commands.json`, per-case reports/logs, per-record latency files,
`host-samples.jsonl` and build identities are authoritative live progress.
Closed evidence will receive a verified SHA256 manifest before private
stores, payloads and build bulk are pruned with `edelete`; open failures
remain preserved until classified. No raw payload/log is committed here.

## New gate failure — K48, W130 partial enumeration

The initial full gate finished **TOTAL 514 FAIL 1 NOTRUN 14 STATUS FAIL
(723s) OVERBUDGET 5 BUILD 19s**. Its only failing command was
`python3 tests/test_w130_dir_fault.py` (index 475). The preceding W165
serve-first case passed its halt, quorum CREATEs and snapshot resync checks.
In the W130 case, node 3 halted group 0 about six seconds in; all 96 target
file writes/fsyncs and the unrelated control read succeeded, but the
healthy-quorum listing returned **95 names**, triggering the unchanged
`partial listing 95 != 96` assertion.

This is **K48, an OPEN correctness candidate, blocks alpha: yes**. It is
not an append failure, not evidence of missing payloads, and not yet a
proven permanent-loss defect. The base and final comparisons are running.
No test was loosened, removed from the pool, delayed until migration
finishes, or retried into a renamed PASS. A later passing invocation does
not erase this observed omission or close K48 without an explanation.

Original closed fixture:
`/home/maik/efs-taskN-20261027/plain/tmp/case-475/efs-w130-7jltagai`.
Before another code gate could reuse `case-475`, all **677 files / 55,139,383
bytes** were copied and checked by full SHA256 into the on-host open
investigation copy:
`/home/maik/efs-taskN-20261027/open-w130-initial/efs-w130-7jltagai`.
`open-w130-initial-files.json` records the original location and every copied
file hash. Keep these failed stores until K48 is classified; no original
failed store is copied to the Mac or committed to Git.

## Corrected setup failure, preserved separately

The original `w64-lost-2` attempt ended **FAIL before any writer or payload
was created**: its root-readiness probe received `status=7 primary=2` and
never followed node 2, where both groups had elected their leader. This is
a harness readiness defect, not a failure to form. Driver `f8ba69f36`
follows exactly one valid configured hint and tests invalid hints, a second
redirect and direct success. It changes no product deadline or behavior.
The original FAIL report/logs remain. An independent fresh fixture
`w64-lost-2-ready1` completed PASS: 1,996 acknowledged records, exactly four
injected failures/holes, successful abandons, full exact warm/peer/cold
reads and no additional failed write or REPORT receive timeout.

`scripts/task_n_followup.py` (`2d04cac23`) preserves the initial gate,
performs that corrected case, then runs an untouched `063154e0e` full gate
and the final branch full gate sequentially. The active product build stays
`f98578e697d3`; measurement changes are `77556050e`. All canonical initial,
base and final summaries remain in separate evidence directories.
