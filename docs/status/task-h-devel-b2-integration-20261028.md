# Task H repair integrated with devel b2d57e421 — October 28, 2026

> Migrated from `agent/task-h-repair` at `cd822e2f7` on Oct 30, 2026, by Task BO. Nothing was shortened: the page already held conclusions, methods, hashes and raw-capture locations only. Scripts and tests it names in backticks exist only on that branch; see [the migration page](task-h-evidence-migration-20261030.md).

**Current reconciliation:** [devel wave 33 and the per-fix inventory](task-h-wave33-reconciliation-20261029.md) supersede this page's branch-head and acceptance recommendation; all evidence below remains historical.

Branch: `agent/task-h-repair`. Target: `b2d57e421` exactly. Previous private
head: `e7b2b4ae9`. **Integration complete; historical baseline qualification FAIL.** The
integration merges devel into this branch; it does not merge this branch
back into devel and nothing is pushed. Historical private passes in
[the repair record](task-h-publication-repair-20261026.md) do not qualify
the integrated source.

Current repaired qualification is recorded in
[the K38/B6 continuation](task-h-k38-b6-repair-20261028.md): complete retained-output
`test-all` PASS at `fdfda8cfa`, with the original K33 cause still OPEN and
historical failures preserved. Nothing is merged back or pushed. The results
below describe the earlier integration baseline, not the current gate.

## Implementation reconciliation

| Area | Resulting implementation and extra case retained |
| --- | --- |
| K47 packed re-probe, wave17 | Devel already handles NET followed by NOT_FOUND. Retain only the additional attempted-holder check for an asynchronous health probe clearing the down-mark between the first fetch and retry. Preserve devel's returned/missing-holder permutation tests and add the private health-clear/deadline/unreachable cases. The separate published-span recorded-holder re-probe remains: it covers span overlays rather than packed base reads. |
| K51 server heartbeat and wave26 wake-up | Keep devel's lifecycle, monotonic condition wait, stop broadcast, join and error unwind. Drop the private heartbeat start/join implementation. The private rejoin-thread ownership is separate: defer its start until the shared owners exist, stop/join it in bounded teardown, and keep its failure/paused-owner tests. It does not replace devel's heartbeat path. |
| K45 compacted-prefix refusal, wave17 | Keep devel's pre-mutation prev-compacted rejection and its resync/campaign fencing. Do not restore the private pre-fix path. |
| Task G pressure ports, wave19 | Keep devel's single host_kv_pressure predicate and memory-based admission/leadership policy, without reintroducing an L0-only pressure selector. The stricter private gate independently proves KV-memory refusal and the actual committed-log watermark; it never combines a former leader's memory refusal with a later follower backlog. |
| Task G client receipt/retry ports | Keep the ported STALE/held-pressure/read-pool behavior and devel's deadline-bounded metadata retry policy. Retain additional exact detached-snapshot replay after a late PARTIAL receipt, stable REPORT coordinator selection across BUSY, and bounded ticket-authorized immutable-body reuse. The actual late-PARTIAL production block passes locally and fails at runtime against b2d57e421 write.c; the older wrapper-only receipt test cannot distinguish that case. |
| Namespace transaction receipts and allocation | Retain exact pending receipt replay only after fresh COMMIT authority, after releasing the KV scan reader gate; uncertain/covered-ACK history still refuses. Retain apply-serialized allocator reservation and mount bootstrap coalescing: these address the retained allocator starvation and shared-window cases absent from devel. |
| Raft pump and snapshot progress | Retain wall-clock empty acknowledged-prefix probes for coarse Raft pump cycles and known-leader queued-contact liveness. These are distinct from K51's server membership heartbeat lifetime. Retain resync by a replacement leader with no prior match and pending-snapshot ownership after a delayed log ACK. Devel's compacted-prefix and receive/persistence fences remain. |
| Session/finalization | Retain exact-epoch session ACKs, cleanup admission reserve, bounded definite handoff retries and last-certified wait diagnostics. Transport uncertainty is never changed into successful cleanup. The current ACK encoding is 34 bytes; all qualification uses fresh stores, with no conversion or downgrade path for retained failures. |
| Other unique private fixes | Retain exact PUT/namespace durability epochs and proven early fsync proofs, bounded prepare/abort/resolve cohorts, checked KV flush-file cohorts, FileID reuse only after authoritative OPEN/GETCHUNKS, and query LOOKUP identity/attributes captured from the same fresh reply. Keep devel's shared codecs, strict stored-row/infrastructure-error handling, K43 hooks and all unrelated newer fixes. |

The private and devel implementation histories remain traceable as merge
parents. Superseded code is removed from the resulting tree, rather than
maintaining two selectable policies. There are no new product A/B switches.

## Required qualification

Fresh exact-source archives, original private profiles, no timeout, workload,
quota or durability relaxation; original failed stores and evidence remain
untouched. The committed `scripts/qualify_publication_repair.py` runs nuc's
three concurrent K38/D4/K22 rounds, exact cold reads and log-watermark gate.
The RDMA profile runs slow-member durability, three RDMA K38 repetitions,
RDMA D4 and TCP K22 plus exact cold reads. Both are single-host private
fixtures, not a multi-host release qualification.

Run the nuc profile normally, with `EFS_TEST_TARGET=test-all` so its final
`scripts/make_test_summary.sh` invocation executes one full make test-all.
Run the RDMA profile with `--targets-only`: the phase selector records
`TARGETED_PASS_FULL_NOTRUN`, not a full-suite PASS. It changes no test body
or product behavior and avoids a second redundant full suite. Build identity,
archive/binary hashes, final gate lines and evidence hashes are recorded
below. The requested one full suite completed; it is not a PASS.

Integration preflights use separate raw directories on node9901 under
`/data1/efs/evidence/taskH-repair-20261026/integrate-b2-preflight*`; scratch
sources are under `/tmp/efs-taskH-repair-20261026/`. The first working-tree
candidate compiled successfully, then a private extraction harness failed
because it omitted devel's shared pressure helper. Its original logs remain;
the harness now extracts that real predicate too. A local qualifier self-test
could not bind loopback under the Mac sandbox and is deferred to Linux.
Neither preflight is final-head gate evidence.

First committed candidate `60833b0d2` passed the Linux fast suites and all
RDMA targets/cold checks, then its independent K22 log-watermark harness
failed before starting daemons: it still extracted a numeric private
SETATTR length, while devel now defines it through the shared codec. The
filler now calls that shared encoder directly, with unchanged load and
deadlines. Its failed setup log and 29,019 exact cold checks remain in
node9901 attempt-65; they are not relabelled as a complete qualification.

## Integrated-source provenance and environment

The corrected tested source is `d0e179a6910aa87595f4ffffcded1f5fe6b47c18`,
which contains merge `60833b0d2` (parents `e7b2b4ae9` and pinned devel
`b2d57e421`). The follow-up changes only the K22 test filler/driver and
this evidence note; product code is identical to the merge commit.
The exact Git archive SHA-256 is
`87464a29ceafb048ed8131d7b66921ad48416712d21818d5c50e10fb1547c36f`.
Builds set `EFS_GIT_ID=d0e179a69`, branch `agent/task-h-repair`, and use
fresh directories without source overlays. The controller's `run.sh` and
source manifest remain alongside each run. No mixed-build or retained-store
conversion is attempted.

- Nuc: 8 logical CPUs, 32 GiB RAM, XFS/NVMe; three private loopback daemons
  and two mounts per fixture, TCP. Source/run root
  `/home/maik/efs-taskH-repair-20261027/attempt-71`; evidence root
  `/home/maik/efs-artifacts/taskH-repair-20261027/attempt-71`.
- Node9901: 192 logical CPUs, approximately 377 GiB RAM; private loopback
  daemons, explicit RDMA `mlx5_0` for K38/D4, TCP for K22 and setup checks.
  Source/run root `/tmp/efs-taskH-repair-20261026/attempt-66`; evidence root
  `/data1/efs/evidence/taskH-repair-20261026/attempt-66`. The main cluster's
  stores and ports 17432–17434 are outside this run.

Fragment I/O is the current O_DIRECT-only product path. Fault-build fixtures
keep packing, query indexing and KV maintenance active; private quotas stay
at their original 1/2 GiB settings. K38 uses two writing mounts and 24 writers
under the 32 MiB body bound; D4 uses eight writers, 45-second growth and
75-second fixed-working-set loads, 10-second foreground and 150-second drain
bounds. The short fixed-set run reports the RSS plateau statistic; it is
not the separate 600-second-or-longer F1 plateau acceptance. K22 independently
checks memory-driven refusal/handoff and committed-log admission. No test
limits, quotas, durability conditions or payload oracles were relaxed.
Nuc's controller checks recognized foreign EFS processes before and during
controlled live stages; this is not proof of exclusive host I/O.

Commands, from each archived `src` with Linux FUSE and build prerequisites:

```sh
export EFS_GIT_ID=d0e179a69 EFS_GIT_DESCRIBE=d0e179a69
export EFS_GIT_BRANCH=agent/task-h-repair EFS_READ_FRAG_PROF=1
export EFS_TEST_TARGET=test-all
# Nuc; ROOT and OUT are fresh sibling paths outside src:
python3 scripts/qualify_publication_repair.py --profile nuc --root ROOT --output OUT
# Node9901, with the private libfuse prefix/pkg-config/library paths set:
python3 scripts/qualify_publication_repair.py --profile rdma --targets-only --root ROOT --output OUT
```

Each run first executes native and ASan/UBSan Raft checks with early-read
mode off/on, then standalone peer-stop and recovery gates. Node9901 uses
`ASAN_OPTIONS=detect_leaks=0` because its leak-sanitizer runtime is unsupported;
that host's sanitizer results do not include a leak-check claim. The full
suite runs exactly once on nuc through `scripts/make_test_summary.sh`, using
`EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8` and target `test-all`. NOTRUN markers remain
NOTRUN and make an otherwise clean full suite INCOMPLETE.

## Recorded rerun failure: K38 RDMA remains OPEN

Corrected-source node9901 `attempt-66` **FAILS** at `k38-rdma-3`:
writer 16's pwrite returns EIO after 43 records (68.375-second test). The writing
mount records two background-pressure REPORT errors, `EFS_ERR_PROTO=-7`,
after 276 ms and 600 ms; neither is an exhausted deadline. This run passed both
slow-member durability cases, resync, automatic IDs, abort-forwarding,
quota-reclaim and its first two RDMA K38 repetitions. The remaining RDMA
D4/K22/cold stages are NOTRUN on this attempt, rather than inferred from
`attempt-65` or the TCP results. No protocol check is relaxed and no error
is retried into a passing verdict.

All original logs/stores remain in the run/evidence roots above. Closed raw
copies and manifest are at node9901
`/data1/efs/evidence/taskH-repair-20261026/attempt-66/final-closed/manifest.json`,
SHA-256 `ced19673c8d25049e6e2ebd7c2c6da1ad3489289a4f2f734f742e3f74ad8bacd`.
The 109 closed files, all five binary hashes, zero owned live executables and
zero owned mounts were checked. The earlier setup-only failures remain
separately closed: nuc70 manifest
`b872e486f690a0b3372e0fa037b8b0810fc0cafe3972857c7290de6b4e75bc14`
and node65 manifest
`40d92a5505d6bf728c9cb2320007401ac723bdb76abe6140e53ad1da2ba874c7`.

Two separate logging-only diagnostic builds each ran six unchanged K38
repetitions without reproducing the error. The first also ran the corrected
K22 log-watermark fixture successfully; the second removed verbose tracing
and added RPC protocol-error return-site logging. These are diagnostic
observations, not fixed-build qualification passes, and they do not clear
the original failure. Neither logging patch is in the product branch.
Raw source/patch manifests, commands, fixtures and closed logs remain under
node9901 `integrate-proto-diag1` and `integrate-proto-diag2` in the same run
and evidence parents. Closed manifest SHA-256 values respectively:
`bbc515c705e55412b06e3f6b8d65fc8568fa6fceb1443b7f2e70c167683b83d3` and
`c2328010c3c355b94009f4faadb2a2b95f08d17b69d5aeb5092b05c435bcdc5f`.
Both were checked for zero owned executables/mounts after completion. The
protocol-error cause is unresolved; changing timeouts, ignoring it or merely
selecting a later clean repetition would not qualify K38.

## Exact-source qualification result

Nuc `attempt-71`, product/test source `d0e179a69`:

- Three concurrent rounds each passed K38 shared-chunk pressure, D4 growth,
  D4 fixed-set/drain and K22 memory-admission/handoff (12/12 loaded checks).
  Resync, auto-ID, abort-forwarding, quota-reclaim, the independent committed-log
  watermark and the focused receipt/ownership/pressure regressions passed.
- Nine retained fixtures completed orderly cold verification: **7,766 exact
  payloads, zero errors**. No sampled payloads stand in for that check.
- One complete full `make test-all`, through the unchanged summary runner:

```text
TOTAL 523 FAIL 2 NOTRUN 12 STATUS FAIL (1268s) OVERBUDGET 41 BUILD 0s
```

All 523 commands were attempted; NOTRUN remains incomplete coverage and the
41 overbudget entries remain performance observations, not suppressed failures.
The 12 NOTRUN markers comprise missing external cluster stop/remount scripts,
missing Valgrind headers, nine RDMA-path cases without a usable nuc IB device,
and the stalled-compactor case requiring a separate fault-build invocation.
The full runner took 1268.579 seconds; the entire qualifier took 2279.216 seconds.
The full gate was run once, not retried until a clean draw appeared.

The two full-suite failures are retained separately:

1. `tests/test_b6_unavailable.py`, case 459, `refuse`: "1 files packed, no
   segment written degraded". The retained n1 log has packed totals 1, 4, 5,
   exactly one degraded-write event at 18:57:17.310Z, and the total 5 line at
   18:57:17.349Z. Pre-fault setup already waited for all four initial small
   files to pack. Together with the reported delta 1 and empty new-degraded
   event set, this implies the baseline cut included the event but not its
   later counter update. A single log snapshot does not make two separately
   logged events atomic. This is an accounting counterexample; it does not
   prove protection of all acknowledged B6 files, and no B6 PASS is inferred.
   The oracle still needs a consistent pack-pass/segment observation and a
   retained rerun. Its limits were not changed. Failure log SHA-256:
   `3a8c8861db62af01279e78b9e6583da6d1ffd799461a9ed35a271e14abd843b9`.
   Retained fixture:
   `/home/maik/efs-taskH-repair-20261027/attempt-71/src/tmp/case-459/efs-b6-af02o37l`.
   The refuse/n1.log SHA-256 is
   `1cc8e5ec7839bb59c214398cc3181d8b34f54fa772535feb18dcf45c14215d2d`.
2. `tests/test_shutdown_stuck_worker.py`, case 473: the expected worker label
   was stale after keeping devel's heartbeat lifecycle. Actual shutdown forced
   at the unchanged 6000 ms deadline, with stores preserved and worker
   `rejoin thread`; the private test still expected `rejoin and heartbeat
   threads`. Commit `931f877b3` changes only that expected label, preserving
   every timing, barrier, store-retention and orderly-stop assertion. The
   original full failure log remains, SHA-256
   `33548aeb409da1025b252c2e720b6124033c605d124a4d965bf040bf4457b1f8`.
   Its fresh focused rerun is recorded below; it does not rewrite the full
   suite's FAIL line.

Nuc's final closed raw manifest (395 files) is
`/home/maik/efs-artifacts/taskH-repair-20261027/attempt-71/final-closed/manifest.json`,
SHA-256 `7dfd3fcba20ea517dcf3b8ad292e89faf0e4dc3829aa9a0a696f2611bd4c2ff1`.
All five binaries still matched the qualifier's initial hashes; owned
executables and mounts were both zero after completion. Qualification
`result.json` SHA-256:
`5f05706a33056f22f6a930d4a97d466259bdb15887da06e3ffa79c08b20f8942`.
The run contains all NOTRUN reasons, overbudget timings and original failures;
no stores or raw captures were exported or removed.

The subsequent `931f877b3` product tree is identical to `d0e179a69`; its only
code/test change is the shutdown test's expected worker string. The full
suite above is attributed to its exact source, rather than claimed as a full
run of that later test revision. No production change is offered as a fix
for the open RDMA K38 protocol error or the original K33 read EIO.

## Focused shutdown-oracle correction

`931f877b334ea729e43fe2bc6df147092ceb8712` changes only the expected rejoin
worker string. A fresh nuc source archive, SHA-256
`65a070211f5616f10d7a4321535ae2f05afafeb4ac30145a41c23af15e40be33`,
built successfully and ran `tests/test_shutdown_stuck_worker.py` with
`EFS_SHUTDOWN_KEEP=1 EFS_TRANSPORT=tcp`, on fresh fixtures. **PASS**, all five
case PASS lines, unchanged 6,000 ms deadlines, barrier/store checks and
restart assertions. Full product sources, headers, scripts and Makefile
are byte-identical to the full-suite source `d0e179a69`.

Run/source root:
`/home/maik/efs-taskH-repair-20261027/integrate-worker-label`.
Raw evidence root:
`/home/maik/efs-artifacts/taskH-repair-20261027/integrate-worker-label`.
Closed manifest SHA-256:
`da22b3d829eae2714df55981f85d54c3b0bf9a8b430a16ac281384589b52030c`;
test log SHA-256:
`b158843948cb83da6f5c49377737a9296b6873ac758692ded0470c841da83d5b`.
Twenty closed files, five binary hashes, zero owned executables and zero
owned mounts were recorded. The original full FAIL is preserved; no full
rerun of the changed test revision or overall qualification PASS is claimed.

## Original K33 EIO

**OPEN.** The original readback EIO for `w-19` during the second single-member
outage has not been causally established. Subsequent exact cold verification
and passing growth runs do not prove its cause. The packed/span recovery and
pending-snapshot fixes are not retroactive proof of that incident. Preserve
the original FAIL, retained stores/captures and documented evidence gaps.
