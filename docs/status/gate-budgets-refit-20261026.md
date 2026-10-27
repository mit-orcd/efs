Branch agent/devel-task-j, tested candidate58b87f0ed: regression controls, 10 operator checks, snapshot catch-up and 3 strict K22/K36 pairs PASS (TCP loopback scoped). Snapshot fault window recorded5 contract-allowed write errors;344 successful payloads exact. Three full8-job calibration runs ACTIVE. Earlier Intel/ORCD FAILs/stores remain preserved; budgets unchanged. Evidence: docs/status/gate-budgets-refit-20261026.md.

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
