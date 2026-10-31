# Task H: five-member O_TRUNC/fsync recheck — October 26, 2026

> Migrated from `agent/task-h-repair` at `cd822e2f7` on Oct 30, 2026, by Task BO. Nothing was shortened: the page already held conclusions, methods, hashes and raw-capture locations only. Scripts and tests it names in backticks exist only on that branch; see [the migration page](task-h-evidence-migration-20261030.md).

`agent/devel-task-h`, observer `7b1bc9676`, product `394f2b791`: gate `TOTAL 508 FAIL 3 NOTRUN 14`; untouched base also 508/3/14 (two matching failures). Evidence: `docs/status/task-a-finding-b-rerun-20261026.md`. NOTRUN deterministic new-failure injection. K38 writer errors and K45 D4 EIO remain OPEN. Candidate B: loopback and RDMA rechecks PASS; not reproduced.

## Scope and current verdict

Task H rechecks [Task A Candidate B](task-a-20261025.md#product-findings-for-the-coordinator)
for checklist F3. **The target O_TRUNC/fsync case is NOT
REPRODUCED on current devel in both required stages (scoped closure); the
full code gate FAILS.**
The related publication/error family is not declared fixed overall: the gate
found K38 writer errors and two D4 failures. K38's loaded qualification is
reopened; K45 records the D4 observations. Root causes and equivalence between
these observations remain unproven. No product fix is claimed by Task H.

All work is on `agent/devel-task-h`, cut from devel
`394f2b791e3e8f9abc23f742dea754ad6f614a97`. Product C and headers are unchanged.
The new script is a manual observer, not a new `make test-long` command;
no budgets, deadlines, behavior flags, wire or stored encodings changed.
Nothing is merged or pushed.

## Environment and identity

Assigned host nuc, user maik UID 1000, Rocky-family Linux kernel
`6.12.0-211.67.1.el10_2.x86_64`, Intel Core i5-1135G7 (4 cores / 8 logical
CPUs), about 30 GiB RAM, XFS home filesystem, GCC and system FUSE 3.16.2.
Private source/work: `/home/maik/efs-taskH`; raw evidence:
`/home/maik/efs-artifacts/taskH-20261026`. No infrastructure store, 17xxx or
27xxx endpoint, efs-user resource, devops/cluster repository or other host
was used. All long runs use screen and nice 10.

Normal binary identity:
`efsd 394f2b791 (branch agent/devel-task-h, built 2026-10-27T01:16:50Z on nuc)`.
The code gate builds the fault daemon from the same source; its compiler
commands and identity flags are in `gate/build.log`.

| Binary / driver | SHA-256 |
| --- | --- |
| `efsd` | bf598fb05fb836fb892008def424bf886d6077c4fe9dac72359fa6ac94f675d8 |
| `efs-fuse` | bc1c798c724c841fe421460adfad540c3f3d49d27e48cdbff30543b115edcfc6 |
| `efs-mgmt` | 5ffc43a3f6b3de0b00650195a4678a0b3c8b8fcae11803b297c03a1a3423f7ae |
| Observer at `f4d783e13` | a6e3e6532a928ae80103cd6e95b45d3142d073c0f4cbf2d7383726f0acbcc4e4 |

## Ten unchanged core suites

Ten fresh `loopback-5` clusters, five normal daemons and two FUSE mounts,
TCP, O_DIRECT, packing and cluster commands enabled, expiry policy default
off, 4 GiB per daemon. Main ports 19740–19744; private helper range starts
19600. Each run selected the complete core suite, four jobs and the matrix's
150-second minimum case timeout; longer decorator budgets, including the
180-second large-truncate cases, remain unchanged.

| Run | Core wall seconds | PASS | FAIL / SKIP / NOTRUN cases | Target case |
| --- | ---: | ---: | --- | --- |
| 01 | 118.698 | 218 | 0 / 0 / 0 | PASS |
| 02 | 109.250 | 218 | 0 / 0 / 0 | PASS |
| 03 | 112.410 | 218 | 0 / 0 / 0 | PASS |
| 04 | 100.962 | 218 | 0 / 0 / 0 | PASS |
| 05 | 49.733 | 218 | 0 / 0 / 0 | PASS |
| 06 | 141.265 | 218 | 0 / 0 / 0 | PASS |
| 07 | 114.535 | 218 | 0 / 0 / 0 | PASS |
| 08 | 112.836 | 218 | 0 / 0 / 0 | PASS |
| 09 | 111.309 | 218 | 0 / 0 / 0 | PASS |
| 10 | 116.759 | 218 | 0 / 0 / 0 | PASS |

**2,180/2,180 cases PASS.** Every results table has its final `# complete`.
Per-run raw outputs are `runs/run-NN/gates/posix-core/`.

The command selected only `posix-core`, as the task suggested. Therefore
its automatic `_identity-end` comparison is **NOTRUN**, reason `starting
build identity was not recorded`; each aggregate qualifier exits 3. These
labels remain unchanged. Separately, the core gate captured live identities
before the workload and again at the end. Rechecking those records with
`qualify.identity_change_reason` passes for all ten runs: five matching
source-394 daemons, two matching clients, unchanged client executable digest,
no deleted executable. This analysis is retained in
`independent-identity-checks.json`; it does not rewrite the wrapper results.

## Strict supplementary success-path check

The original case accepts an open-time EIO/EBUSY if the old 300 MiB file
remains exact. Its PASS alone cannot prove that the later fsync ran. The
committed observer (`agent/task-h-repair:scripts/task_h_truncate_probe.py`, branch only; see [migration page](task-h-evidence-migration-20261030.md)) executes that
unchanged function ten times, retaining its 300 MiB initial file, 10 MiB
rewrite and 180-second case budget. Python C-return events must show exactly
one successful `os.open` and `os.fsync` in the target function. An allowed
early refusal cannot pass this observer. No new retries or smaller payloads.
Each round also reads every rewritten byte through the second mount.

**10/10 strict rounds PASS**, ten successful truncate opens and rewrite
fsyncs; 104,857,600 bytes checked on the peer, plus the original function's
local full-content/size checks. Workload wall 144.414 s; plugin gate 145.380 s.
Explicit `identity`, observer and final identity gates all PASS, aggregate
exit 0. This is correctness observation, not a latency benchmark or crash
persistence test. Raw: `strict/gates/task-h-strict/probe/results.json`.

Relevant merged coverage is K38's body-less STALE-span replay and exact-record
classification (`48c78447a`, `bac34da00`) and D4's unconditional publication
handling after a truncate, narrowed to preserve unseen same-epoch spans
(`988efba3e`). K39 is also present but no outage is injected here. No bisect
isolates which fix eliminated the original one-off O_TRUNC failure; the
pooled K38 failure below prevents a blanket K38 closure.

## Full code gate and untouched-base comparison: FAIL

Exact first result:

```text
TOTAL 508 FAIL 3 NOTRUN 14 STATUS FAIL (1578s) OVERBUDGET 17 BUILD 24s
```

| Command | Observed failure | Disposition |
| --- | --- | --- |
| `tests/test_d4_progress.py` | One foreground `write+fsync 1 MiB` operation took 15.394 s against 10 s and reported EIO at load time 48.03 s | K45 OPEN; syscall stage and publication outcome not established |
| `tests/test_d4_plateau.py` | Population create and load overwrite reported EIO; 1,023/1,024 working-set files created | K45 OPEN; shared root cause with progress failure not proven |
| `tests/test_k38_shared_chunk_pressure.py` | Three writer EIOs and two EAGAINs; retained client tail includes REPORT timeout, PARTIAL/STale receipt and unrecorded apply-time CAS loser | K38 loaded qualification reopened; no new root-cause claim |

These are application errors, not waived as environmental slowdown. D4's
reported `verify_bad=[]` does not prove the failed operation's durability,
commit status or rollback. K38 failed before its final record verification.
The 17 over-budget commands and wall exceeding D44's 1,100-second target are
reported, not fixed by raising budgets or changing the target.

The first D4/K38 scripts remove their fixtures in `finally` by default, even
on assertion failure. The full gate failure outputs remain, including K38's
client-log tail, but full daemon/client logs of those first failed fixtures
are unavailable. This limits diagnosis; no request ladder is invented.

A separate **untouched base** export at `base/src` runs the same eight-job
`test-all` command, with source/build id `394f2b791`, branch label `devel`.
Only existing test retention settings `EFS_D4_KEEP=1 EFS_K38_KEEP=1` are
added, so all D4/K38 fixtures/logs survive for triage. They change final
fixture disposal, not assertions, workload, deadlines or product code.
Output: `base-control/`. Final control result:

```text
TOTAL 508 FAIL 3 NOTRUN 14 STATUS FAIL (1506s) OVERBUDGET 12 BUILD 58s
```

| Command | First gate | Untouched-base control |
| --- | --- | --- |
| D4 progress | FAIL: EIO and 15.394 s foreground operation | PASS; foreground write/fsync maximum 6.628 s, 421 files exact |
| D4 plateau | FAIL: population/create and overwrite EIO | FAIL: population create EIO, 1,023/1,024 files; same failure class |
| K38 shared-chunk pressure | FAIL: three writer EIO, two EAGAIN | FAIL: writer 18 EIO; same partial/unrecorded REPORT class |
| K22 leader admission | PASS | FAIL: four client EIOs after leadership settled; also 26 inside the pressure window |

Thus D4 plateau and K38 reproduce on the unchanged base. D4 progress is
intermittent and still OPEN, not excused by the control PASS. K22's extra
base failure is retained against its existing pressure/error qualification;
no new K22 root cause or equivalence to W166 is asserted. Neither full gate
passes. The 14 NOTRUN commands include unavailable RDMA devices, Valgrind,
sanitizer runtime, external cluster scripts and a fault-build-only subcheck;
the later RDMA POSIX stage does not silently qualify those omitted commands.
Both walls exceed D44's 1,100-second target; budgets and target are unchanged.

### Retained failure ladder and recommended next investigation

The control preserves complete D4/K38 client and daemon logs. For D4
plateau's failed inode 68693, three-record publication `:302`:

1. Node 1 returns unit BUSY / unknown after about 4.27 s.
2. Node 2 handles the same publication and reports one stale record,
   PARTIAL/STALE after about 3.15 s.
3. Client fragment-budget exhaustion logs degraded acknowledgements; its
   REPORT returns NET, then same-identity queries see node 1 UNKNOWN and
   node 2 PARTIAL. Resolution returns STALE immediately, with
   `stuck=unrecorded (an apply-time CAS loser)`; flush maps that to EIO.

This correlates the REPORT and receipt path; it does not prove which write
syscall failed, that all fragments were durable, or that the operation was
fully committed or rolled back. Exact log timestamps and identities are
retained in `base-control/retained-fixture-logs/case-452/`.

K38's control failure has an 8,015 ms REPORT loop (one STALE, one BUSY),
then 3,151 ms of receipt lookup ending PARTIAL/STALE and the same unrecorded
CAS-loser diagnostic. Later unowned-staging/putid-miss observations are
present. Follow this path before changing packing, timing bounds or receipt
verdicts; current evidence does not prove a packing-specific defect. A
PARTIAL receipt must not be turned into success. The safe recovery question
is whether the unpublished records can be rebuilt and reproposed under a
new publication identity within the existing write contract, without
replaying already-published effects or claiming absent receipts are aborts.
A deterministic pause/fault reproducer and causal fix are still owed. The
cause is not small or isolated enough for a product edit in Task H.

## Authorized real-host RDMA stage: PASS

The user explicitly authorized stopping, deploying and restarting orcd-mini
for Task H. Both old mounts were idle. Node9900's client stopped rc 0;
node9901's mount and all three daemons stopped orderly rc 0, with sync,
reclaim-journal and usage flush complete. No format or store deletion.
Both release links now point at `394f2b791e3e`; start rc 0 preserved the
existing format marker and stores. All three members and both main mounts
remain serving on that build after the test. Devops was not edited.

Three daemons on node9901, endpoints 10.1.99.1:17432–17434, 720 GiB quota
per member, O_DIRECT, `.efs` policy enabled, expiry off. Node9901 and
node9900 each expose 192 logical CPUs and about 377 GiB RAM; captured
hardware records supersede the task brief's older CPU estimate. Node9900
runs RHEL 9.8 kernel `5.14.0-687.39.1.el9_8.x86_64`; node9901's full uname
and CPU records are retained. Clients use strict RDMA on mlx5_0: actual
logs show segmented RDMA connections and sampled post-setup TCP counters
0/0 with zero violations. This is workload evidence, not exhaustive proof
of every transport exchange.

Server build: `efsd 394f2b791 (branch agent/devel-task-h, built
2026-10-27T02:22:40Z on node9901)`. Node9900 client build time
`2026-10-27T02:22:49Z`, same source and branch. Both used private libfuse
3.16.2; the first link omitted its runtime path and was corrected before
server shutdown using explicit make `FUSE_LIBS` with the private rpath.
Both the failed link observation and corrected build logs remain.

| Final binary | SHA-256 |
| --- | --- |
| node9901 efsd | 2e5b9e1d93816bdc3cc9184ce5e999f7824e896c8792bc74d478e5e5387c3bb0 |
| node9901 efs-fuse | 3e2323ff9994a9a30b2fb0d4f0cc1959eebdfca7e846943b19e7241c7ee4ff80 |
| node9900 efs-fuse | ab3242c25c7192d3b1b386fc44c65f2b52111cd36a7005305cc5ea5fe912c20e |
| Observer `7b1bc9676` | 176adc03c518b741188c868175d7518ee9c9f2215aad81e999d3b82687260883 |

Ten complete core suites ran directly on node9900's RDMA mount, using the
same committed core suite that devops smoke invokes, with four jobs and
150-second minimum case timeout. Every run: **218/218 PASS, zero FAIL,
SKIP or NOTRUN**, and complete TSV. Walls for runs 01–10: 23.2, 23.5, 23.2,
24.1, 20.5, 22.4, 22.1, 19.9, 20.1, 19.9 seconds. Then **20/20 standalone
strict target-case rounds PASS**, each proving successful truncate open and
rewrite fsync; full peer readback totals 209,715,200 bytes, workload wall
26.525 s. The temporary second node9900 mount stopped rc 0. Ending process
executable hashes match the selected releases; main mounts remain live.

Candidate B closes as **NOT REPRODUCED in these required reruns**, not a
causal proof that one fix alone eliminated the old failure. K38/K45 stress
failures remain separate OPEN blockers. No outage or crash was injected.

## Reproduction and retention

Export `394f2b791` into a new private home-directory source tree with
`git archive`, build on nuc using `EFS_FUSE_PREFIX=/usr`,
`EFS_GIT_ID=394f2b791 EFS_GIT_BRANCH=agent/devel-task-h
EFS_GIT_DESCRIBE=394f2b791 make -j8`. Then, for each `NN=01..10`:

```sh
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json \
  --topology loopback-5 --gates posix-core --candidate 394f2b791 \
  --keep-work --out "$HOME/efs-artifacts/taskH-20261026/runs/run-$NN" \
  --work "$HOME/efs-taskH/work/run-$NN"
```

After those clusters stop, run the code gate:

```sh
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
```

The strict supplement uses the committed observer copied to the evidence
root (matching SHA above), with the same topology and fresh work/out paths:

```sh
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json \
  --topology loopback-5 --candidate 394f2b791 --keep-work \
  --gates identity,task-h-strict \
  --out "$HOME/efs-artifacts/taskH-20261026/strict" \
  --work "$HOME/efs-taskH/work/strict" \
  --plugin "task-h-strict=python3 $HOME/efs-artifacts/taskH-20261026/strict-probe.py --source {source} --mount {mount} --peer-mount {peer_mount} --out {gate_dir}/probe"
```

For the real-host stage, after the explicitly authorized lifecycle operations,
run on node9900 (new empty result paths, detached screen):

```sh
src=/data1/efs/orcd-mini-client/src
mnt=/data1/efs/orcd-mini-mnt
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
# Repeat with NN=01..10; keep every exit status and TSV.
python3 "$src/tests/posix/posix_suite.py" "$mnt" --suite core --jobs 4 \
  --timeout-s 150 --results "$OUT/core/run-$NN.tsv"
# Start a temporary peer mount using the same release's scripts/client.sh.
python3 scripts/task_h_truncate_probe.py --rounds 20 --source "$src" \
  --mount "$mnt" --peer-mount "$PEER" --out "$OUT/strict-20"
# Stop only the temporary peer via scripts/client.sh stop "$PEER".
```

Build each real host from the source archive with `EFS_FUSE_PREFIX` and
`PKG_CONFIG_PATH` pointing to `/data1/efs/deps/libfuse`; pass
`FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,/data1/efs/deps/libfuse/lib64"`
as a **make command-line assignment**. Use the same identity variables as
above. Deploy existing release links through the cluster's recorded lifecycle,
with mount-user checks first, client-before-daemon shutdown, unchanged
format marker, `EFS_CLUSTER=orcd-mini EFS_START_TIMEOUT=900 ./start.sh`, and
node9900 `client.sh start`. A new run needs its own cluster-change approval.
Exact controllers, commands, per-case output, identities, transport logs and
closed captures are retained outside Git:

| Host / stable evidence root | Final `SHA256SUMS` SHA-256 |
| --- | --- |
| nuc `/home/maik/efs-artifacts/taskH-20261026` | 29413a5e7e1cca33965ca09365f62efdd1d772b9e5475197d894a4c32d6d5dd2 |
| node9901 `/data1/efs/traces/taskH-20261026` (includes copied node9900 `client-run/`) | 9f9c3a9da6826ea4d4bd91aefd7aa747dc26913ded842433a5b64d9126f7c9da |

All manifest entries were checked. The 64 client artifacts were copied to
node9901 and their hashes verified before deleting the originals. Nuc
retains 77 closed core/strict daemon/client logs and the control's full
D4/K38 logs/results. First-gate full failed-fixture logs remain unavailable
as noted above. All copied text captures are closed bounded files; live
cluster logs continue independently. No raw artifacts are committed.

After checking no scratch executable, cwd or mount remained, private trees
`/home/maik/efs-taskH` and `/tmp/efs-taskH-20261026` on both ORCD hosts were
removed with `edelete --force --delete`; `PRUNED.txt` records the removal.
Nuc is free for Task I. Synthetic nuc stores, payloads, source archives and
build trees were pruned; full failed-store replay is forfeited. Recreate
fixtures from the source and method. Both active ORCD release directories,
main clients, cluster stores and earlier evidence remain intact.

NOTRUN: original concurrent full-rehearsal load, deterministic fault
reproduction/causal fixes for the new stress failures, crash/power-loss
persistence, and attribution of the positive O_TRUNC result to one merge.
The fourteen host-limited code-gate entries remain NOTRUN. Other alpha
findings, including K43, W165 and W166, remain open independently.
