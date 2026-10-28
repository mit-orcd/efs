# Task T — gate budgets across supported hosts

**INCOMPLETE / awaiting nuc and intel2 release.** The multi-host fitter is implemented;
no fitted budget table or confirming gate is accepted. Three clean intel1
pools now supply the required K43 measurements: 54.9/54.9/55.0 s, median
54.9 s. Untouched `fe18612ae` fails the eight-job nuc gate. Later devel
`424d0db8f` arrived after these frozen runs and is not qualified by them.
Nuc/intel2 are occupied by other qualification work. Nothing is pushed or merged into devel.

## Contract and source

W160 / D44: three clean serial eight-job gates per supported host. Fit each
row to `ceil_to_5s(1.25 × max(host medians))`, minimum five seconds. Preserve
slots and reason prefixes. Keep `GATE_TARGET_S=1100`; do not raise it to fit
the measurements. Report model time separately from measured wall time.
A failed or contaminated gate never becomes a calibration sample.

Branch `agent/devel-task-t`: fitter implementation `292034404`; completed intel1
measurement source `9638c34e0273fe9ff291ab7deae7ec1940516886`, merging
wave-29 devel `fe18612ae`. Against that untouched base, selected source
changes are only the fitter, its regression and one Makefile invocation:
no product-code or Task T budget/slot change. Devel's graph/K48 budget rows
are inherited. All nine rebuilt graph checks, the fitter regression and
52-row K-register checker pass locally; docs check passes 5/5.

Source archives contain tracked files, with explicit build ID/description
and branch variables. Corrected archives have **no Git index**. The graph
scope subcase reports NOTRUN there, per devel `c0081d095`; full local graph
validation remains separate. Archive SHA-256:

- Task T `9638c34e0`: `4aaf2ded75cfe46f198722edde12fe5da45710244622c39764850872e86a4ce2`.
- Untouched `fe18612ae`: `6b0968f3d2addfbe5baa986dc9a524ee603718303d4c4666f4f110e4a309016f`.

The branch now includes newer devel `424d0db8f` through `da9509fdd`, with
receipt/stripe/Raft repairs. Scoped component checks on devel are not a full
Linux gate; old failed `fe18612ae` results do not certify or condemn this new
source. Future cohorts will freeze a new commit once the hosts are released.
D4 progress now keeps its fixture only on failure, matching the already merged
plateau retention rule. Passing-test cleanup, workload and assertions stay the
same; this avoids losing the next failed fixture without changing calibration
costs with an always-keep environment override.

## Coverage and fitter

Task J's historical intel1 source `49e5ffd71` has three zero-failure gates
(729/723/725 s, 509 commands, 14 NOTRUN each). Its copied small inputs were
checked against the original manifest. They predate K43, W130, W166 and
`dir_ops_busy`, plus the newly inherited graph/K48 rows. The first coverage
audit missed K43; that omission is corrected here.

The user allowed nuc/intel2-only measurements for W130/W166/directory BUSY,
then specifically required **three intel1 pool measurements for K43**. The
fresh intel1 pools observe every current row, including those three. Keep
that actual coverage; do not label observed cells unmeasured or mix its
command sequence with Task J's older cohort. Historical inputs remain at
`/Users/mike/Documents/Codex/artifacts/taskT-20261028/inputs/intel1/` and
`intel1:/home/maik/efs-artifacts/taskJ-20261026/intel1-calibration-49e5ffd71/`.

Marker refusal is hardened in `db4908040`: a contamination/interruption
marker rejects even a zero-failure timing file; focused regression PASS.

`scripts/refit_test_budgets.py` accepts repeatable `--host-runs HOST RUN1
RUN2 RUN3`. It requires three distinct clean directories per host, identical
within-host command sequences and recorded priorities, valid unique timing
indices and finite nonnegative durations. Only the informational build-time
stamp is normalized. Different host cohorts match exact budget commands;
the fullest cohort supplies the scheduling reference. Every current row
must be measured on every host unless explicitly declared `--unmeasured`.
Unknown/stale declarations and all-host gaps are refused; absence is never
zero. Reports retain observations, medians, limiting host and skip markers.

Regression `python3 tests/test_refit_test_budgets.py` distinguishes the maximum
of host medians from pooled medians or one outlier; checks rounding, floor,
slots/reasons, explicit gaps, failures, changed priorities and CLI/reference
coverage. Linux regression PASS on intel2, log
`/home/maik/efs-artifacts/taskT-20261028/tool-check/refit.log`, SHA-256
`b33b051e73e9e5a8f9543cf6196d7f12a0e9f13c4018a8a31743a68653239ed8`.
It uses synthetic inputs, not a cluster qualification.

## Actual runs

All full gates use `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8`
with `EFS_FUSE_PREFIX=/usr`, private archived source, isolated test fixtures,
loopback TCP and each test's unchanged workload, quota, durability and
internal deadlines. Nuc is an i5-1135G7 (four cores/eight threads);
intel1/intel2 are Core Ultra 7 256V (eight cores/eight threads), all x86_64.
Scratch is under each host's HOME; preflights retain filesystem/kernel details.
Equal logical CPU counts are not equal physical execution or storage capacity.
Host-limit NOTRUNs are not passing coverage. No real
cluster, Task H mount/store or other user's infrastructure was stopped.

| Host / source / attempt | Full-gate result | Calibration disposition |
| --- | --- | --- |
| nuc / `babeea640` | 523 commands, 7 FAIL, 12 NOTRUN, 1461 s, 45 OVERBUDGET, build 50 s | Rejected; stopped before run 2 |
| intel1 / `babeea640` | 523 commands, 2 FAIL, 12 NOTRUN, 727 s, 4 OVERBUDGET, build 30 s | Rejected; stopped before run 2 |
| nuc / untouched `fe18612ae` | 528 commands, 4 FAIL, 13 NOTRUN, 1405 s, 43 OVERBUDGET, build 60 s | Failed baseline; not a calibration input |
| intel2 / `9638c34e0` | Interrupted during build after Task H started independent daemons | Contaminated; no test timings |
| intel1 / `9638c34e0`, run 1 | 529 commands, 0 FAIL, 13 NOTRUN, 734 s, 4 OVERBUDGET, build 31 s | Clean gate with recorded host limits; K43 54.9 s |
| intel1 / `9638c34e0`, run 2 | 529 commands, 0 FAIL, 13 NOTRUN, 751 s, 5 OVERBUDGET, build 1 s | Clean gate with recorded host limits; K43 54.9 s |
| intel1 / `9638c34e0`, run 3 | 529 commands, 0 FAIL, 13 NOTRUN, 738 s, 2 OVERBUDGET, build 0 s; K43 55.0 s | Rejected: one unclassified process observation |
| intel1 / `9638c34e0`, replacement | 529 commands, 0 FAIL, 13 NOTRUN, 663 s, 3 OVERBUDGET, build 0 s | Accepted third clean observation; K43 55.0 s |

The original attempts used a synthetic Git index to satisfy the graph test.
That was this session's setup error: it made `test_qualify_driver.py` reject
its output as inside Git. The original graph test was also unwired; devel
fixed that in `8106dab79`. Those two failures account for intel1's original
failed attempt. Its K43 exited zero in 55.3 s, but that is an attempted
observation, not one of the three required clean measurements.

The other original nuc failures were:

- D4 progress: a 1 MiB write+fsync took 11.04 s against 10 s; the reported
  payload checks had no mismatch.
- D4 plateau: 11 packer PUT errors, no read errors in the final sample.
- Forwarded-PREPARE progress: one write+fsync took 3.20 s against 3 s.
- W130: operator resync returned `-13`.
- W166: leadership moved during its L0-cap setup; the placement assertion
  failed before the intended write test.

The corrected source inherits devel's bounded pre-admission resync helper
and failed-plateau retention; no bound or workload was relaxed by Task T.
The untouched nuc baseline still failed:

- **D4 plateau:** one load rewrite returned EIO; four foreground operations
  exceeded 10 s (write+fsync up to 13.512 s, `.efs/stats` 11.64 s); 56
  abandoned uploads after 480 rewrites exceeded the bound of 24.
- **K43 setup:** `raft-step-down` returned BUSY before the coordinator kill.
  This is not a reproduced coordinator-kill corruption.
- **Expiry:** `restricted` disappeared before `stat` in `expiry_lifecycle.py`.
- **W130:** the snapshot-import/halt-clear condition missed its recovery
  deadline, despite the new pre-admission helper.

These are distinct failures, not proof of a common root cause. Task U already
retains an independent untouched-base D4 packer failure with `NOT_PRIMARY`
PUT retries ([report](k32-write-cost-profile-20261028.md)); that open issue
is not erased by later individual passes.

## Quiet-host diagnostic and scheduling finding

After the nuc baseline stopped, run each D4 test **once, serially**, on its
unchanged `fe18612ae` binaries, with `EFS_D4_KEEP=1` and a fresh private TMPDIR:

```sh
python3 tests/test_d4_progress.py
python3 tests/test_d4_plateau.py
```

Both pass: **73.376 s** and **168.553 s**, original bounds/workloads unchanged.
This is diagnostic contrast, not a calibration or a replacement for the
failed pool. It supports investigating cross-test contention; it does not
prove the EIO harmless or establish its low-level cause. Detailed fixtures
and control logs remain for that open investigation.

The runner sorts pooled commands by slots, then decreasing budget. Current
first wave is report-stepdown (6 slots, 440 s) plus snapshot-import (2 slots,
310 s). D4 progress (2 slots, 260 s) cannot occupy the first two-slot window
claimed by its reason text. This is a deterministic scheduling calculation,
not observed process start timestamps. Updating time estimates can therefore
change overlap; the safe-neighbor assumption is not a stable invariant.
Task H independently reproduced a 3.85 s REPORT operation under the unchanged
3 s bound while snapshot import shared its disk. Its committed runner-only
fixes `ea2b9a342` and `f1ed2ccd8` are adopted as `7a292ac53` and `c7a70201f`:
packing-epoch setup, W166 setup and REPORT step-down now run alone. Workloads,
assertions, deadlines and declared slots are unchanged; there is one current
policy, no A/B selector. The runner regression and fitter regression PASS.
Budget header comments now distinguish historical reason text from actual
isolation. Old intel1 observations retain their old policy/source identity;
new nuc/intel2 measurements and confirmations must use the corrected policy.
The fitter's schedule model reads that same EXCLUSIVE classification.
`test_make_test_recipe.py` passes: 91 rows, static recipe model 1519 s against
the existing 1650 s margin guard. This is not a refitted total or measured wall
time; the user target remains 1100 s. The final fit must report its own actual-
command reference model and the rows that exceed that target.

## Raw evidence and controls

Host roots below are under `/home/maik/efs-artifacts/taskT-20261028/`.
All listed text manifests were closed and every hash rechecked. Sources were
rechecked with no tracked changes (2,282 files in the original attempts;
2,293 in untouched devel). Archive sources, exact external controller source,
preflight/process state, commands/timings, failures, budgets, build/binary
identity and final results are retained. No payload/store was exported.

| Host / evidence root | Files | SHA-256 of `SHA256SUMS` |
| --- | ---: | --- |
| nuc / `nuc-babeea640` | 30 | `5099b1004ab99fbe584e39a17b8b602d9aeb784b23d0b84260d79c23ddbfa29f` |
| intel1 / `intel1-babeea640` | 25 | `ae857a0b8a3e6ca7517115281fca17b6bdfd019206ea7662523c4f4bbcfb76c4` |
| intel2 / `intel2-9638c34e0` | 25 | `5374fbca8d57d7293895f94d7e99fa43f9200616ac44251a0b4fcb9ec154e323` |
| nuc / `nuc-base-fe18612ae` | 27 | `83b2b2af66d0a75bbbd66711a701f97c270003340b676fd7d72673b00231e752` |
| nuc / `nuc-base-fe18612ae-serial-d4` | 11 | `8219f072db9268190bb2b77afa2c2ae2d179cd19f0ed39d4ff50782eb8348489` |
| intel1 / `intel1-9638c34e0` | 54 | `81d3e8e2477a0302ddf6113704f1e0f20bf928d9859ac09288f20d8c1578bbcf` |
| intel1 / `intel1-9638c34e0-replacement1` | 22 | `6cd80c0fd6d004d425332702a8213a89f7874553fdc62871bc366bd72a47ff33` |
| intel1 / `intel1-cleanup-9638c34e0` | 3 | `c5c5575f51ba4348e08b02a4d4d4a5fc704a000769625c7a3563f99ecbdd196c` |
| intel2 / `intel2-owned-closure` | 3 | `93b71174ac5cf4deb7108cbf4c21d1b1e225b13da5ae7b32afb627230db26cde` |

Corrected intel1 evidence: `intel1-9638c34e0/`, detached screen
`taskT-intel1-9638`, source `~/efs-taskT-20261028-9638c34e0/src`.
The initial three-run controller stopped after one guard sample in run 3:
`cc1`, PID 1705236, with no readable executable/cwd paths. It recorded no
parent/start identity, so this is unclassified, not proven foreign work and
not a clean sample. Its source check found all 2,295 files unchanged. The
replacement observer records parent/start identity and arguments, checks
process identity again after inspection, audits exited/reused samples and
still rejects live unidentified or foreign work. Replacement output is
`intel1-9638c34e0-replacement1/`; it finished with no contamination or observer
race records, no tracked source changes and controller exit 0. Accepted inputs
are original run 1, original run 2 and replacement run 1; the rejected run 3
stays unchanged. The current fitter validated their identical 529-command
sequences and priorities, zero failing commands, and all 91 budgeted commands
present exactly once. It refused the actual marked run 3.

61 copied small run-input files were checked against their host manifests at
`/Users/mike/Documents/Codex/artifacts/taskT-20261028/inputs/intel1-corrected/`.
`validation.json` records accepted paths, summaries and the 54.9 s K43 median;
SHA-256 `596cba31405ef2e96b78c0bcd49a0d826ee1be8d38e5c65fd13473e51555340e`.
No global budget is inferred from this one-host median. After zero-owned-process
and zero-owned-mount checks, both stopped intel1 source/build trees were pruned
with edelete; `intel1-cleanup-9638c34e0/PRUNED.txt` and its receipt name exactly
what was removed. Raw evidence directories and failed/indeterminate results
were retained. Tested source paths above are historical, reproducible from Git.
Untouched nuc source is `~/efs-taskT-base-fe18612ae/src`; the failed D4,
K43, expiry and W130 fixtures there, plus serial D4 controls under
`~/efs-taskT-base-fe18612ae/serial-d4-tmp`, are kept for open investigation.
The original nuc D4 tests self-cleaned their stores; their stdout is retained
but cannot reconstruct those deleted fixtures. Retention in the new baseline
was explicit, not a claim that old evidence was recovered.

Controllers stop before a next calibration run on failure/contamination.
The five-second guard observes competing EFS/build jobs under the test
account (`maik`); it does not prove no other-user or external I/O activity.
Preflights record all processes. Existing intel1 infrastructure owned by
`efs` stays untouched. Intel2's interruption records exactly Task T's
terminated children; Task H's processes/stores were not touched. Its
missing-summary controller diagnostic is from interruption, not a test
assertion. A later root-ownership scan found one surviving Task T daemon
(PID 1472455), spawned during the earlier descendant-snapshot termination
race. It ran **19:44:52–20:30:47 UTC**, overlapping other intel2 work. Only
that owned daemon was signalled: TERM, then KILL after 20 s; a repeat scan
found no owned process remaining. Logs and `orphan-cleanup.json` /
`overlap-window.json` are retained. This session's cleanup mistake means
other gates overlapping it are not quiet-host timing evidence; it does not
prove that the daemon caused any failure. Future interruption closure needs
repeated ownership scans and mount checks, not one descendant snapshot.
A later closure check verified zero Task T-owned processes and mounts and
copied the final stopped-daemon log. With explicit user authorization, both
Task H and Task AA were notified of the overlap and its qualification limits. Xorinox was idle when checked; it cannot replace the required
nuc/intel2 coverage. SSH checks did not use BatchMode.

## Completion still required

1. **DONE:** three clean intel1 pools and K43 timings validated; retain the
   explicit rejected attempts and host-limit skips.
2. Review later devel repairs and freeze the next source; the `fe18612ae`
   failure is not a result on newer `424d0db8f`. After the host is released,
   collect three clean supported-profile nuc gates, investigating any failure
   against an untouched same-host baseline. Do not fit failed
   timings or retry them into a claimed PASS.
3. Collect three clean intel2 gates after Task H releases it, with a new
   quiet preflight and evidence directory.
4. Fit complete cohorts, review every limiting row and modeled total against
   1100 s, and report any excess without changing D44. Example:

```sh
python3 scripts/refit_test_budgets.py \
  --budgets tests/budgets.tsv --runner scripts/make_test_summary.sh \
  --host-runs intel1 I1_RUN1 I1_RUN2 I1_RUN3 \
  --host-runs nuc NUC_RUN1 NUC_RUN2 NUC_RUN3 \
  --host-runs intel2 I2_RUN1 I2_RUN2 I2_RUN3 \
  --output NEW_DIRECTORY
```

5. Commit the reviewed table and run one confirming gate on nuc and intel2;
   record actual OVERBUDGET counts. Zero is expected, not guaranteed by a
   median fit. Preserve failures and inspect any excess without relaxing
   durability, test bounds, slots or the target to manufacture success.
6. Finalize manifests and prune closed bulk only after checking no owned
   processes/mounts remain, using `~/bin/edelete --force --delete`. Keep open
   investigation stores. Raw logs/captures stay outside Git.
