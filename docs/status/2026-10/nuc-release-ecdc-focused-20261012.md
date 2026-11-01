# NUC focused disabled-command qualification — 2026-10-12

Actual direct human resume verified in AMD manager chat: resource-weighted
split, same frozen commit, `.efs` disabled. NUC owns lightweight configuration
and focused deep concurrency; AMD owns broad seven-client lifecycle,
peer/cold/physical/shutdown. Earlier stop is superseded for this assignment.

Exact source `ecdc8c6034b610a2bf69e7a79664d8a5e990de84`, central
`qualification-v030-tcp-direct`. Candidate changes only the mounted helper's
explicit `--disable-efs-commands` option; daemon implementation unchanged from
03bd. Archive SHA256
`2e5a231b1e54958f37c370a382be3c617ed307701051f4c9a89df5c649979634`.
All23,318 regular tracked-file hashes/commit verified before build and before/
after quiet checks. Normal build `ecdc8c60-group3-normal`; actual image hashes
and version retained. No source modifications, pushes, tags or promotions.

## Focused scope and actual configuration

External focused runner, not a replacement of the existing case assertions:
four private direct/TCP/FILES daemons, **4G per node**, seven FUSE clients.
Actual `/proc/PID/environ`, executable image hashes and command lines captured
for all4+7 processes. Each verifies `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, flow/earlyretire/groupdrain0,
repair0. `.efs` absent in actual root listing and existence lookup PASS.
No build/profiler/other EFS daemons during measurements.

Seven concurrent invocations of unmodified `tests/posix/posix_suite.py`:
`--suite core --filter dir_deep_nesting --jobs 16 --timeout-s 30`, unique tags
and result paths. Substring selects exactly the original60-depth case and
100-depth beyond64 case. Thus14focused cases, not seven broad221-case suites
and not112 background cases. Worker assertions and30-second deadline unchanged;
600-second per-suite outer cap retained. No test filtering acceptance is
transferred to AMD broad workload.

## Results

**PASS**: all seven suites2PASS/0FAIL/0SKIP/0NOTRUN,14cases total.
Per-suite time1.5–1.7s; complete concurrent phase2.226706s. All seven return0.
The 60-depth workload still uses original full-path makedirs/write/read/rmtree;
no dir_fd rewrite or deadline extension.

Light correctness PASS: actual disabled-command visibility; existing one-case
`basic_write_read`; explicit small fsynced payload read from another mount,
rename bytes visible there, unlink absence visible there. These are reversible
small checks, not the broad I/O/recovery matrix assigned to AMD/Intel.

Normal client drain and daemon TERM cleanup: no errors, all seven mounts
unmounted, no remaining efsd. Private roots remain identified under
`/data1/efs/evidence/nuc-ecdc-focus`; original service remains stopped. No old-binary/data
retention or backwards qualification; unrelated host services/paths untouched.

## Interpretation and evidence

Focused deep concurrency is fast with `.efs` disabled and without broad
background cases. **Two factors changed from prior broad03bd run**: query
flags and background workload. This result does not isolate `.efs` causality,
does not prove the broad seven-client deadlines are fixed, and does not close
W97 sustained slowdown or original fullW52/bothmodeW112. The manager's AMD
full workload is the required next confirmation. Prior failed broad evidence
07fc366c remains intact. RDMA and repair deferred.

Raw: `results/measure/20261012-nuc-ecdc-focused/raw/`: full source manifest,
build log/executable hashes, external focused runner, exact test commands,
actual4+7 live configuration proof, all complete TSVs/logs, result and cleanup.
Collected archive retained. Remote `/home/efs/nuc-ecdc-focus`, ports24410–24413.
Manager informed of fast focused passes and the causal limitation. No additional
NUC broad test or live profiling launched.

**Raw evidence note (2026-10-23):** the bulk data in `nuc-ecdc-focus` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
