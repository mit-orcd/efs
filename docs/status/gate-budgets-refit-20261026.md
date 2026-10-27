agent/devel-task-j (analysis 28d6659dc, base 394f2b791): ORCD gates RUNNING; first fstor007 attempt TOTAL 0 FAIL 1 NOTRUN 0 (libfuse prerequisite). Intel calibration NOTRUN pending F release. Evidence: docs/status/gate-budgets-refit-20261026.md. No product defect established; tests/budgets unchanged.

# W160 three-run budget refit — October 26, 2026

**REAL-HOST GATES RUNNING, NOT COMPLETE.** No measured budget is changed and no gate PASS is
claimed. The user assigned Task J from `parallel-tasks-20261026.md`; its updated
instructions retain intel1/intel2 after Task F/E releases the host and add
one real-host gate each on node9901 and fstor007. The user explicitly
overrode that order: run both ORCD gates first, then three Intel calibration
runs after Task F releases intel1.

## Resource and source scope

Planned calibration host: **intel1, after Task F's explicit handoff**. Task F
is active and has started its unchanged-base build there. A lightly loaded
host does not establish that its reservation has ended. No Task J source,
build, cluster or measurement has been started on either Intel host. The
coordinator can collect this report from branch `agent/devel-task-j`; no merge
or push was performed. The shared devel checkout and its uncommitted D39 edit
are untouched. The preflight base is devel `394f2b791`; pin current devel
again at the eventual calibration start if the coordinator has merged fixes.

Intel measurements, per-command medians and the budget refit remain
**NOTRUN**; no historical timing is reused as a new observation. ORCD gates
use fresh untouched devel **394f2b791** archives, build identity
`EFS_GIT_ID=394f2b791`, `EFS_GIT_BRANCH=devel`,
`EFS_GIT_DESCRIBE=394f2b791`. Exact version lines, completed command counts,
walls and closed gate manifests remain pending. Historical evidence stands
on its own.

## ORCD-first execution

The user explicitly requests this order; it does not release intel1 early.
Agent-forwarded SSH through `orcd-login` works without typing a password or
token. Each run is detached in screen, with a new private `/tmp` source tree.
Neither running cluster stores nor listeners on 17432–17435 are changed.
No devops/cluster repository is edited. Tests retain their own dynamically
reserved loopback ports. No test, pool slot or deadline is changed.

| Host | Actual environment | Task J run |
| --- | --- | --- |
| node9901 | RHEL kernel 5.14.0-503.38.1.el9_5.x86_64; 192 logical CPUs in affinity; MemTotal 395217884 KiB; GCC 11.5.0; Python 3.9.25; supplied libfuse 3.16.2 | 8 build jobs / 8 test slots, running |
| fstor007 (`fstor007-mgmt`) | RHEL kernel 5.14.0-687.36.1.el9_8.x86_64; 96 logical CPUs in affinity; MemTotal 791576532 KiB; GCC 11.5.0; Python 3.9.25 | 8 build jobs / 8 test slots, fresh prerequisite-corrected gate running |

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

Current source/launcher roots and externally retained text evidence:

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
  logs and per-failure logs are saved independently. No final gate manifest
  is claimed while its files are open. Scratch trees remain during diagnosis;
  prune only this task's inactive bulk after recording the final method.

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

Only static Python syntax parsing has been done on the Mac. Functional
validation of the helper and all EFS builds/tests remain for the released
host. This base has no `make evidence-check` target; invoking it returned
"No rule to make target". No evidence-policy PASS is claimed from that call.
The separate architecture checker is the applicable documentation check.

## Remaining calibration plan (Intel not executed)

1. After intel1's handoff, export one pinned current devel commit into
   `~/efs-taskJ/src`; build and run the three 8-job `test-all` gates through
   `scripts/make_test_summary.sh` serially in detached screen. Use the exact
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

- Intel host handoff; three measured calibration runs and safe failure handling.
- Functional analysis-helper validation and actual budget refit.
- Recipe check and post-refit gate on the released host.
- Completion/review of node9901 and fstor007 real-host gates, preserving failures.
- Per-command table, critical chain, exact build identities, closed manifests
  and cleanup receipts. No raw archive or manifest hash is invented here.
