# Task T — gate budgets across supported hosts

`agent/devel-task-t`, implementation `292034404`, base devel `b2d57e421`.
Nuc first attempt: **TOTAL 523 FAIL 7 NOTRUN 12 STATUS FAIL (1461s)
OVERBUDGET 45 BUILD 50s**. Baseline comparison RUNNING; intel1 first K43
pool attempt RUNNING; intel2 calibration and confirmations NOTRUN. Evidence `docs/status/gate-budgets-per-host-20261028.md`. Synthetic tool
regression PASS on intel2; no product change or product finding. Task T INCOMPLETE.

## Scope and status

W160 / D44: derive one budget table from three clean eight-job gates per
host, using 1.25 times the **maximum host median**, rounded upward to five
seconds with a five-second floor. Preserve slots/reason prefixes and the
user's `GATE_TARGET_S=1100`; never raise it to accommodate the fit.

The fitting tool and its fast regression are committed. No replacement
`tests/budgets.tsv` has been fitted or committed. Nuc run 1 failed; the
controller stopped before run 2. Corrected intel1 measurements are RUNNING; intel2
measurements, the modeled total and confirming gates remain pending. This is
not a gate PASS or a completed calibration.

Current source is frozen at `babeea6404df2008d4393a461adb4480f9acab65`,
after merging devel `3debca7ec` at `234b30775`. Its archive SHA256 is
`3a3cf46a5faa62fbd383f8709d2883e3428f38efc8c635f15750ab5ae8c7abb7`.
The canonical graph was rebuilt per the merged AGENTS instructions; its nine
navigation tests PASS. No product source or budget value was changed.

The user requested another availability check and continuation to completion.
Nuc's Task H gate had finished; repeated checks found no active EFS/build/test
processes. Four older retained Task H mounts remain untouched. Intel1 and
xorinox were quiet, but their timing cannot substitute for the requested nuc
and intel2 measurements. Intel2 still had overlapping Task W/merge gates; none
of those overlapping observations will be used as clean calibration inputs.

Nuc is now running three serial eight-job gates in detached screen
`taskT-nuc-babeea`, owned source `~/efs-taskT-20261028-babeea640/src`, output
`~/efs-artifacts/taskT-20261028/nuc-babeea640/`. Its build succeeded and the
first gate finished FAIL with failures recorded below. A five-second process monitor records any competing
EFS/compiler/make workload; a contaminated run is rejected and no subsequent
calibration run starts. Failures are preserved, never retried by the controller.
Only owned fixtures are created; no Task H mount or existing cluster is stopped.
Intel2 has no Task T workload yet.

The source archive has no Git metadata. A private index was initialized from
its exact tracked-path list for `test_code_graph.py`'s `git ls-files` check;
this does not alter source bytes. Explicit build variables identify the actual
frozen source commit, not the temporary index commit. Per-file source hashes
are recorded before/after, plus every run's command list, timing, summary,
budget table, preflight, binary hashes, version and exit status. The external
controller's exact source is retained beside the output as `controller-source.py`.

Task J's twelve copied timing/summary/budget/command files have been checked
against its original closed SHA256 manifest. Those small inputs are kept
outside Git at `/Users/mike/Documents/Codex/artifacts/taskT-20261028/inputs/intel1/`.
No payloads or runtime stores were copied.

## Coverage decision

Task J's three clean intel1 measurements are retained at
`/home/maik/efs-artifacts/taskJ-20261026/intel1-calibration-49e5ffd71/run-{1,2,3}`.
Their totals are 509 commands, zero FAIL and 14 NOTRUN each (729/723/725 s).
They predate four current commands entirely:

- `python3 tests/test_k43_coordinator_kill.py`
- `python3 tests/test_w130_dir_fault.py`
- `python3 tests/test_w166_authority.py`
- `python3 tests/test_dir_ops_busy.py`

The initial coverage audit missed K43. The user chose nuc/intel2-only coverage
for W130, W166 and directory BUSY retries, then explicitly required **three
intel1 pool measurements for K43**. Three new full eight-job intel1 gates
are therefore planned on the same frozen `babeea640` source; the controller
stops on failure. They also observe the other newer rows. Keep these fresh
observations separate from Task J's older cohort; never label an observed
cell unmeasured or silently combine different within-host command sequences.
The final fit must state which complete cohorts it uses.

Intel1 series started at approximately 19:32 UTC October 28, screen
`taskT-intel1-babeea`, source `~/efs-taskT-20261028-babeea640/src`, evidence
`~/efs-artifacts/taskT-20261028/intel1-babeea640/`. No competing test/build
job under the test account was present. Existing infrastructure owned by
`efs` was left untouched; the process guard covers competing jobs under
`maik`, not all activity by other users. This limitation applies to quiet-host
classification and must be reviewed against the final preflights.

## Observed nuc failures — qualification remains incomplete

Run 1 finished **523 commands, 7 FAIL, 12 NOTRUN, 45 OVERBUDGET**,
1461 s test wall and 50 s build. These failures are not calibration samples:

| Command | Observed failure |
| --- | --- |
| `test_d4_progress.py` | One foreground 1 MiB write+fsync took 11.04 s against 10 s; reported payload verification had no mismatches. |
| `test_d4_plateau.py` | Packer reported 11 PUT errors; no read errors in the final sample. |
| `test_pub_unit_forward_abort.py` | One write+fsync took 3.20 s against 3 s. |
| `test_w130_dir_fault.py` | `raft-resync` returned `rc=-13` after the injected fault. |

Two more runtime/setup failures were observed near the end: `test_w166_authority.py`
failed (full diagnostics retained), and `test_qualify_driver.py` rejected its
output as inside a Git worktree. The latter was caused by this session's
synthetic Git index setup, not a product regression. That index made the graph
scope test run but changed the qualification-driver environment. This attempt
is invalid for calibration even apart from its runtime failures. Devel's
`c0081d095` now reports the graph scope check NOTRUN for Git-free archives;
the next cohort will use that fix and no synthetic index. The same setup was
used for the ongoing intel1 attempt, so it cannot count as a clean cohort.
Keep its K43 observations as attempted pool evidence only.

The seventh failure, `test_make_test_recipe.py`, detected the unwired
`test_code_graph.py`; devel already fixed that in `8106dab79`.

Raw command outputs were copied to `nuc-babeea640/run-1/`. The closed text
manifest covers 30 files, all rechecked; `SHA256SUMS` SHA-256 is
`5099b1004ab99fbe584e39a17b8b602d9aeb784b23d0b84260d79c23ddbfa29f`.
The controller rechecked 2,282 tracked source files with no changes. Surviving failed fixtures remain for investigation; the original D4 tests
self-cleaned their stores, so their retained stdout is not a recoverable
store capture. The baseline enables D4 retention explicitly. No deadline, slot count,
workload or product behavior has been relaxed. The process monitor has not
reported competing same-account workloads at this checkpoint; that is not
proof of no external I/O. An untouched same-host baseline comparison is
RUNNING at devel `fe18612ae` in `~/efs-taskT-base-fe18612ae/src`, evidence
`~/efs-artifacts/taskT-20261028/nuc-base-fe18612ae/`, screen
`taskT-nuc-base-fe`. It is one full eight-job gate, no synthetic Git index;
`EFS_D4_KEEP=1` retains D4 diagnostics without changing workload or bounds. No claim of a Task T regression or a product fix is made.

Task T merged current wave-29 devel at `9638c34e0`, retaining upstream fixes
for archived-source graph testing, W130 operator resync pre-admission BUSY,
and failed plateau-fixture retention. The graph was rebuilt and all nine
navigation tests PASS; the multi-host fitter regression and 52-row register
checker PASS. Relative to untouched `fe18612ae`, selected source changes
remain only the fitter, its test and one Makefile test invocation: no product
code or Task T budget/slot change. New devel budget rows for the graph and
K48 tests are inherited, and need measured coverage in the fresh cohort.

## Tool change and validation

`scripts/refit_test_budgets.py` accepts repeatable `--host-runs HOST RUN1 RUN2
RUN3`. Each host needs three distinct clean result directories with the same
command sequence and recorded budget priorities within that host. Timestamp
normalization remains limited to the informational compile-time stamp;
compiler flags, source/build identity or workload differences are not erased.
Different host command sets are matched by the exact budgeted command. The
fullest measured gate is the scheduling reference, so an older, shorter
intel1 gate cannot remove newer commands from the model.

`--unmeasured HOST COMMAND` declares a specific absent cell. Unknown hosts or
commands, undeclared gaps, stale declarations for observed commands, failed
commands, changed within-host priorities and invalid durations are refused.
The report includes every observation, host median, limiting host, explicit
coverage gap and NOTRUN marker. Marker-bearing command costs are not proof
that their skipped subcases ran. The deterministic slot model is kept separate
from measured wall time and does not claim real process start timestamps.

The new `tests/test_refit_test_budgets.py` passes on intel2 (Linux
6.12.0-211.67.1.el10_2 x86_64, Python 3). It uses synthetic result directories,
not a live cluster: it distinguishes max host median from pooled median,
last/first host and a single-run outlier; checks decimal rounding and the
five-second floor, preserved slots/reason prefixes, explicit gaps, invalid
inputs, differing host command sets and the CLI/model reference. It is wired
into the fast test recipe. No long-test command, slot or deadline changes.

Exact check:

```sh
python3 tests/test_refit_test_budgets.py
```

Closed regression log: `intel2:/home/maik/efs-artifacts/taskT-20261028/tool-check/refit.log`.
SHA256: `b33b051e73e9e5a8f9543cf6196d7f12a0e9f13c4018a8a31743a68653239ed8`.
No baseline/full-gate comparison is yet claimed. The small synthetic check
finished before intel2's later merge gate started.

## Remaining measurements and reproduction

On a frozen current-develop source in an owned tree, run the committed
`scripts/calibrate_test_budgets.sh FULL_COMMIT ARTIFACT_ROOT BRANCH` on quiet
nuc, then quiet intel2. It records three serial eight-job gates, preserves
failures and stops on failure. Retain commands, timings, summaries, source
and binary identities, budget tables and host preflights. Do not use failed
or overlapping gates as successful workload costs. Compare any failure with
the untouched same-host base rather than adjusting tests to manufacture PASS.

If all three fresh intel1 gates are clean, they provide actual observations
for all current rows, so the fitting invocation needs no missing-cell
declarations. Keep the historical Task J cohort separate:

```sh
python3 scripts/refit_test_budgets.py \
  --budgets tests/budgets.tsv --runner scripts/make_test_summary.sh \
  --host-runs intel1 INTEL1_RUN1 INTEL1_RUN2 INTEL1_RUN3 \
  --host-runs nuc NUC_RUN1 NUC_RUN2 NUC_RUN3 \
  --host-runs intel2 INTEL2_RUN1 INTEL2_RUN2 INTEL2_RUN3 \
  --output NEW_ARTIFACT_DIRECTORY
```

Commit the fitted rows only after reviewing coverage and the modeled gate
against 1100 s (and its existing margin bound). Report the limiting rows if
the target is exceeded; leave D44 unchanged. Then run one confirming gate on
nuc and one on intel2. Record every OVERBUDGET outcome; a median fit does not
guarantee a single run cannot exceed it. Do not retry or widen budgets merely
to obtain zero overruns. Final raw manifests, source verification, cleanup
receipts and both confirming gate lines remain pending. Nothing is pushed
or merged into devel.


## Corrected measurements and retained invalid attempts

Intel1's original `babeea640` attempt finished **TOTAL 523 FAIL 2 NOTRUN 12
STATUS FAIL (727s) OVERBUDGET 4 BUILD 30s**. Both failures were the synthetic
index / qualification-driver interaction and unwired graph test. K43 exited
zero in 55.3 s in that pool, but this is an attempted observation, **not one
of the three required clean measurements**. Source verification found no
tracked changes. Three corrected full serial pools started on `9638c34e0`
without Git metadata, screen `taskT-intel1-9638`, evidence
`intel1:/home/maik/efs-artifacts/taskT-20261028/intel1-9638c34e0/`.
All current rows are observed; historical Task J data stays separate.

Intel2's `9638c34e0` attempt was interrupted during build after the guard
observed Task H starting independent daemons after our preflight. Only
Task T's child processes were terminated, with their identities recorded
in `INTERRUPTED.json`. No test timings exist and no calibration run counts.
The controller exited with missing-summary diagnostics after termination;
this is an interrupted/contaminated setup, not a product test failure.
Task H's processes/stores were left untouched. Further intel2 measurement
must wait for its queued work to finish and a new quiet preflight.

Closed evidence roots (under each host's `/home/maik/efs-artifacts/taskT-20261028/`):

| Host / root | Files | SHA-256 of checked `SHA256SUMS` |
| --- | ---: | --- |
| nuc / `nuc-babeea640` | 30 | `5099b1004ab99fbe584e39a17b8b602d9aeb784b23d0b84260d79c23ddbfa29f` |
| intel1 / `intel1-babeea640` | 25 | `ae857a0b8a3e6ca7517115281fca17b6bdfd019206ea7662523c4f4bbcfb76c4` |
| intel2 / `intel2-9638c34e0` | 22 | `c0eb7c392361a4d02923a734eca15aa13a999a33587b24afb24e1c522f4401d1` |

The untouched nuc baseline has already observed a POSIX expiry failure:
`restricted` disappeared before `stat` in `expiry_lifecycle.py`. Its fixture
is retained. Baseline is not a clean calibration input, regardless of later
results; its final summary and the D4 comparison remain pending. Task U also
records an independent untouched-base D4 plateau failure and retained stores
([report](k32-write-cost-profile-20261028.md)); that issue remains OPEN even
when an individual rerun passes. No Task T product fix or relaxed gate is
claimed.
