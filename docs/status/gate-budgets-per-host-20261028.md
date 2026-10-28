# Task T — gate budgets across supported hosts

`agent/devel-task-t`, implementation `292034404`, base devel `b2d57e421`.
Full-gate TOTAL line unavailable: calibration and confirmation NOTRUN, hosts
busy. Evidence `docs/status/gate-budgets-per-host-20261028.md`. Synthetic tool
regression PASS on intel2; no product change or product finding. Task T INCOMPLETE.

## Scope and status

W160 / D44: derive one budget table from three clean eight-job gates per
host, using 1.25 times the **maximum host median**, rounded upward to five
seconds with a five-second floor. Preserve slots/reason prefixes and the
user's `GATE_TARGET_S=1100`; never raise it to accommodate the fit.

The fitting tool and its fast regression are committed. No replacement
`tests/budgets.tsv` has been fitted or committed. Calibration, the modeled
new total, per-row host medians and confirming gates remain NOTRUN. This is
not a gate PASS or a completed calibration.

At the latest read-only preflight nuc is running Task H's full gate
(attempt 71; retained older Task H mounts also remain). Intel2 is running
merge-wave29's gate. Neither host is quiet enough for calibration. No Task H
fixture, running gate or shared cluster has been stopped or modified.
There is no detached Task T calibration queued that could overlap these runs.
The measurements begin only after host handoff and a quiet preflight.

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
