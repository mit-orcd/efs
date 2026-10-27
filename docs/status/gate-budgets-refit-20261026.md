Branch agent/devel-task-j, tested head4c4446e02. Base4066f74df: TOTAL508 FAIL2 NOTRUN14 (616s); strict fixed K22/K36 3/3 each, zero client errors. Full corrected calibration FAIL: operator-workflow create EIO (509 commands, 1 FAIL, 14 NOTRUN, 738s); diagnosis active. Evidence: docs/status/gate-budgets-refit-20261026.md. W166 candidate unifies L0 leadership/admission; qualification pending. Original Intel/ORCD FAILs retained; no budget fit.

# W160 three-run budget refit — October 26, 2026

**INCOMPLETE: BASELINE FAILED; AUTHORIZED FIXES UNDER QUALIFICATION.** No measured budget is changed and no gate PASS is
claimed. The user assigned Task J from `parallel-tasks-20261026.md`; its updated
instructions retain intel1/intel2 after Task F/E releases the host and add
one real-host gate each on node9901 and fstor007. The user explicitly
overrode that order: run both ORCD gates first, then three Intel calibration
runs after Task F releases intel1.

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
2/3. Per-command three-run medians, refit and post-refit gate remain NOTRUN;
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
