agent/devel-task-j (analysis 9567e6b9a, base 394f2b791): node9901 TOTAL 508 FAIL 5 NOTRUN 5 (769s); fstor007 TOTAL 508 FAIL 1 NOTRUN 5 (750s), both unchanged-base gates. Evidence: docs/status/gate-budgets-refit-20261026.md. Intel calibration NOTRUN pending F release; no product defect established; no refit yet.

# W160 three-run budget refit — October 26, 2026

**REAL-HOST GATES COMPLETE WITH FAILURES; CALIBRATION NOT COMPLETE.** No measured budget is changed and no gate PASS is
claimed. The user assigned Task J from `parallel-tasks-20261026.md`; its updated
instructions retain intel1/intel2 after Task F/E releases the host and add
one real-host gate each on node9901 and fstor007. The user explicitly
overrode that order: run both ORCD gates first, then three Intel calibration
runs after Task F releases intel1.

## Resource and source scope

Planned calibration host: **intel1, after Task F's explicit handoff**. Task F
is active, fixing its serve-path failure there. A lightly loaded
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
walls and closed gate manifests are recorded below. Historical evidence stands
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

## Remaining calibration plan (Intel not executed)

1. After intel1's handoff, export one pinned current devel commit into
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

- Intel host handoff; three measured calibration runs and safe failure handling.
- Actual Intel budget refit (synthetic helper validation passed).
- Recipe check and post-refit gate on the released host.
- Coordinator fixes for the two baseline test defects; ORCD gate FAILs remain recorded.
- Per-command table, critical chain, exact build identities, closed manifests
  and Intel cleanup receipts. ORCD manifests and cleanup are complete above.
