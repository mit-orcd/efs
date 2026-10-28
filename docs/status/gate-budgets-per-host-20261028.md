# Task T — gate budgets across supported hosts

`agent/devel-task-t`, implementation `292034404`, base devel `b2d57e421`.
Full-gate TOTAL line pending: nuc calibration RUNNING; intel2 calibration
and confirmations NOTRUN. Evidence `docs/status/gate-budgets-per-host-20261028.md`. Synthetic tool
regression PASS on intel2; no product change or product finding. Task T INCOMPLETE.

## Scope and status

W160 / D44: derive one budget table from three clean eight-job gates per
host, using 1.25 times the **maximum host median**, rounded upward to five
seconds with a five-second floor. Preserve slots/reason prefixes and the
user's `GATE_TARGET_S=1100`; never raise it to accommodate the fit.

The fitting tool and its fast regression are committed. No replacement
`tests/budgets.tsv` has been fitted or committed. Nuc calibration is RUNNING;
intel2 measurements, the modeled total and confirming gates remain pending. This is
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
first gate is RUNNING. A five-second process monitor records any competing
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
They predate these commands entirely:

- `python3 tests/test_w130_dir_fault.py`
- `python3 tests/test_w166_authority.py`
- `python3 tests/test_dir_ops_busy.py`

The user explicitly chose: **“Use nuc/intel2 for those three rows and mark
intel1 unmeasured.”** Those three cells will be null/unmeasured, never zero
or imputed. All other rows require all three hosts; any additional missing
cell is an error rather than an expanded exception. No additional intel1
workload is authorized or planned by this decision.

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

Once inputs exist, the fitting invocation is:

```sh
python3 scripts/refit_test_budgets.py \
  --budgets tests/budgets.tsv --runner scripts/make_test_summary.sh \
  --host-runs intel1 INTEL1_RUN1 INTEL1_RUN2 INTEL1_RUN3 \
  --host-runs nuc NUC_RUN1 NUC_RUN2 NUC_RUN3 \
  --host-runs intel2 INTEL2_RUN1 INTEL2_RUN2 INTEL2_RUN3 \
  --unmeasured intel1 'python3 tests/test_w130_dir_fault.py' \
  --unmeasured intel1 'python3 tests/test_w166_authority.py' \
  --unmeasured intel1 'python3 tests/test_dir_ops_busy.py' \
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
