# Full code-gate runtime review — October 23, 2026

**Verdict: REVIEW COMPLETE for execution structure; full-suite speedup NOTRUN.**
Scope: K25/W139 test execution and alpha qualification. Source reviewed:
`b3975d270ddc3099b9a4556d1e18a35dd97b8d65`, including build fix
`a3166c315` and the fast/long split. No production or test assertions changed.

## Conclusion

Yes: reduce repeated harness compilation and run isolated tests concurrently.
Moving slow tests to `test-long` is useful for edit feedback, but does not
reduce the work of the release gate `test-all`. Keep that full gate mandatory.
The reported 449 → 233 seconds is the earlier commit's measurement, not a
measurement repeated by this review. Do not forecast a new total without the
same host, source, test cases and retained per-command timing evidence.

## What was inspected

The Makefile execution graph, every referenced Python script by source-pattern
inventory, C fixture/resource patterns, the summary runner and its regression,
and focused manual inspection of compiler, deadline, shutdown, RDMA, query
negative-control, Raft and scratch-lifetime tests. This is not a line-by-line
correctness certification of every assertion in the suite.

Recursively following recipe `$(MAKE)` calls finds 307 distinct Python test
scripts in `test` and 27 in `test-long`. Of those, 273 and 18 respectively
contain a quoted cc/gcc/clang compiler selector: **291 scripts**, not 291
compiler invocations (some compile multiple positive/negative variants).
288 fast and 26 long scripts reference tempfile. Temporary paths are good
isolation evidence but do not prove that a test has no other shared resource.
55 fast and 14 long scripts contain sleep/usleep/nanosleep calls, sometimes
in embedded C. These are inspection counts, not measured bottleneck rankings.

## Findings and priorities

### 1. Fix the missing dependency before any parallel execution

`test-long: all` starts `test_shutdown_blocked_io.py` and later
`test_shutdown_stuck_worker.py`. Both need `tests/shutdown_faults/efsd`, which
is absent from TEST_BINS/all and is instead built by a recursive make inside
`test-create-errors`, reached through the fast suite. Thus a clean standalone
`make test-long` can fail; `make -j test-all` can start it before the fast
suite builds its daemon. This is a source-proven dependency gap, not a
runtime reproduction in this review.

Put the fault daemon and every other special test binary in real build
prerequisites. Finish the build phase before starting tests. Move the four
put/data helper builds and compaction-fault build out of execution recipes
too. Do not parallelize a flattened list containing compiles, links and their
consumers without those dependency edges.

### 2. Expose test jobs, rather than hoping make -j splits a recipe

Nearly all fast tests are individual commands in one recipe. Make executes
that recipe serially regardless of -j. `test-all: test test-long` exposes only
two large lanes (plus the RDMA prerequisite), not hundreds of test jobs.

Use one explicit case inventory shared by make and the summary runner:
case id, argv/environment, required binaries and resource class. Have make
build the prerequisites once; execute cases through a bounded worker pool.
Start with four workers on the reported eight-core host and measure two,
four and eight, including failures and tail latency. This is an executor
resource budget, not an EFS product behavior flag.

Each case needs its own TMPDIR and output file, a process-group deadline,
child cleanup, and deterministic result accounting. Continue independent
cases after a failure; retain FAIL and NOTRUN/INCOMPLETE semantics. Record
queue time separately from execution time, and include build time in the
end-to-end total. Use prior durations to start long independent cases early.
Do not share or overwrite summary/progress/build files between runs.

### 3. Make the generated C harnesses incrementally buildable

Most Python tests generate C in a fresh TemporaryDirectory, compile it, run
it, and remove it. That compilation repeats even on a warm unchanged tree;
`a3166c315` fixes library rebuild expansion, not this class of work.

Generate stable harness sources as build outputs, rewriting only when their
contents change, with explicit dependencies on the extracted production
sources, headers, compiler and flags. Build their binaries through the normal
parallel build graph; give runtime scratch a separate unique directory.
Positive, mutant, fault and sanitizer variants remain distinct artifacts.
A filename-only cache would be unsound. Retain checks that source extraction
matches the intended function and that each negative control fails for the
specific expected assertion. `test_raft_ack_window.py`,
`test_raft_pending_reads.py` and `test_query_sort_peak_negative.py` illustrate
why the variants cannot simply be deleted as duplicate coverage.

Do this incrementally, starting with the largest measured compilation costs.
Avoid combining every harness into one executable: duplicate stubs, globals,
compile flags and failure isolation are intentional in many of these tests.

### 4. Parallelize by resources, not just fast versus slow

| Class | Initial policy | Reason |
| --- | --- | --- |
| Source-only Python checks; isolated generated-C units | Bounded parallel pool after dependency audit | Usually independent; compiler CPU dominates many tiny cases |
| KV/store/query disk and large negative controls | Small I/O pool, measure RSS and scratch peak | Concurrent compaction/fsync and duplicated datasets can dominate or exhaust resources |
| Real deadline/latency and sanitizer tests | Exclusive or separately budgeted lane initially | Load can create timing failures or memory pressure |
| RDMA cases | Serialize device tests initially | Shared hardware resources; missing hardware remains INCOMPLETE |
| Process discovery/shutdown tests | Exclusive initially | Observe real processes and exercise lifecycle control; unique roots alone are insufficient |
| Build/recipe introspection | Build-phase barrier/exclusive | make dry-runs recurse and parse-time stamp updates touch build state |

For example, the two query-delta modes use unique mkdtemp stores, so they are
candidates for concurrent execution, subject to I/O/memory measurements.
RDMA segmented tests use socketpairs, which avoids TCP port collisions but
not shared-device contention. Shutdown tests obtain an ephemeral TCP port
then close the probe before daemon bind; that leaves a port-allocation race.
Keep them isolated until endpoint allocation/startup is robust. Never retry
a test merely to turn a concurrency-induced failure into PASS.

### 5. Replace scheduling sleeps selectively, preserve deadline coverage

Use barriers/condition variables or a test clock when a delay merely tries
to arrange an interleaving. Preserve bounded failure diagnostics if the
expected event never arrives. `test_opid_admission.py` waits 1.25 seconds
before releasing an identity; first determine which wait/retry behavior that
assertion is meant to cross before replacing it with an explicit handshake.
Do not blindly shorten that sleep or the shutdown budgets.

Keep real-clock tests for timeout, cancellation, process-group cleanup and
hard shutdown. Fake-clock unit tests complement those tests, not replace them.
Likewise do not shrink KV workloads below the thresholds that trigger
compaction, index rollover or disk-accounting failures. Measure compile and
runtime separately before optimizing a test's workload.

### 6. Harden the runner while replacing dry-run replay

`scripts/make_test_summary.sh` obtains commands from `make -n ... | grep` and
executes the text serially. This loses the dependency graph and cannot safely
be turned into an arbitrary parallel map. It also suppresses make stderr and
does not check that command enumeration succeeded: a partial nonempty list
can pass the subsequent check. Check the producer's status, not just whether
the output file is nonempty.

`build.failed` is never cleared at the start, so a previous failed build can
poison a successful rerun. Use a fresh run directory and explicit build result.
The hardcoded -j64 build budget and private libfuse environment are host
assumptions, not sensible universal defaults; preserve configured tool paths
and budget CPU/memory across build workers. The shell's current reported
seconds cover execution of the command list, not the preceding build. Report
build, tests and combined wall time separately.

The embedded runner's regression checks failure, timeout, continuation,
NOTRUN, empty input and complete failure logs. Extend it for parallel mixed
outcomes, exactly-once case execution, child cleanup, interrupted runs and
failed inventory generation before switching the gate executor.

## Acceptance and reproduction

On the same Linux build host, freeze one source/build identity and save both
cold-build and warm-build results. Run the complete existing case set serially,
then with bounded parallelism; compare case ids, variants and verdicts exactly.
Retain commands, per-case logs/timings, compiler flags, CPU/RAM/scratch/device
inventory, peak memory and scratch use. Repeat timing-sensitive cases under
the intended concurrent load without changing their assertions or deadlines.
A faster subset, more SKIPs, or a retry-only success is not a full-gate speedup.

Review environment: Darwin arm64, no cluster, mounts, RDMA or Linux production
binaries used. Executed `python3 tests/test_make_test_summary_runner.py` and
`python3 tests/test_tmpdir_hygiene.py`: both PASS. Full Linux suite and proposed
scheduler/cache: NOTRUN. Source inspection is reproducible with the named
commit, Makefile and scripts; no private raw timing capture is needed for
these findings. No test implementation or scheduling change is claimed here.

## Measurement: harness compilation (October 23, follow-up)

**Finding 3 deferred: caching the generated C harnesses would save at most
about 30 s of serial compile time.** Every compile can be reused, but there
are too few to matter.
- **Method:** a logging wrapper in front of `cc`/`gcc` on intel2 (8 cores,
  devel `6a8a95bab` tree plus the runner). It recorded each compile's owning
  Python test, its duration, and a fingerprint of the C inputs and flags,
  with temporary paths normalized.
- **Runs:** two full `test-all` runs (`EFS_TEST_JOBS=4`), both 421 commands,
  0 FAIL, 14 NOTRUN.

| Measure | Value |
| --- | --- |
| Python tests that compile C | 291 |
| Compiler invocations per run | 361 (259 tests compile once, the rest 2–5 times, one test 25) |
| Compile time per run | 31.5 s |
| Compiles identical in both runs (reusable) | all 361 (31.5 s) |
| Python test time per run (sum of per-command times) | about 172 s |

**Decision (user):** a harness cache is not worth it for about 30 s. Work
moves to the exclusive tail, the 29 commands run alone after the pool
(about 87 s), which bounds `test-all` at 4 workers.

**Reproduce:** put a `cc`/`gcc` wrapper earlier in `PATH` that runs
`/usr/bin/cc` and logs the owning `tests/test_*.py` (found by walking
`/proc` parents), the seconds, and a sha256 of the arguments and `.c`/`.h`
contents with `TMPDIR` paths normalized. Then run
`scripts/make_test_summary.sh` twice and compare fingerprints per test.
The wrapper used is on intel2 at
`/data1/efs/evidence/build/ccwrap/ccwrap.py` (outside Git); its logs are
`/data1/efs/evidence/build/cc-run{1,2}.log`.

## Exclusive tail (October 23, follow-up)

Tests that run alone after the pool are the floor of `test-all` at
`EFS_TEST_JOBS=4`. Changes (no deadline or assertion changed):

- **`test_shutdown_blocked_io.py`, 34 s → 11.4 s:** its three cases (write,
  fsync, stderr) run concurrently. They have their own stores, daemons and
  pipe, and ports are allocated up front from probes held open together.
- **`test_shutdown_stuck_worker.py`, 13 s → 6.8 s:** its three groups run
  concurrently, with the packer case followed by its restart.
  `free_port()` never returns the same port twice in a process.
- **`test_raft_ack_window.py`:** leaves the exclusive pattern. It builds
  sanitizer variants and asserts no wall-clock timing.

Each shutdown test passed three times in a row. Full runs on intel2:

| Run | Result | Before |
| --- | --- | --- |
| 4 workers | 421 commands, 0 FAIL, 14 NOTRUN, 96 s | 132–152 s |
| 4 workers | same, 95 s | |
| serial | same, 185 s | 213 s |

The exclusive group is now about 59 s. The largest remaining items are
`test_opid_admission.py` (11.8 s of deliberate admission deadlines; see
finding 5 before changing them), the shutdown tests, process discovery
(`test_server_processes.py`, `test_client_processes.py`) and the two bench
CLI tests.

