agent/devel-task-j @ 28d6659dc: gate TOTAL unavailable (NOTRUN: E/F host release pending); base 394f2b791, no measured comparison. Evidence: docs/status/gate-budgets-refit-20261026.md. Three calibration gates, refit and ORCD gates NOTRUN. Product defects: none established; no test or product changes.

# W160 three-run budget refit — October 26, 2026

**PREPARED, NOT COMPLETE.** No measured budget is changed and no gate PASS is
claimed. The user assigned Task J from `parallel-tasks-20261026.md`; its updated
instructions retain intel1/intel2 after Task F/E releases the host and add
one real-host gate each on node9901 and fstor007 after calibration.

## Resource and source scope

Planned calibration host: **intel1, after Task F's explicit handoff**. Task F
is active and has started its unchanged-base build there. A lightly loaded
host does not establish that its reservation has ended. No Task J source,
build, cluster or measurement has been started on either Intel host. The
coordinator can collect this report from branch `agent/devel-task-j`; no merge
or push was performed. The shared devel checkout and its uncommitted D39 edit
are untouched. The preflight base is devel `394f2b791`; pin current devel
again at the eventual calibration start if the coordinator has merged fixes.

No `efsd --version` build line, test count, measured wall, per-command median
or raw manifest exists yet. They are **NOTRUN**, not zero, guessed or reused
from historical runs. The historical gate-speed evidence remains independent.

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

## Measurement plan (not executed)

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
4. Updated real-host stage: a clean archive under `/tmp` on **node9901**,
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
- node9901 and fstor007 real-host gates, subject to availability/access.
- Per-command table, critical chain, exact build identities, closed manifests
  and cleanup receipts. No raw archive or manifest hash is invented here.
