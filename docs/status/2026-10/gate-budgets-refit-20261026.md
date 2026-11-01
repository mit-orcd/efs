# W160 three-run budget refit — October 26, 2026

**Task J COMPLETE, scoped Intel budget calibration; not a universal gate PASS.**
Branch `agent/devel-task-j`: corrected-source49e5ffd71 completed three serial
8-job full gates at729/723/725s,509 commands,zero failures and14 NOTRUN each.
Budget refit0f72c1665 retains slots/reasons and the1100s target/1650s bound;
fitted model1025s. Separate post-refit full gate:730s plus30s build,zero
failures,14 NOTRUN. All85 per-command medians and modeled critical chain/
exclusive tail are below. Host-limit skips and earlier Intel/ORCD FAILs
remain explicit; no failed measurements enter the fit and no repaired-build
ORCD/RDMA claim is made. Failed fixtures remain retained on Intel.

The original assignment and stopped baselines below are historical evidence.
The user ordered ORCD gates first, then quiet Intel calibration after Task F,
and subsequently authorized fixing the discovered gate failures on this branch.

## Resource and source scope

Calibration host: **intel1**. Task F completed its qualification and final
report at `fe3b34a79`; its finalized `private-stop-check.json` records no
remaining Task F process or mount. Its private trees remain retained and
untouched. Independent Task J preflight found no active test/build jobs,
no screen session, eight CPUs, load 0.09/0.23/1.95 and 828 GiB home free.
Existing cluster listeners on 17432/17433 remain untouched.

Three serial eight-job gates started in detached screen `taskJ-calibration`
after that handoff, on pinned **current devel
4066f74df5b997d70f263ae8dc1063b1a6049b92**. This differs from the ORCD source
394f2b791 only in documentation: `git diff --quiet 394f2b791 4066f74df --
src include tests scripts Makefile` succeeds. Task F's unmerged product and
budget changes are not silently included in this calibration.

Historical private source was `/home/maik/efs-taskJ/src` (now pruned), with launcher and helper copied
outside the pinned source under `/home/maik/efs-taskJ/scripts/`. Results are
`/home/maik/efs-artifacts/taskJ-20261026/intel1-4066f74df/run-1`, `run-2`,
`run-3`. The archive SHA-256 is
`ae4f166b247596f3bbc6ad151335c8cc6c4bc8bbde4bb596cd0a15084d48e0b7`;
`SOURCE-MANIFEST.sha256` records the 2,205 regular source files before build.
`PROVENANCE.json` and `host.txt` retain exact source/environment facts.
The stopped run now has a closed verified manifest, recorded below.

Intel1 is Linux 6.12.0-211.67.1.el10_2 x86_64, GCC14.3.1, Python3,
FUSE3.16.2, MemTotal15,774,852 KiB, XFS home on LVM. The launcher exports
`EFS_FUSE_PREFIX=/usr`, `EFS_TEST_TARGET=test-all`, `EFS_TEST_JOBS=8`,
`EFS_BUILD_JOBS=8` and commit-derived `EFS_GIT_ID`, `EFS_GIT_BRANCH=devel`,
`EFS_GIT_DESCRIBE`; exact build version is saved per run. Invocation:

```sh
cd "$HOME/efs-taskJ/src"
EFS_FUSE_PREFIX=/usr bash "$HOME/efs-taskJ/scripts/calibrate_test_budgets.sh" \
  4066f74df5b997d70f263ae8dc1063b1a6049b92 \
  "$HOME/efs-artifacts/taskJ-20261026/intel1-4066f74df"
```

Only Intel run 1 completed, with failures. The launcher stopped before runs
2/3. For that stopped baseline, three-run medians, refit and post-refit were NOTRUN;
no failed or ORCD timing is reused as a successful calibration cost. ORCD gates used
fresh untouched devel394f2b791 archives, with its source/build identifiers.
Their exact results and closed manifests remain below. The coordinator can
collect this branch; no merge or push was performed. Shared devel's other
agent D39 edit remains untouched.

## ORCD-first execution

The user explicitly requests this order; it does not release intel1 early.
Agent-forwarded SSH through `orcd-login` works without typing a password or
token. Each run is detached in screen, with a new private `/tmp` source tree.
Neither running cluster stores nor listeners on 17432–17435 are changed.
No devops/cluster repository is edited. Tests retain their own dynamically
reserved loopback ports. No test, pool slot or deadline is changed.

| Host | Actual environment | Task J run |
| --- | --- | --- |
| node9901 | RHEL kernel 5.14.0-503.38.1.el9_5.x86_64; 192 logical CPUs in affinity; MemTotal 395217884 KiB; GCC 11.5.0; Python 3.9.25; supplied libfuse 3.16.2 | 8 build jobs / 8 test slots; 769 s, 5 FAIL |
| fstor007 (`fstor007-mgmt`) | RHEL kernel 5.14.0-687.36.1.el9_8.x86_64; 96 logical CPUs in affinity; MemTotal 791576532 KiB; GCC 11.5.0; Python 3.9.25 | 8 build jobs / 8 test slots; compatible-FUSE gate 750 s, 1 FAIL |

The assignment's eight-CPU shorthand does not describe these exposed CPU
allocations. These are **8-job** gates, without CPU pinning; their walls will
be reported separately from the eight-job Intel calibration, never fitted
into its medians. Build time is separate from the runner's gate wall.

**fstor007 first attempt FAIL (prerequisite):** unchanged devel does not build
against system libfuse 3.10.2: `fuse_cmdline_opts.max_threads` is unavailable.
The first summary is exactly `TOTAL 0 FAIL 1 NOTRUN 0 STATUS FAIL (build or
command enumeration failed, or no test commands; see build.log, enum.log)`.
No test ran. Raw first-attempt evidence remains under
`/home/erbmi1/efs-artifacts/taskJ-20261026/fstor007/gate-394f2b791/`, including
`PREREQUISITE_FAILURE.txt`. This is not promoted to PASS or silently replaced.

The corrected attempt uses a **private copy of the supplied node9901
libfuse 3.16.2** under
`/home/erbmi1/efs-taskJ-deps-20261026/libfuse-3.16.2`, visible on the shared
home filesystem. Library/header bytes are unchanged; only pkg-config prefix
paths are relocated. Copied library symlinks remain inside the canonical
private prefix. Dependency `SHA256SUMS` SHA-256:
`6ca161809f4a13b385d3aa8d98e897653fef82c88d7cfd47d649f1d0583c2b4f`.
No root installation, system library replacement, source fallback or EFS test
edit was performed. The compatible build starts in a separate fresh source
tree and output directory.

Completed source/launcher roots (now pruned) and retained text evidence:

- node9901 source `/tmp/efs-taskJ-20261026-394f2b791/src`; screen
  `taskJ-node9901`; output
  `/data1/efs/traces/taskJ-20261026/node9901/gate-394f2b791/`.
- fstor007 compatible source `/tmp/efs-taskJ-20261026-394f2b791-fuse316/src`;
  screen `taskJ-fstor007-fuse316`; output
  `/home/erbmi1/efs-artifacts/taskJ-20261026/fstor007/gate-394f2b791-fuse316/`.
- Both launchers export the exact source identifiers above,
  `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8` and invoke
  `bash scripts/make_test_summary.sh`. Node9901 uses
  `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`; fstor007 uses the relocated
  private prefix. The runner supplies its FUSE rpath and per-command TMPDIRs.
- `exit-status`, `DONE`, summaries, timings, progress, build/enumeration/run
  logs and per-failure logs are saved independently. The closed manifests
  and task-owned scratch cleanup are recorded below.

## Intel calibration stopped on unchanged-base failures

Verbatim result:
`TOTAL 508 FAIL 2 NOTRUN 14 STATUS FAIL (616s) OVERBUDGET 1 BUILD 29s`.
Build: `efsd 4066f74df (branch devel, built 2026-10-27T03:21:40Z on intel1)`.
Controller exit1, stage `run-1 FAILED; preserved; calibration stopped`;
`run-2` and `run-3` were never created. This is directly an untouched-current-
devel comparison: Task J changed no product, test or measured budget.
The 616 s wall is **not** evidence for lowering D44's target: failing tests
ran shorter paths. `test_k33_growth.py` also exceeded its 135 s budget at
136.0 s; no change is inferred from this single observation.

| Finding | Observation and assessment | Remaining resolution |
| --- | --- | --- |
| J1 / K22 fixture | `test_k22_leader_admission.py:172` asserts group0 leader is node1; observed node2, before pressure is armed and before client load. The fixture configures pressure only on node1 and assumes bootstrap leadership persists. | Explicitly establish the fixture's pressured leader before arming; do not weaken the admission/client correctness assertions. No test change is made by Task J. |
| J2 / K36, unclassified | `test_k36_recovery.py:509` expected at least one missing segment after deleting a fragment. Observed recovery `verified`, segments `(3,0)`, no missing/unrecoverable count. Victim is `segs(c, holder)[0]`, selected by filesystem ordering. | Prove the removed fragment belongs to a current referenced segment and correlate recovery's inventory before deciding fixture race versus product defect. Recovery corruption or successful repair is not established by this output. |

Both tests unconditionally delete their fixture trees in `finally` in the
ordinary gate (`EFS_K36_KEEP` was not set). Their detailed daemon logs and
stores were therefore already unavailable when the command outputs reported
failure. Full command stdout/stderr, summaries and timings survive; this
limits J2 diagnosis. A future diagnostic should retain failure fixtures,
not silently retry them. These failures remain failures. Task J's explicit
"do not change any test" boundary keeps the required fixes outside this
budget-only branch; there is no exclusion, timeout relaxation, or extra
attempt on the unchanged source.

The fourteen NOTRUN markers match the expected Intel prerequisites: no
cluster lifecycle scripts, no memcheck header, unavailable IB paths,
fault-build-only stalled-compactor case, and two missing-tool self-test
markers. They are reported separately from the two failures.

After completion, all2,205 source files verified against the initial
manifest; source-manifest SHA-256:
`40cceac22f5e0ab0749525d059706daff6086ad95e7504fe72da348bde927edb`.
`private-stop-check.json` records no Task J process or mount. Task J's own
inactive source/build/fixture root was pruned with edelete; Task F's retained
trees and all existing clusters remain untouched. `PRUNED.txt` states the
fixture-loss limitation and what survives. Closed raw evidence:
`intel1:/home/maik/efs-artifacts/taskJ-20261026/intel1-4066f74df/`.
Its verified `SHA256SUMS` covers33 files; SHA-256:
`286844d59f36742fd037243f8560d9fdf351b539a302d48a427ba26f5aa4f593`.

Task J remains INCOMPLETE. A materially corrected source needs a new,
separately retained three-run campaign before any fit is possible; an
unchanged-source retry cannot turn these findings into PASS. No new timing,
median, critical chain or target reduction is invented here.

## Prepared analysis

`scripts/refit_test_budgets.py` consumes three complete summary-runner result
directories, each containing `cmds.txt`, `timings.tsv` and `summary.txt`. It
checks identical command enumeration, unique complete timing indices and the
original budget values. Failed commands are rejected as calibration inputs:
an early failure is not the cost of a successful workload. A failure must
remain in the report; it must not be silently excluded or rerun into PASS.
NOTRUN entries are identified per command so skip-path costs cannot qualify
hardware-dependent execution.

For each budgeted command, the proposal is exactly **1.25 × median of three,
rounded up to 5 seconds, with a 5-second floor**. Slots and existing reasons
are preserved. A median exceeding its old budget gets an appended measured
reason; a new budget over 300 seconds needs a reason too. It emits a separate
proposed table and aggregate report, never edits the input table. Tests and
the summary runner are unchanged. `GATE_TARGET_S` remains **1,100 seconds**;
the existing recipe's margin remains 1.5 (1,650-second budget bound).

The analysis reproduces the runner's actual priority: **most slots first,
then largest budget, then recipe index**, taking the first command that fits.
It reports old/new budget models and old/new measured-median models, the
serial EXCLUSIVE tail, named virtual slot chains and the longest reservation
dependency chain. These are deterministic schedule models, **not observed
start timestamps**: the current timings file does not record those.
Changing budgets can change launch order, so a modeled improvement is not a
new measured gate result. A separate post-refit gate is planned to verify
that order change, without adding its timings to the three-run median.

Functional helper validation ran on now-idle node9901 with deterministic
synthetic inputs, not ORCD gate timings. Rounding/floor, reason/slot retention,
five refusal cases (failed command, duplicate timing index, differing budget,
changed enumeration, negative duration), and 500 seeded schedule comparisons
against the existing recipe checker algorithm passed. Named slot chains were
also checked for overlap and critical-chain duration. The initial validation
script incorrectly expected a reason annotation when none was required; that
script failure and the corrected successful validation are both retained.
No helper or EFS test was executed on the Mac. This base has no
`make evidence-check` target; invoking it returned "No rule to make target".
No evidence-policy PASS is claimed from that call. The separate architecture
checker is the applicable documentation check.

## Completed ORCD results and diagnosis

Both runs used **untouched base 394f2b791e3e8f9abc23f742dea754ad6f614a97**;
therefore these are baseline failures directly, not newly introduced branch
failures. No test was changed, excluded, or retried into a gate PASS.

- node9901: `TOTAL 508 FAIL 5 NOTRUN 5 STATUS FAIL (769s) BUILD 21s`.
  Build: `efsd 394f2b791 (branch devel, built 2026-10-27T01:32:42Z on node9901)`.
- fstor007 compatible prerequisite attempt:
  `TOTAL 508 FAIL 1 NOTRUN 5 STATUS FAIL (750s) OVERBUDGET 1 BUILD 22s`.
  Build: `efsd 394f2b791 (branch devel, built 2026-10-27T01:36:20Z on fstor007-mgmt)`.
  `test_k33_growth.py` measured 151.1 s against 135 s; this observation is
  preserved, not used to refit from ORCD.

**Node9901 failures:** `test_readindex_pair.py`, `test_host_outbox.py`,
`test_raft_ack_window.py` and `test_stage_evict_lru.py` failed in LeakSanitizer
with its fatal ptrace/runtime message. The assignment explicitly requires
separate diagnostics with `ASAN_OPTIONS=detect_leaks=0`; all four completed
with rc 0. Commands were `ASAN_OPTIONS=detect_leaks=0 python3 tests/test_NAME.py`
in the same source tree, with individual TMPDIRs. These diagnostics establish
no leak coverage and do not change the original FAIL verdict.
`test_qualify_driver.py:477` assumes its hardcoded target `node9901` is remote.
On that host `Runner.argv` correctly returns `['bash', '-c', 'echo hi']`
because `is_local('node9901')` is true. This is a hostname-dependent test
assertion defect. The actual argv and hostname are retained separately.

**Fstor007 failure:** `test_reclaim.c:903` uses `char cmd[256]` for a shell
cleanup containing three copies of its TMPDIR path. The actual command
needed 272 bytes; `snprintf` truncated it to a final `/.`, and the shell
cleanup refused to remove dot. The assertion `!system(cmd)` then aborted.
`reclaim-cleanup-diagnosis.json` records the exact path, full length, buffer
size and truncated command, without executing that reconstructed command.
This is a long-path test defect on the unchanged source; it does not
establish a reclaim product failure. Task J forbids modifying tests, so the
fix belongs to the coordinator rather than this budget-only branch.

**NOTRUN accounting on both hosts:** missing cluster lifecycle scripts,
missing `valgrind/memcheck.h`, and the fault-build-only stalled-compactor
case are three real prerequisite limits. The other two markers are the
leak/sanitizer driver self-tests verifying their own missing-tool refusal;
they are not evidence that the installed tools are absent. The gate is
loopback/local, not a multi-host RDMA qualification. Neither real cluster
nor its mounted data was stopped or changed.

## Retention and cleanup

After launchers and diagnostic commands completed, process executable/cwd
and mount checks showed no task-root users. Only this task's inactive `/tmp`
source/build/fixture trees, synthetic-validation scratch, diagnostic scratch
and private copied FUSE dependency were pruned with
`~/bin/edelete --force --delete DIR`. Relevant text logs, exact launch scripts,
helper validation scripts/logs, build identities and binary hashes remain
outside Git. Each output has `PRUNED.txt`; runtime-store replay is forfeited,
while the source and reproduction method remain available.

Closed `SHA256SUMS` manifests and their SHA-256 values:

| Host / attempt | Manifest path | SHA-256 |
| --- | --- | --- |
| node9901 | `/data1/efs/traces/taskJ-20261026/node9901/gate-394f2b791/SHA256SUMS` | `c6f1f18c65ff683bb5f8786ab15c0a04fcc9a1962030ee88dcb0dd13781ebf35` |
| fstor007 first prerequisite failure | `/home/erbmi1/efs-artifacts/taskJ-20261026/fstor007/gate-394f2b791/SHA256SUMS` | `db0ca10d3d19c8b95575dd566c6070f33fed97fc846097650bfdf9afd98a7cb0` |
| fstor007 compatible-FUSE gate | `/home/erbmi1/efs-artifacts/taskJ-20261026/fstor007/gate-394f2b791-fuse316/SHA256SUMS` | `a2d00ae42f1b789787196a46862b0c63ced3fb8c7e696b4bb6bd8fd536a0b086` |

The serial calibration launcher also passed an isolated node9901 synthetic
check: three successful stub gates retain separate results, while a first
failed stub stops before run 2. This was not an EFS product test or a
calibration measurement. Its scripts, logs and synthetic output are retained
under `/data1/efs/traces/taskJ-20261026/node9901/launcher-validation/`;
verified `SHA256SUMS` SHA-256:
`44a16d2ee18e9c0650cb4001c240d4e66b3c6b9ed255e8b2036a0805b2e98942`.
The synthetic scratch tree was pruned separately with edelete.

## Remaining verification plan (blocked by baseline failures)

1. After the two baseline findings are resolved in reviewed source, export one
   pinned current devel commit into
   `~/efs-taskJ/src`; build and run the three 8-job `test-all` gates through
   `scripts/make_test_summary.sh` serially in detached screen with the prepared
   `scripts/calibrate_test_budgets.sh FULL_COMMIT ARTIFACT_ROOT` launcher
   copied outside that pinned archive. The launcher stops on a failed gate
   or leftover task process; it never retries a failed measurement. Use the exact
   source/build identifiers, save each gate's summary, command enumeration,
   progress, timings, build/run logs and failure logs in its own directory
   under `~/efs-artifacts/taskJ-20261026/`. Preserve every verdict. Never use
   `/data1/efs`, running cluster ports, or another agent's test tree.
2. Check the helper against deterministic small inputs on that host, including
   rounding/floor, mismatched source command lists, failed/missing timings,
   retained reasons/slots and weighted scheduling. No test code is changed.
   Invoke the committed analysis with the three retained run directories:

   ```sh
   python3 scripts/refit_test_budgets.py \
     --host intel1 --budgets tests/budgets.tsv \
     --runner scripts/make_test_summary.sh \
     --output "$HOME/efs-artifacts/taskJ-20261026/refit" \
     "$HOME/efs-artifacts/taskJ-20261026/run-1" \
     "$HOME/efs-artifacts/taskJ-20261026/run-2" \
     "$HOME/efs-artifacts/taskJ-20261026/run-3"
   ```

3. Review/apply only the proposed `tests/budgets.tsv`; run
   `tests/test_make_test_recipe.py` and a separate post-refit gate. Report
   three measured walls, all command medians, the modeled critical chain,
   modeled gate vs D44, and any supported target reduction without changing
   the target. Preserve all failures and compare against unchanged base.
4. ORCD-first override is executing the real-host stage already: a clean
   archive under `/tmp` on **node9901**,
   `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, and one on **fstor007**, each with
   an 8-job `bash scripts/make_test_summary.sh` gate. Record their walls as
   separate host observations, not calibration inputs. Access goes through
   ORCD; stop for user authentication if a password/token prompt appears.
   No orcd-mini stop, format or deployment is authorized by this task.
5. Keep text evidence on each assigned host, hash a closed `SHA256SUMS`
   manifest and record its own hash. Prune only this task's inactive bulk and
   private build tree with `edelete`; write `PRUNED.txt`. No raw outputs enter
   Git. Update this page and W160 with actual results, then the coordinator
   can merge and run the merged-tree gate.

## Outstanding

- Resolution of J1/K22 and unclassified J2/K36; three clean Intel calibration runs on corrected source.
- Actual Intel budget refit (synthetic helper validation passed).
- Recipe check and post-refit gate on the released host.
- Coordinator fixes for the two baseline test defects; ORCD gate FAILs remain recorded.
- Per-command table, critical chain, exact build identities, closed manifests
  for a future successful calibration. Failed Intel and ORCD manifests and cleanup are complete above.

## Authorized follow-up on this branch

The user expanded Task J to investigate/fix J1 and J2 on this branch and
calibrate corrected code. The initial failed runs remain immutable. The
candidate changes only these two fixtures: K22 explicitly establishes the
pressured leader using bounded pre-load leadership transfer (including a
forced intervening node2 leadership), and K36 selects a victim from a live
file's packed metadata reference and holds segment rebuild until recovery
records that exact loss. Packed compaction stays outside this fixture's
observation window, as in the dedicated segment-recovery gate. No recovery
or client durability assertion is relaxed. Both tests now retain failed
fixtures after clean process/mount shutdown instead of deleting diagnostics.

These changes are candidates, not a new PASS claim. Standalone qualification
and a new pinned three-run calibration are pending. No product behavior,
pool slots, test deadlines or D44 target changed.

Candidate qualification is detached on intel1 in screen
`taskJ-fix-qualification`, source `/home/maik/efs-taskJ/src` at
`8f6cd4b5f065febc6893b1406ba5c8c55bfad55e`. Controller builds `test-bins`
then runs K22 and K36 serially three times each, stopping at the first
failure and copying retained diagnostic logs. Raw output:
`/home/maik/efs-artifacts/taskJ-20261026/intel1-fix-8f6cd4b5f/`.
Build branch is `agent/devel-task-j`; K36's existing keep hook is enabled
for these diagnostic runs. The first K22 setup successfully transferred
group0 to node2 and back to node1; the pressure assertions are still pending.
No final manifest or PASS is claimed while qualification is running.

### First candidate exposed W166; not qualified

Candidate8f6cd4b5f's first two K22 runs returned rc0 but each printed24
application EIOs before handoff. Those runs are **not qualification PASS**,
regardless of the old test's final PASS line. The first K36 run passed in
56.606s with a metadata-referenced fragment, exact identity in the missing/
mark logs, rebuild, and full fixture payload readback. It is one diagnostic
run, not three-run acceptance. Raw logs remain in the candidate directory.

The W166 mechanism is concrete: `host_pub_batch_propose` refuses publication
when `efs_kv_lsm_l0_hot` is true, while admission and the pump's leadership-
pressure signal consider only RAM/log capacity. L0 can remain full while
RAM still has room, leaving a leader refusing publication until flushes
time out. The next candidate shares the L0/RAM predicate between admission
and leadership; no Raft timing, client timeout, durability rule or setting
is changed. The live test now requires zero application errors throughout
pressure/handoff and observes the first KV/log capacity bound rather than
insisting that the log must fill after publication is already blocked. The
independent Raft follower-bound tests retain the log-watermark coverage.
A new fast extracted-code regression covers L0-full with free RAM, pressure
clearing, reserved cleanup, and the unchanged log watermark. W166 remains
OPEN until these stronger gates and calibration finish.

### Strict W166 candidate queued

Commit `4c4446e0225b2e09d5d4b5fac50b3e3654784c4e` is staged at
`intel1:/home/maik/efs-taskJ2/src`. Detached screen
`taskJ-w166-qualification` first waits for the earlier diagnostic controller
to end (no overlapping test load), then runs the new fast pressure unit
against the candidate and an expected-failure control against8f6cd4b5f's
unchanged production source. It builds, checks `test_follower_bound` and the
recipe, and requires three serial strict K22/K36 repetitions before the
new three-run full calibration. A failed stage stops the controller.

Raw qualification output is
`/home/maik/efs-artifacts/taskJ-20261026/intel1-fix-4c4446e02/`; proposed
calibration output is
`/home/maik/efs-artifacts/taskJ-20261026/intel1-calibration-4c4446e02/`.
The source is pinned throughout; the calibration launcher receives the
correct `agent/devel-task-j` build branch. Diagnostic K36 retention is
removed from the calibration environment, restoring ordinary successful-run
cleanup; failed fixtures are still retained by the corrected tests. No
calibration or W166 PASS is claimed yet. Both task-owned source trees are
kept temporarily for this open investigation; neither Task F tree is changed.

The strict candidate's first K22 run passed in28.758s: L0 was full while
RAM/log watermarks were false; admission refused, group0 moved to node3,
and473 client writes completed with zero errors throughout pressure/handoff.
This contrasts with the uncorrected production candidate's208s run with24
application EIOs. It is **one standalone run**, not full qualification. The
capacity unit, expected missing-predicate source control, follower-bound
checks and recipe check passed before it. K36 and the remaining repetitions
are active; full calibration has not started yet. No budgets changed.

### Strict repetitions complete; full calibration active

At exact source/build `4c4446e02`, all three strict repetitions passed:

| Repetition | K22 wall s | K36 wall s | Verdict |
| --- | ---: | ---: | --- |
| 1 | 28.758 | 73.104 | both rc0; zero K22 client errors |
| 2 | 21.144 | 69.820 | both rc0; zero K22 client errors |
| 3 | 20.151 | 67.448 | both rc0; zero K22 client errors |

K36 checked the exact metadata-referenced victim's missing/mark identity,
verified recovery, rebuilt it and read every fixture payload exactly in each
run. The normal/fault build is
`efsd 4c4446e02 (branch agent/devel-task-j, built 2026-10-27T03:56:35Z on intel1)`.
These are TCP loopback observations, not a new real-host/RDMA result. The
old application-EIO runs remain failed evidence and are not median inputs.

The controller automatically started full calibration run1 at eight jobs
after these checks. Its new fast regression makes the recipe509 commands.
No corrected full-gate result, budget fit or final closed manifest is claimed
until all three runs finish and their retained source/timings are validated.

### Full corrected gate stopped: operator-workflow EIO

The first full gate at `4c4446e02` ended **FAIL**: 509 commands, 1 FAIL,
14 NOTRUN, 738s, build0s. Command446 (`tests/test_operator_workflows.py`)
failed opening a file for creation immediately after requesting follower
resync, at line107; its wall42.8s exceeded its old30s estimate. Neither
this failed cost nor the gate wall is a calibration input. The controller
stopped before runs2/3. Raw summary, timing and traceback remain at
`intel1:/home/maik/efs-artifacts/taskJ-20261026/intel1-calibration-4c4446e02/run-1/`.
The original fixture deleted its daemon/client logs even on failure, so
that run's root cause is **unclassified**, not assumed to be W166.

One isolated diagnostic on the same binaries passed all operator checks;
it does not reverse the full-gate FAIL. It used `EFS_OPS_KEEP=1`, with raw
output and stores temporarily retained for this open investigation at
`intel1:/home/maik/efs-artifacts/taskJ-20261026/operator-4c4446e02/`.
The diagnostic change now records the exact operation and all three live
Raft status replies before cleanup, and automatically retains failed
fixtures. Assertions, deadlines, slots and product code are unchanged.
The next pool run is diagnostic, not permission to retry into PASS or
fit budgets while this failure remains unresolved.

Closed text-evidence manifests were re-read and verified after the controllers
exited (bulk remains separate, temporarily kept for diagnosis):

| Directory beneath intel1 `~/efs-artifacts/taskJ-20261026/` | Files | SHA256 of SHA256SUMS |
| --- | ---: | --- |
| `intel1-calibration-4c4446e02` | 19 | `0f7a215213bc2ae06de7b77b7f7c2962fb60a45f19a81d6d1e69015287756452` |
| `intel1-fix-4c4446e02` | 65 | `7bb0e66b7be48590da18de1bb033d363f85e0e41f6ed17305f87a5392f91e55f` |
| `intel1-fix-8f6cd4b5f` | 58 | `251faff021638f341e752a790b0a11af53d4045da93dd7e695f4ce1a70e69a91` |

The diagnostic pool is running at source `36db33f98`, in
`intel1:/home/maik/efs-taskJ3/src`, screen `taskJ-operator-pool-staged`.
Reproduction: archive that commit into a clean tree, set `FUSE_PREFIX=/usr`,
`EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 EFS_OPS_KEEP=1`,
and run `bash scripts/make_test_summary.sh` with the commit's build identity.
The source manifest contains2208 files. Output is
`intel1:/home/maik/efs-artifacts/taskJ-20261026/operator-pool-36db33f98-staged/`;
retained operator fixtures are beneath the source's `tmp/case-446/`.
This is diagnosis only, not one of the three calibration inputs.

An initial diagnostic launch raced an unfinished archive transfer and exited127
before any test ran (`scripts/make_test_summary.sh` absent). Its86-file
manifest and launcher error are preserved separately in
`operator-pool-36db33f98/`. The corrected launch waited for staging completion;
its build completed and the test pool started. This setup error supplies no
product result. No failed run or fixture has been pruned in this investigation.

### Diagnostic pool complete; EIO still unresolved

The diagnostics-only pool at `36db33f98` completed509 commands with **0 FAIL,
14 NOTRUN, STATUS INCOMPLETE**, wall731s plus build30s. The same Intel host
limits remain. Source manifest verification passed for all2208 archived
files. This run does not fix or replace the earlier EIO failure and is not
one of the calibration inputs. The retained operator logs show a group0
snapshot imported at index12, diff16 rows, in51ms from import-start to
installed, followed by successful workload and orderly fixture shutdown.

Its closed text manifest contains21 files, including the five operator
fixture logs copied to `operator-logs/`; SHA256 of `SHA256SUMS`:
`5d55bb5ad5c7759bc0e3781283a557943f5de1a5a7f0dc42f184e83e6133ee5f`.
The source and fixture bulk remain temporarily retained for the open EIO
investigation. No budget value or acceptance assertion changed.

A bounded **20-repetition diagnostic series** is now running serially on
those same binaries, stopping on its first error. Each invocation is
`TMPDIR=NEW_RUN/tmp python3 -u tests/test_operator_workflows.py`; the existing
keep-success override is removed, while failed fixtures are automatically
retained. Output is `intel1:~/efs-artifacts/taskJ-20261026/operator-series-36db33f98/`
(`stage`, `results.json`, numbered run logs and `exit`, plus any failed
fixture). Controller: `~/efs-taskJ3/operator-series.py`, screen
`taskJ-operator-series`. This series only seeks the missing failure trace;
clean repetitions cannot count as correcting the original failed gate.

### Resync/election race reproduced independently

The operator series finished20/20 without an error; this remains diagnostic,
not a fix for the original EIO. A separate deterministic in-memory probe
against the unchanged `36db33f98` library found a concrete resync race:
queue a valid leadership transfer to a caught-up follower, request resync
on that follower, then deliver the queued transfer. It becomes leader in
the next term while `resync_pending=1`. Probe output was
`resync=1 role=2 term_before=1 term_after=2 leader=1 target=1` (exit1).
Raw source/build/output is at
`intel1:~/efs-artifacts/taskJ-20261026/resync-transfer-probe/`.
This is a demonstrated defect; its connection to the earlier create EIO
is **not established** and no such causal claim is made.

Candidate `90345a46e` blocks both timer/pre-vote and transfer elections
while resync is pending, cancels an existing candidacy/pre-vote and read
proof when resync is requested, and refuses mutation during an owned
receive-persistence cohort. It keeps voting/replication and all durability
rules, formats, deadlines and knobs unchanged. The regression covers delayed
transfer, election timer and delayed vote grant; the snapshot test checks
that verified installation restores election eligibility.

The first candidate unit run exposed a regression-fixture mistake: the
post-install election check tried sending to an absent replica2 and got
INVAL. The same check failed on the old library too. The fixture now drops
that absent peer's vote traffic explicitly, preserving the OK/candidate/term
assertions; the first candidate unit FAIL remains retained at
`intel1:~/efs-artifacts/taskJ-20261026/resync-90345a46e/`. The old-library
negative control also demonstrated the actual pending-resync failures.
A corrected pinned candidate and live resync/full gates are still owed.
Neither the original EIO nor W160 is closed; no budgets have been fitted.

### Corrected resync candidate qualifying

Exact candidate `93d96f8e6d5f286f91342ef2532aa8a7b163e278` is archived at
`intel1:/home/maik/efs-taskJ4/src-93d96f8e6`. Controller
`~/efs-taskJ4/qualify-resync.py` in screen `taskJ-resync-qualification`
records ordered commands, walls and exits under
`~/efs-artifacts/taskJ-20261026/resync-93d96f8e6/`. It builds `test-bins`,
compiles the new regression against the old `36db33f98` library, requires
exactly four failures of the pending-resync check (transfer and delayed
vote in both acknowledgement-window modes), then runs the fixed complete
Raft unit in normal and early-read modes, follower-bound and recipe checks.
Those stages passed with the expected negative-control failure. The first
two live operator checks passed11.200s/12.171s; the third is in progress.
Next are snapshot-import, strict leader-admission, K36 recovery and one
full eight-job diagnostic gate, stopping at the first unexpected result.
The full gate remains **diagnostic**, not a median input or closure of the
unclassified original EIO. No active candidate source is modified.

The earlier20 clean operator repetitions took8.624–12.327s (median10.713s).
Their43-file closed text manifest was verified; SHA256 of SHA256SUMS is
`e789580d386dac9763b2331cf5d24d5fd64d5614f1d6811f2acc9d32eeefac58`.
Successful series fixtures were removed by their normal cleanup; raw run
logs and outcomes remain. The original failed captures/stores still kept
for open investigations have not been pruned.

### Operator EIO reproduced with retained state — qualification stopped

The third live operator check on `93d96f8e6` **FAILED** in41.834s at
`create f1, bytes=1`. The controller stopped; snapshot-import, K22, K36 and
the new full gate were **NOTRUN**. The earlier two operator successes and
unit successes remain scoped; the candidate is not qualified end to end.
Exact failed fixture (kept unchanged):
`intel1:~/efs-artifacts/taskJ-20261026/resync-93d96f8e6/operator-3-tmp/efs-ops-okaubhoy/`.
It includes `failure.json` with pre-shutdown statuses and all daemon/client
logs and stores. The election fix does not resolve this second failure.

The follower actually logged **successful** group0 snapshot installation
at index4, then stayed commit/applied4 while the leader reached19. Node1
reported405 rejections from that follower with hint0; the follower's leader
hint was-1, while group2 remained healthy at38. RAM/L0 pressure, admission
BUSY and apply-deferred counters were0, excluding the earlier L0-pressure
mechanism for this capture. Its repeated400ms apply waits blocked a local
PUT proof (67 PUT errors, no disk writes there). At30s the client recorded
a degraded fragment acknowledgement and REPORT BUSY, then flush EIO; its
unmount also failed its storage-fence stage. These are failures, not evidence
of a durable successful write. The mechanism after snapshot installation
still needs tracing; the original logless full-gate failure's exact cause
cannot be proven retrospectively merely from this matching symptom.

A new bounded20-run diagnostic on the same pinned source enables existing
`EFS_RAFT_DBG=1 EFS_RAFT_AE_DBG=1 EFS_OPID_TRACE=1 EFS_OPID_TRACE_VERBOSE=1`
without changing behavior/deadlines/assertions. It stops at its first error
and retains that fixture. Screen `taskJ-operator-debug`, controller
`intel1:~/efs-taskJ4/operator-debug.py`, output
`~/efs-artifacts/taskJ-20261026/operator-debug-93d96f8e6/`.
No qualification/calibration controller is still active. Budgets stay unchanged.

The failed `resync-93d96f8e6` capture's21-file text manifest is closed and
verified: SHA256 of SHA256SUMS
`968587537eff6d8cce1455f9e25a979a22d2f0e0d936e7fbd258a53a39e485c7`.
The manifest covers the controller/unit/operator results plus the failed
fixture's failure.json and five daemon/client logs; runtime stores remain
kept separately and unchanged for the open investigation.

The fully verbose series completed20/20 without reproducing the stall;
it supplies no qualification result. A separate20-run, fail-stopping
series now uses only `EFS_RAFT_AE_DBG=1` to reduce tracing overhead:
`intel1:~/efs-taskJ4/operator-ae.py`, screen `taskJ-operator-ae`, output
`~/efs-artifacts/taskJ-20261026/operator-ae-93d96f8e6/`. Source and all
assertions/timeouts remain exactly93d96f8e6. The purpose is the missing
AppendEntries transition trace after a successful snapshot, not additional
successful repetitions. No fit or post-refit gate has started.

### Pinpoint the stopped replica's fail-closed transition

The AE-only trace series also completed20/20 without a failure; it does not
close the reproduced EIO. Source inspection identifies another necessary
piece of evidence: `fence_storage` makes a replica follower with leader-1,
sets commit back to applied and refuses further Raft traffic, matching the
captured node2 status. That is a **hypothesis**, since the old function
logged no call site; the retained capture alone does not establish it.

Diagnostic commit `44cc4e3e227466aa3bec2d938444ca81851f1ced` adds one log line
only on the first storage-fence transition, naming its function/line, term,
commit/applied/snapshot and resync state. The fence and all decisions remain
unchanged. No flag, format change, timeout change or assertion change.
Source `intel1:~/efs-taskJ4/src-44cc4e3e2` is isolated from the failed93
source/stores. Controller `~/efs-taskJ4/fence-diagnostic.py`, screen
`taskJ-fence-diagnostic`, waits for the preceding trace series to finish,
then builds, runs full Raft units in both modes, and tries at most20 fresh
operator fixtures, stopping at its first failure. Verbose tracing is off.
Output is `~/efs-artifacts/taskJ-20261026/operator-fence-44cc4e3e2/`.
Build and both unit modes passed; the first three operator checks passed.
This is diagnostics only; no calibration or qualification claim follows
from clean repetitions, and no prior failed evidence is changed.

The minimal-fence build completed20 operator repetitions without failure;
its two Raft unit modes and all2208 tracked-source hash checks passed. These
are diagnostic results and do not resolve the captured EIO. Closed manifests:

| Diagnostic directory | Files | SHA256 of SHA256SUMS |
| --- | ---: | --- |
| `operator-debug-93d96f8e6` | 43 | `412cc972d7f4564f3dec1afb7f199cb5188eec8d1f17b3b9689daa8eb82afc11` |
| `operator-ae-93d96f8e6` | 43 | `255576277d94c89c5a4cf799671ecc01c7575f0265d7cc736fac90eaf5d57e9e` |
| `operator-fence-44cc4e3e2` | 29 | `c9143289e501bc08c69dd3ce2478673a1fbfa8e4225e2208eec9f0e49551aa45` |

A bounded100-fixture series now reuses that verified44cc4e3e2 build with
no verbose tracing, stops at the first error, and preserves its failed
fixture automatically. Controller `intel1:~/efs-taskJ4/fence100.py`, screen
`taskJ-fence100`; output
`~/efs-artifacts/taskJ-20261026/operator-fence100-44cc4e3e2/`. It verifies
all tracked files against the prior manifest before starting; each run is
`TMPDIR=NEW_DIRECTORY python3 -u tests/test_operator_workflows.py`. Successful
fixtures clean up normally. This larger bounded sample seeks the rare
failure's first fence call site; **even100 clean runs cannot stand in for a
root-cause correction**, and none are median inputs. No duplicate workload,
assertion, timeout, slot or budget change; no qualification controller is active.

### Compacted-prefix mechanism demonstrated; corrective candidate queued

A deterministic, socket-free probe against the **unchanged44cc4e3e2** library
installed a snapshot at4, delivered an old AppendEntries starting at0 with
entries1–5, then delivered a correct suffix starting at4. It reproduced:

- first storage fence: `at=on_ae_req:1868 term=1 commit=4 applied=4 snap=4 resync=0`;
- delayed-prefix rc-5; subsequent valid-suffix rc-1; applied stayed4.

The missing entries below the snapshot were treated as new appends; storage
correctly refused an index already compacted, and the receive path wrongly
turned that stale network request into a permanent storage fence. This
proves a concrete mechanism matching the captured stuck replica. The actual
packet sequence of the original logless full-gate failure remains unknown;
that failure stays recorded, rather than being relabelled a PASS.
Raw probe source/build/output:
`intel1:~/efs-artifacts/taskJ-20261026/compacted-prefix-probe-44/`.

Fix `58b87f0ed3b71f47c0e7af0518b1398d6bd35c5a` refuses a request whose
previous index is below the installed snapshot and returns that committed
boundary as a rebase hint. It does not append, truncate or apply the stale
prefix, and the leader can send the valid suffix from that boundary.
Resync-pending requests retain their explicit snapshot-pull behavior.
No durability, format, clock, timeout, slot or configuration change.
The new regression installs a real snapshot through the receive API, then
checks wholly old and overlapping batches, an empty old-prefix probe and
a mismatched boundary term, using both single and batch receive and both
existing acknowledgement-window modes. It requires unchanged snapshot
payload and successful subsequent exact suffix apply.

The exact source is staged at `intel1:~/efs-taskJ4/src-58b87f0ed`.
Controller `~/efs-taskJ4/prefix-qualification.py`, screen
`taskJ-prefix-qualification`, **waits for the existing100-fixture diagnostic**
before any build/test load. If that run fails at another fence origin it
stops for investigation instead of silently proceeding. Planned output:
`~/efs-artifacts/taskJ-20261026/prefix-58b87f0ed/` (`stage`, `results.json`,
per-command logs). It requires the new regression to fail on the old44
library only at the compacted-prefix checks, then fixed normal/early Raft
units, follower-bound, recipe,10 unchanged operator checks, snapshot import
and three strict K22/K36 pairs. Only after all those pass does the committed
launcher, copied outside the source, run three serial8-job full gates at
`~/efs-artifacts/taskJ-20261026/intel1-calibration-58b87f0ed/`. Every failure
stops and is retained. The candidate remains **NOTRUN/pending qualification**
while the predecessor diagnostic is active. No budgets have been fitted.

### Corrective candidate controls PASS; live qualification active

The predecessor100-fixture diagnostic completed without failure (8.456–13.109s,
median10.691s); this does not resolve the earlier failure by repetition.
It verified all2208 source files unchanged and finalized a106-file manifest,
SHA256 of SHA256SUMS:
`86cdb9d63a4470dd83500723c78ca6c8f61d5719c909c6ff42d0684cff1fd971`.
The deterministic unchanged-source failure probe is also closed and verified
(6 files), SHA256 of SHA256SUMS:
`72fca62a062732c0b6ad2db39be8972c053a6fd735f0f4528b88fb44e2b0d579`.

The waiting controller then built exact58b87f0ed in29.492s. Binary:
`efsd 58b87f0ed (branch agent/devel-task-j, built 2026-10-27T05:18:38Z on intel1)`.
The new complete unit linked against the unchanged44 library failed20
assertions, all confined to compacted-prefix rejection and survival of the
subsequent valid suffix; it had no unrelated failure. The fixed complete
Raft suite passed in normal and early-read modes (0.027/0.029s), and
follower-bound (0.017s) and recipe (1.028s) passed. Thus the corrective guard
has a real failing control, not just a source-text test. The first two live
operator checks passed8.811s/8.682s; remaining live checks are active.

The controller's original order and fail-stopping behavior remain unchanged:
10 operator runs, snapshot-import, three strict K22/K36 pairs, then three
full serial8-job calibration gates only if all preceding checks pass.
The exact old failure's packets were not captured, so causality is still
an inference from the demonstrated mechanism and matching stuck state.
Acceptance depends on the fixed-build gates, never on relabelling old FAILs.
No fit, target reduction, post-refit result or real-host/RDMA PASS yet.

All10 unchanged operator checks on58b87f0ed have now passed (8.682–12.163s),
including post-resync workload readback and the POSIX cases. The controller
is running snapshot-import; it has not started the subsequent strict pairs
or full calibration. These live checks extend the candidate evidence but
are not substitutes for the three full gates or the still-owed refit.

### Corrective live gates complete; full calibration started

At exact source/build58b87f0ed, snapshot-import passed in249.615s. Every
leader advanced its snapshot past the stopped node four times (197s);
node3 installed5s after resume, caught up6s after resume and drained every
commit2s after the load stopped. Peak RSS92,112KiB was below the627,068KiB
bound. **Five write-close EIOs were recorded during the intentional member
stall**, allowed by this test's existing B6 contract; those unsuccessful
files are outside its success manifest, not silently verified or retried
into PASS. All344 successfully published fixture files read back byte-exact
through node3. This is scoped catch-up evidence, not a zero-error workload
claim or verification of the five failed writes.

| Repetition | Strict K22 wall s | Client writes / errors | K36 wall s |
| --- | ---: | ---: | ---: |
| 1 | 21.791 | 531 / 0 | 77.557 |
| 2 | 23.261 | 546 / 0 | 61.815 |
| 3 | 21.732 | 479 / 0 | 57.428 |

Each strict K22 run observed L0-full with RAM/log watermarks false, admission
BUSY, leadership movement, drain after releasing the compactor and **zero
client errors throughout**. Each K36 run passed its original crash/torn/
missing-fragment recovery, exact referenced segment-loss/mark/rebuild and
payload checks. Normal/early Raft controls and10 operator checks already
passed. All2208 archived source files were verified unchanged before the
controller copied the committed calibration launcher outside the source
and started run1. No overlapping diagnostic or build controller remains.

Authoritative full-gate progress:
`intel1:~/efs-artifacts/taskJ-20261026/intel1-calibration-58b87f0ed/`
(`stage`, `run-1` through `run-3`); parent controller remains
`~/efs-taskJ4/prefix-qualification.py`, screen `taskJ-prefix-qualification`.
The source, budgets and pool slots remain pinned throughout these three
serial8-job gates. No full-gate result, fitted median, budget reduction or
post-refit PASS is claimed yet. Earlier failed baselines remain FAIL and
all failing fixtures remain kept for traceability. TCP loopback/direct-I/O
fixture evidence does not supply a new repaired-build real-host/RDMA gate.

### Second corrective full gate failed: packed read after compaction crash

The first full eight-job gate of58b87f0ed stopped with **509 commands,
1 FAIL,14 NOTRUN,729s, build0s**. K36 step8 readback after the injected
KV-compaction crash raised EIO. Runs2/3 never started and these timings are
not calibration inputs. Its earlier three standalone K36 passes remain scoped
results, not a full-gate PASS.

The retained client log identifies inode57345,901 bytes, packed segment
`g0/e24c517a4e3cb802/1`: node1 NET(-6), node2 valid, node3 NOT_FOUND(-3).
Node3's log independently records this segment published degraded during its
reclaim crash and marked for rebuild. Node1 had restarted and verified before
the read. No Raft storage-fence diagnostic appears for this failure. The
captured logs do not prove whether node1's NET was its cooldown or a failed
connection; that distinction remains a live-evidence limit.

Code inspection found a concrete client defect: the packed reader returns
the last unsuccessful fragment's verdict. Its existing returned-member
re-probe ran only when that verdict was NET. A later absent fragment changed
the verdict to NOT_FOUND and bypassed that recovery path. New deterministic
cases in `tests/test_get_integrity.py` extract the production readers and
use framed socketpair replies: one down-marked returned holder, one valid
holder and one absent fragment, in all six holder/absence orders. The new
test against unchanged58 source fails exactly on the first expected-success
assertion. Corrective49e5ffd71 also permits the existing bounded re-probe on
NOT_FOUND; all six cases pass, while unreachable returned holders still
return EIO and all integrity-refusal cases remain passing. This proves the
mixed-verdict defect, not retrospectively the captured client's cooldown.
No timeout, durability, assertion, slot or budget was relaxed.

Exact candidate `49e5ffd7116fa0bfca0273aa5202126d26226e55` is archived separately
at `intel1:~/efs-taskJ4/src-49e5ffd71`. Ordered controller
`~/efs-taskJ4/packed-qualification.py`, screen `taskJ-packed-qualification`,
runs build, GET integrity, normal/early Raft and recipe, then three strict
K36/K22 pairs and three operator checks. Only their success permits three
new serial8-job full gates. Output:
`~/efs-artifacts/taskJ-20261026/packed-reprobe-49e5ffd71/` and, if reached,
`intel1-calibration-49e5ffd71/`. Fixed-source live qualification is pending;
budgets and target1100s/bound1650s remain unchanged.

Failed stores remain unchanged for this open investigation at
`intel1:~/efs-taskJ4/src-58b87f0ed/tmp/case-462/efs-k36-6l9j3zwl/`.
Diagnostic log copies are under the stable artifact root, directory
`k36-compaction-58b87f0ed`. Closed manifests were re-read and verified:

| Directory under `intel1:~/efs-artifacts/taskJ-20261026/` | Files | SHA256 of SHA256SUMS |
| --- | ---: | --- |
| `intel1-calibration-58b87f0ed` |17| `f9fc220139cac7a988ad11edee30553ce7b098d3630943a360329b5f8c28ab3a` |
| `prefix-58b87f0ed` |32| `1b53ae350911fec400b3521cffe508e71863adc7b54cb9240215cf25e034f6ad` |
| `k36-compaction-58b87f0ed` |10| `3f23d0b9e048fdb67cd6ec9b3b8f00fc7e6faabeb09b2e7f5ba54b28956e6113` |

The failed runtime stores are deliberately outside those text manifests and
remain retained, not pruned. Earlier original/operator failures stay FAIL.

### Mixed-result correction: standalone qualification complete

Exact49e5ffd71 binary built `2026-10-27T05:47:34Z on intel1`, branch
`agent/devel-task-j`. Build29.602s, production GET-integrity0.262s,
normal/early full Raft0.032/0.028s and recipe0.939s all passed. Results:

| Repetition | K36 wall s | Strict K22 wall s | Client writes / errors | Operator wall s |
| --- | ---: | ---: | ---: | ---: |
|1|80.583|21.813|505 /0|12.240|
|2|84.928|20.909|538 /0|12.643|
|3|53.102|21.461|400 /0|11.614|

All three K36 runs passed compaction-crash readback and the original later
certificate, unhosted-group and referenced-segment rebuild checks; these
standalones do not relabel the failed58 pool. All2208 archived source files
were verified unchanged before the controller started full calibration
run1. Authoritative stage is now
`intel1:~/efs-artifacts/taskJ-20261026/intel1-calibration-49e5ffd71/stage`.
It will stop at any failed full gate. Three complete successful gates,
refit and the separate post-refit gate are still owed; budgets unchanged.

### Three complete Intel measurements and fitted budgets

Pinned49e5ffd71 completed all three serial8-job gates with **509 commands,
zero FAIL,14 NOTRUN each**, measured walls **729/723/725s**, build0s (the
candidate was built before timing). No failed diagnostic or ORCD timing
entered this fit. Run2's growth command took137.5s against its135s budget;
its three-run median106.3s yields the same135s fitted budget. This individual
overrun remains recorded; the fit follows the requested median rule.

The offline helper initially refused the command comparison. The only
command-text differences were four inline compiles' `EFS_BUILD_TIME` ISO
strings, generated by make during each enumeration; those four test sources
do not inspect that macro. Correctionf5c065e8a canonicalizes only the exact
`-DEFS_BUILD_TIME='"YYYY-MM-DDTHH:MM:SSZ"'` value for cross-run identity.
Raw commands and timings remain untouched. Synthetic checks accept that
one difference and refuse changed optimization flags, source files, build
IDs or malformed timestamp text. No product, test, runner, slot or deadline
changed for this helper correction. The new helper lives outside the measured
archive at `intel1:~/efs-taskJ4/scripts/refit-f5c065e8a.py`.

Reproduction (after the three measurements and source verification):

```sh
cd /home/maik/efs-taskJ4/src-49e5ffd71
python3 /home/maik/efs-taskJ4/scripts/refit-f5c065e8a.py \
  --budgets tests/budgets.tsv --runner scripts/make_test_summary.sh \
  --host intel1 --output /home/maik/efs-artifacts/taskJ-20261026/refit-49e5ffd71 \
  /home/maik/efs-artifacts/taskJ-20261026/intel1-calibration-49e5ffd71/run-1 \
  /home/maik/efs-artifacts/taskJ-20261026/intel1-calibration-49e5ffd71/run-2 \
  /home/maik/efs-artifacts/taskJ-20261026/intel1-calibration-49e5ffd71/run-3
```

All85 budget rows retain their slots and original reason prefixes;57 budgets
change. Each new budget is exactly1.25 times the median, rounded upward to5s,
minimum5s. No median exceeded its old budget, so no such extra reason was
needed. `test_make_test_recipe.py` passes locally: structural budgeted gate
1024s of1650s; its make enumeration is platform-dependent. The following
model uses the actual509-command Intel enumeration (including1s defaults
for unbudgeted commands), not that structural enumeration:

| Model | Pool s | Exclusive tail s | Total s |
| --- | ---: | ---: | ---: |
| Old budgets |1508|136|1644|
| Fitted budgets |889|136|1025|
| Measured medians under old priority |657.6|69|726.6|
| Measured medians under fitted priority |657.6|69|726.6|

Fitted budget total1025s is75s below the unchanged1100s target and625s below
its1650s bound. This is a modeled capacity budget, **not an observed post-refit
wall**. Measured walls729/723/725s are independent observations. The modeled
median total does not change here even though command ordering changes.
The full schedules, dependencies, virtual-slot reservations and both critical
chains are retained in `refit-49e5ffd71/report.json`.

The fitted-budget virtual-slot dependency chain is listed below. It follows
dependencies between reservations, not one pinned CPU or observed test starts.

| Command on modeled critical chain | Budget s |
| --- | ---: |
| `python3 tests/test_report_stepdown.py` |235.0|
| `python3 tests/posix/posix_expiry.py --private` |165.0|
| `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py` |275.0|
| `python3 tests/test_k38_protection_now.py` |70.0|
| `python3 tests/test_k42_peer_stop_open.py` |45.0|
| `python3 tests/test_d39_model.py --shard 0/3` |25.0|
| `python3 tests/test_orcd_verify_resume.py` |10.0|
| `EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta` |5.0|
| `./tests/test_kv_insertion_buffer` |5.0|
| `./tests/test_apply_wait` |1.0|
| `python3 tests/test_raft_log_entries.py` |1.0|
| `python3 tests/test_env_removed.py` |1.0|
| `python3 tests/test_direct_io_required.py` |1.0|
| `python3 tests/faults/test_create_retry_fault.py` |1.0|
| `./tests/test_put_storage_fence` |1.0|
| `./tests/test_data_commit` |1.0|
| `python3 tests/test_chown_permissions.py` |1.0|
| `python3 tests/test_xattr_names.py` |1.0|
| `python3 tests/test_lookup_replacement.py` |1.0|
| `python3 tests/test_readindex_result.py` |1.0|
| `python3 tests/test_lookup_group_plan.py` |1.0|
| `python3 tests/test_host_cover_route.py` |1.0|
| `python3 tests/test_query_rpc_routing.py` |1.0|
| `python3 tests/test_session_cleanup_cli.py` |1.0|
| `python3 tests/test_lease_edge_rpc.py` |1.0|
| `python3 tests/test_put_ticket_batch.py` |1.0|
| `python3 tests/test_retained_create_authority.py` |1.0|
| `python3 tests/test_pending_create_command.py` |1.0|
| `python3 tests/test_pressure_cohort.py` |1.0|
| `python3 tests/test_pressure_background.py` |1.0|
| `python3 tests/test_soak_driver.py` |1.0|
| `python3 tests/test_wb_fault.py` |1.0|
| `python3 tests/test_getchunks_budget.py` |1.0|
| `python3 tests/test_report_retry_eof.py` |1.0|
| `python3 tests/test_inode_pending_eof.py` |1.0|
| `python3 tests/test_client_ready.py` |1.0|
| `python3 tests/test_writer_view_rpc.py` |1.0|
| `python3 tests/test_truncate_prefix.py` |1.0|
| `python3 tests/test_unlink_verdict.py` |1.0|
| `python3 tests/test_write_queue.py` |1.0|
| `python3 tests/test_small_frame_receive.py` |1.0|
| `python3 tests/test_append_oracle.py` |1.0|
| `python3 tests/test_get_pair_readiness.py` |1.0|
| `python3 tests/test_session_read_proof.py` |1.0|
| `python3 tests/test_read_failure_admission.py` |1.0|
| `python3 tests/test_publication_writeback.py` |1.0|
| `./tests/test_data` |1.0|
| `python3 tests/test_raft_slow_sync.py` |1.0|
| `python3 tests/test_host_retire.py` |1.0|
| `./tests/test_name_index` |1.0|
| `./tests/test_writer_routing` |1.0|
| `./tests/test_session_owned_capture` |1.0|
| `python3 tests/test_append_prepare_batch.py` |1.0|
| `./tests/test_raft` |1.0|
| `python3 tests/test_txn_apply_retry.py` |1.0|
| `python3 tests/test_query_page_barriers.py` |1.0|
| `python3 tests/test_close_retire_batch.py` |1.0|
| `python3 tests/test_reap_fairness.py` |1.0|
| `python3 tests/test_query_loaded_oracle.py` |1.0|
| `python3 tests/test_query_target_route.py` |1.0|
| `python3 tests/test_rename_budget.py` |1.0|
| `./tests/test_pack_ledger_recovery` |1.0|
| `./tests/test_member_view` |1.0|

The exclusive tail runs serially after the pool. Its full sequence and
measured medians are below (NOTRUN costs are not runtime qualification):

| Exclusive command | Budget s | Measured median s |
| --- | ---: | ---: |
| `bin=$(mktemp "${TMPDIR:-/tmp}/efs-rdma-message.XXXXXX"); trap 'rm -f "$bin"' EXIT; \` |1.0|0.3|
| `python3 tests/test_rdma_trace.py` |1.0|0.1|
| `python3 tests/test_direct_read_fork_oracle.py` |1.0|0.6|
| `python3 tests/test_cluster_stop.py` |1.0|0.0|
| `python3 tests/test_rdma_memcheck.py` |1.0|0.0|
| `python3 tests/test_bench_lookup_walk.py` |1.0|0.1|
| `python3 tests/test_shutdown_bounded.py` |1.0|0.7|
| `python3 tests/test_rdma_reply_budget.py` |1.0|0.2|
| `python3 tests/test_rdma_progress_ready.py` |1.0|0.1|
| `python3 tests/test_rdma_setup_reentry.py` |1.0|0.1|
| `python3 tests/test_rdma_fork_poller.py` |1.0|0.0|
| `./tests/test_rdma_xprt` |1.0|0.0|
| `./tests/test_rdma_segmented normal` |1.0|0.0|
| `./tests/test_rdma_segmented interrupt` |1.0|0.0|
| `./tests/test_rdma_segmented tcp-violation` |1.0|0.0|
| `./tests/test_rdma_segmented deadline` |1.0|0.0|
| `./tests/test_rdma_segmented burst` |1.0|0.0|
| `./tests/test_rdma_segmented burst-asym` |1.0|0.0|
| `./tests/test_rdma_segmented churn` |1.0|0.0|
| `./tests/test_bench_latency` |1.0|0.0|
| `python3 tests/test_gc_reap_shutdown.py` |1.0|0.1|
| `python3 tests/test_shutdown_blocked_io.py` |15.0|11.4|
| `python3 tests/test_client_processes.py` |20.0|13.3|
| `python3 tests/test_shutdown_stuck_worker.py` |10.0|6.8|
| `python3 tests/test_opid_admission.py` |15.0|11.8|
| `python3 tests/test_server_processes.py` |10.0|5.5|
| `python3 tests/test_bench_cli.py` |5.0|3.2|
| `python3 tests/test_bench_profile.py` |5.0|2.5|
| `python3 tests/test_leak_gate.py` |5.0|1.5|
| `python3 tests/test_storage_root_inventory.py` |5.0|2.2|
| `python3 tests/test_session_latch_tsan.py` |5.0|1.5|
| `python3 tests/test_make_test_summary_runner.py` |10.0|4.7|
| `python3 tests/test_make_test_recipe.py` |5.0|1.2|
| `python3 tests/test_stage_evict_lru.py` |5.0|1.1|

All per-budget-command observations and medians follow. `slack`, `tight` and
`within-margin` compare the old budget with the measured median; none is a
new correctness verdict. The leak/sanitizer gate rows time their Python
self-tests, which deliberately emit missing-tool markers: they do not
measure a full real leak or sanitizer gate. Valgrind exists on intel1 but
its development header is absent; IB/RDMA and cluster-script skips remain
NOTRUN. These85 rows do not claim every support dimension is qualified.

| Command | Run 1 s | Run 2 s | Run 3 s | Median s | Old s | New s | Slots | Assessment | NOTRUN runs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| `python3 tests/test_shutdown_blocked_io.py` | 11.4 | 11.4 | 11.4 | 11.4 | 15 | 15 | 1 | within-margin | none |
| `python3 tests/test_w153_formation.py` | 8.7 | 10.4 | 9.1 | 9.1 | 15 | 15 | 1 | within-margin | none |
| `python3 tests/test_k33_growth.py` | 106.3 | 137.5 | 96.1 | 106.3 | 135 | 135 | 2 | within-margin | none |
| `python3 tests/test_k33_autoid.py` | 29.7 | 28.9 | 29.1 | 29.1 | 45 | 40 | 1 | within-margin | none |
| `python3 tests/test_k33_client_view.py` | 18.8 | 19.7 | 21.9 | 19.7 | 70 | 25 | 2 | slack | none |
| `python3 tests/test_k33_interrupt.py` | 65.5 | 64.7 | 36.1 | 64.7 | 70 | 85 | 1 | tight | none |
| `python3 tests/test_k33_join_retry.py` | 6.6 | 6.3 | 7.1 | 6.6 | 15 | 10 | 1 | slack | none |
| `python3 tests/test_k33_payload.py` | 33 | 32.1 | 26.6 | 32.1 | 240 | 45 | 2 | slack | none |
| `python3 tests/test_k35_fsync_degraded.py` | 51 | 72.5 | 61.2 | 61.2 | 95 | 80 | 1 | within-margin | none |
| `python3 tests/test_k37_segment_rebuild.py` | 194.2 | 227.8 | 195.7 | 195.7 | 335 | 245 | 1 | within-margin | none |
| `python3 tests/test_k41_packer_placement.py` | 88.6 | 88.5 | 86.7 | 88.5 | 110 | 115 | 1 | tight | none |
| `python3 tests/test_k38_protection_now.py` | 53.5 | 54.2 | 55.2 | 54.2 | 90 | 70 | 1 | within-margin | none |
| `EFS_B_PHASES=grow-a python3 tests/test_b_geometry.py` | 54.5 | 54.2 | 55.7 | 54.5 | 120 | 70 | 3 | slack | none |
| `EFS_B_PHASES=grow-b python3 tests/test_b_geometry.py` | 65 | 63.8 | 71.1 | 65 | 260 | 85 | 3 | slack | none |
| `EFS_B_PHASES=full python3 tests/test_b_geometry.py` | 52.9 | 47.4 | 46.8 | 47.4 | 75 | 60 | 2 | within-margin | none |
| `python3 tests/test_k33_seed_loss.py` | 46 | 48.1 | 45.2 | 46 | 70 | 60 | 1 | within-margin | none |
| `python3 tests/test_k33_bookkeeping.py` | 34.9 | 36.7 | 34.9 | 34.9 | 65 | 45 | 2 | within-margin | none |
| `python3 tests/test_k34_stop_put.py` | 38.4 | 38.2 | 38.2 | 38.2 | 55 | 50 | 1 | within-margin | none |
| `python3 tests/test_k42_peer_stop_open.py` | 36.4 | 35.9 | 35.7 | 35.9 | 90 | 45 | 1 | slack | none |
| `python3 tests/test_k38_shared_chunk_pressure.py` | 37.7 | 24.3 | 27 | 27 | 120 | 35 | 2 | slack | none |
| `python3 tests/test_stop_progress.py` | 34.3 | 42.5 | 36.8 | 36.8 | 165 | 50 | 3 | slack | none |
| `python3 tests/test_operator_workflows.py` | 11.8 | 9.7 | 10.5 | 10.5 | 30 | 15 | 1 | slack | none |
| `python3 tests/test_member_limit.py` | 16.1 | 13.2 | 14.7 | 14.7 | 25 | 20 | 1 | within-margin | none |
| `EFS_B6_MODES=down python3 tests/test_b6_unavailable.py` | 43.2 | 42.3 | 46.7 | 43.2 | 75 | 55 | 1 | within-margin | none |
| `EFS_B6_MODES=refuse python3 tests/test_b6_unavailable.py` | 41.3 | 33.5 | 32.6 | 33.5 | 65 | 45 | 1 | within-margin | none |
| `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py` | 218.4 | 215.3 | 238.2 | 218.4 | 275 | 275 | 1 | within-margin | none |
| `EFS_B6_MODES=full python3 tests/test_b6_unavailable.py` | 50.9 | 51 | 44.9 | 50.9 | 125 | 65 | 2 | slack | none |
| `python3 tests/test_d4_progress.py` | 72.2 | 76.4 | 77.8 | 76.4 | 260 | 100 | 2 | slack | none |
| `python3 tests/test_d4_plateau.py` | 139 | 145 | 137.5 | 139 | 180 | 175 | 2 | within-margin | none |
| `python3 tests/test_pack_dup_candidate.py` | 75.6 | 61.7 | 71.8 | 71.8 | 105 | 90 | 1 | within-margin | none |
| `python3 tests/test_pub_unit_forward_abort.py` | 23.6 | 18.9 | 19.8 | 19.8 | 60 | 25 | 1 | slack | none |
| `python3 tests/test_conn_idle_reap.py` | 56 | 54.1 | 54.9 | 54.9 | 80 | 70 | 1 | within-margin | none |
| `python3 tests/test_report_stepdown.py` | 187 | 163.1 | 190.6 | 187 | 440 | 235 | 6 | slack | none |
| `python3 tests/test_query_first_catalog.py` | 16.1 | 14 | 16.2 | 16.1 | 35 | 25 | 1 | slack | none |
| `python3 tests/test_client_processes.py` | 13.3 | 13.2 | 13.4 | 13.3 | 20 | 20 | 1 | within-margin | none |
| `python3 tests/test_shutdown_stuck_worker.py` | 6.8 | 6.8 | 6.8 | 6.8 | 10 | 10 | 1 | within-margin | none |
| `python3 tests/test_k36_recovery.py` | 80.8 | 71 | 98 | 80.8 | 120 | 105 | 1 | within-margin | none |
| `python3 tests/test_expiry_policy_live.py` | 31.1 | 29.1 | 30.2 | 30.2 | 45 | 40 | 1 | within-margin | none |
| `python3 tests/posix/posix_expiry.py --private` | 131.9 | 127.3 | 133.1 | 131.9 | 175 | 165 | 3 | within-margin | none |
| `python3 tests/test_k36_segment_recovery.py` | 124.2 | 125.6 | 124.3 | 124.3 | 160 | 160 | 1 | within-margin | none |
| `./tests/test_reclaim` | 12.4 | 12.2 | 12.3 | 12.3 | 35 | 20 | 1 | slack | none |
| `python3 tests/test_opid_admission.py` | 11.8 | 11.8 | 11.8 | 11.8 | 15 | 15 | 1 | within-margin | none |
| `EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta` | 4.5 | 7.1 | 6.8 | 6.8 | 40 | 10 | 1 | slack | none |
| `python3 tests/test_raft_ack_window.py` | 7.7 | 7.7 | 7.7 | 7.7 | 30 | 10 | 1 | slack | none |
| `./tests/test_kv_lsm` | 6.9 | 7.2 | 6.8 | 6.9 | 35 | 10 | 1 | slack | none |
| `python3 tests/test_server_processes.py` | 5.5 | 5.5 | 5.6 | 5.5 | 10 | 10 | 1 | within-margin | none |
| `python3 tests/test_w125_harness.py` | 10.7 | 10.1 | 10.7 | 10.7 | 20 | 15 | 1 | within-margin | none |
| `./tests/test_append_transaction` | 5 | 5.4 | 5.5 | 5.4 | 40 | 10 | 1 | slack | none |
| `python3 tests/test_orcd_verify_resume.py` | 5.1 | 5.1 | 5.1 | 5.1 | 10 | 10 | 1 | within-margin | none |
| `./tests/test_kv_lsm_concurrent 4` | 4.1 | 4.3 | 4.1 | 4.1 | 10 | 10 | 1 | slack | none |
| `./tests/test_txn_retire` | 4.7 | 4.5 | 4.5 | 4.5 | 20 | 10 | 1 | slack | none |
| `./tests/test_pub_stage` | 0.3 | 0.3 | 0.3 | 0.3 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_d39_model.py --shard 0/3` | 16.4 | 16.5 | 16.4 | 16.4 | 65 | 25 | 1 | slack | none |
| `python3 tests/test_d39_model.py --shard 1/3` | 10.3 | 10.1 | 10.1 | 10.1 | 45 | 15 | 1 | slack | none |
| `python3 tests/test_d39_model.py --shard 2/3` | 4.1 | 3.8 | 3.9 | 3.9 | 20 | 5 | 1 | slack | none |
| `python3 tests/test_bench_cli.py` | 3.2 | 3.2 | 3.1 | 3.2 | 5 | 5 | 1 | within-margin | none |
| `python3 tests/test_bench_profile.py` | 2.6 | 2.5 | 2.5 | 2.5 | 5 | 5 | 1 | within-margin | none |
| `python3 tests/test_query_subtree_negative.py` | 3.3 | 3.3 | 3.6 | 3.3 | 20 | 5 | 1 | slack | none |
| `python3 tests/test_query_sort_peak_negative.py` | 3.6 | 3.4 | 3.6 | 3.6 | 20 | 5 | 1 | slack | none |
| `EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta` | 2.7 | 2.4 | 2.5 | 2.5 | 15 | 5 | 1 | slack | none |
| `python3 tests/test_raft_rotate_stall.py` | 2.7 | 2.8 | 2.7 | 2.7 | 10 | 5 | 1 | slack | none |
| `python3 tests/test_leak_gate.py` | 1.5 | 1.5 | 1.6 | 1.5 | 5 | 5 | 1 | slack | 1,2,3 |
| `python3 tests/test_append_sequence.py` | 2.4 | 2.4 | 2.4 | 2.4 | 5 | 5 | 1 | slack | none |
| `./tests/test_store_durability` | 1.4 | 2.4 | 1.4 | 1.4 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_storage_root_inventory.py` | 2.2 | 2.3 | 2.2 | 2.2 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_efs_commands.py` | 2.3 | 2.4 | 2.2 | 2.3 | 10 | 5 | 1 | slack | none |
| `python3 tests/test_client_memory.py` | 2.2 | 2.3 | 2.1 | 2.2 | 10 | 5 | 1 | slack | none |
| `python3 tests/test_storage_principles.py` | 1.9 | 2.1 | 1.9 | 1.9 | 10 | 5 | 1 | slack | none |
| `python3 tests/test_session_latch_tsan.py` | 1.5 | 1.5 | 1.6 | 1.5 | 5 | 5 | 1 | slack | none |
| `./tests/test_kv_insertion_buffer` | 2.2 | 2.3 | 2.2 | 2.2 | 10 | 5 | 1 | slack | none |
| `python3 tests/test_make_test_summary_runner.py` | 4.7 | 4.7 | 4.8 | 4.7 | 10 | 10 | 1 | slack | none |
| `./tests/test_query_disk_io` | 2.1 | 2.2 | 2.1 | 2.1 | 10 | 5 | 1 | slack | none |
| `./tests/test_raft_store` | 1.6 | 1.6 | 1.5 | 1.6 | 10 | 5 | 1 | slack | none |
| `./tests/test_meta_apply` | 1.5 | 1.5 | 1.5 | 1.5 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_put_retry_budget.py` | 1.5 | 1.5 | 1.5 | 1.5 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_make_test_recipe.py` | 1.2 | 1.2 | 1.2 | 1.2 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_stage_evict_lru.py` | 1.1 | 1 | 1.1 | 1.1 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_create_retry_ack.py` | 1.1 | 1.1 | 1.1 | 1.1 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_apply_ring.py` | 1.3 | 1.2 | 1.2 | 1.2 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_posix_phase_trace.py` | 0.9 | 0.9 | 0.9 | 0.9 | 5 | 5 | 1 | slack | none |
| `python3 tests/test_sanitizer_gate.py` | 1.7 | 1.8 | 1.7 | 1.7 | 120 | 5 | 1 | slack | 1,2,3 |
| `python3 tests/test_w159_apply_wait.py` | 52.5 | 53.6 | 54.3 | 53.6 | 70 | 70 | 2 | within-margin | none |
| `python3 tests/test_decoder_refusal.py` | 5.6 | 4.9 | 4.2 | 4.9 | 120 | 10 | 1 | slack | none |
| `python3 tests/test_k22_snapshot_import.py` | 247.3 | 253.4 | 229.8 | 247.3 | 380 | 310 | 2 | within-margin | none |
| `python3 tests/test_k22_leader_admission.py` | 21.5 | 29.2 | 23.9 | 23.9 | 290 | 30 | 2 | slack | none |

The fitted table is now applied on the isolated Task J branch. A separate
post-refit full gate is still required and will not become a fourth median
input. No fresh repaired-build RDMA/ORCD or unrestricted alpha PASS is claimed.

### Final post-refit result, retention and cleanup

The separate gate at exact `0f72c166557fdafe16822a9826d5ab3bd4d619ef`
passed every runnable check: **509 commands,0 FAIL,14 NOTRUN,730s**, plus
30s build (whole wrapper759.622s). Binary `efsd0f72c1665`, branch
`agent/devel-task-j`, built `2026-10-27T06:32:22Z on intel1`.
Recipe check passed1.091s (structural1024s of1650s). Both measurement and
post-refit source manifests verified all2208 files unchanged. This gate is
excluded from the three-input medians. It supplies no measured speedup:
730s is comparable to the729/723/725s observations. The change removes
inflated budget declarations, while retaining the requested safety margin;
it does not change execution slots or claim the modeled1025s was observed.

All14 NOTRUN markers remain in each summary, so the runner honestly labels
these gates INCOMPLETE despite zero failures. Missing RDMA/device/header and
cluster-script coverage is not transformed into PASS. Intentional missing-
tool markers from leak/sanitizer gate self-tests are distinguished from real
runtime coverage above. Earlier ORCD stages remain baseline FAILs and their
raw evidence/limits remain unchanged. W166's reproduced L0-pressure subcase
is fixed and strictly qualified on loopback TCP; the separate fault-window
stalled-follower write errors and production-scale/RDMA scope remain open.

Raw roots below are absolute under
`intel1:/home/maik/efs-artifacts/taskJ-20261026/`. All manifests were closed,
re-read and verified after controllers finished:

| Directory | Files | SHA256 of SHA256SUMS |
| --- | ---: | --- |
| `intel1-calibration-49e5ffd71` |46| `9f3162159f86cdcc423c75f2c384b9710b43d23f599aea621ca348074b1d63cc` |
| `packed-reprobe-49e5ffd71` |26| `b7184eccdd72a87e170e10365bfb556a943270a80aced13b35a5b444f619fe2e` |
| `refit-49e5ffd71` |3| `d48b35ccc1063fb5ba2f2f20fcdb5fbe7c93b03e558f9964e3326288f54303d1` |
| `post-refit-0f72c1665` |18| `5a9e11557fdbaf0c55d34fcea3732c4008fda6352fe1e1604a90d04959765672` |
| `exact-builds` |45| `3b5391e3238c3da89b95e2ccf05c55b57f3b8da075df462e0a712670e081f1db` |

`exact-builds` retains nine full-commit source tarballs, eight exact tested
binary/library tarballs with per-member hashes, copied controllers/tools and
`PRUNED.txt`. Every archive opens; each binary member was re-hashed against
its source file before pruning. The49/0f source archives also match every
file in their measured2208-file manifests. These archives are206MiB on Intel;
no payload/store export to the Mac occurred.

After proving no process executable/cwd or mount used each target, eight
inactive source/build trees were removed using
`~/bin/edelete --force --delete PATH`: the `src` trees under `efs-taskJ`,
`efs-taskJ2`, `efs-taskJ3`, and `efs-taskJ4/src`, plus Task J4's93,44,49 and0f
source trees. Their exact paths are in `exact-builds/PRUNED.txt`; earlier
source-path references on this page are historical and now resolve through
the retained archives. Controllers/text evidence remain. No Task F tree was
touched. No Task J daemon, client or mount remains.

The failed58 K36 fixture remains unchanged at
`/home/maik/efs-taskJ4/src-58b87f0ed/tmp/case-462/efs-k36-6l9j3zwl/`;
the captured93 operator stores remain at
`/home/maik/efs-artifacts/taskJ-20261026/resync-93d96f8e6/operator-3-tmp/efs-ops-okaubhoy/`.
They are deliberately retained for the unresolved exact-capture causality;
this does not negate the deterministic negative controls or fixed-build
qualification, nor turn the old failures into PASS. Task J's remaining
416MiB source tree contains that failed58 fixture. No shared devel edit,
merge, push, Task F evidence, real cluster or devops repository changed.

### Task M: additional current-tree fstor007 measurement — Oct27

[Task M](gate-fstor007-20261027.md), tested7ca545e2b with the unchanged
063154e0e devel product and documented libfuse3.12+ prerequisite satisfied
by the supplied private3.16.2 library: **514 commands,0 FAIL,5 NOTRUN,
INCOMPLETE,617s, build0s**, after a separately measured21.664s build.
No timing here is an input to the Intel refit; budgets/slots/D44 unchanged.
Seven per-command budget overruns and all coverage limits remain on its page.
This adds the requested third completed real-host measurement, across two
ORCD hosts: historical node9901 Task J769s (FAIL), historical fstor007
Task J750s (FAIL), current fstor007 Task M617s (zero failures, scoped skips).
These are different source/test sets, not a controlled speedup comparison.
Task J's original system-FUSE and compatible-FUSE failures stay unchanged.
Task M's final raw manifest is
`fstor007:/home/erbmi1/efs-artifacts/taskM-20261027/fstor007/candidate-7ca545e2b/SHA256SUMS`,
SHA256 `542913e6b2c99cf0f7853f916fb2affa6a477b23e81b8809a670e3ae723d04ce`.
