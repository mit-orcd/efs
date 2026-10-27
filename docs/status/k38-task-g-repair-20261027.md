# K38 / W161–W162 — Task G publication-pressure repair, October 27

Branch `agent/devel-task-g`; no merge or push. User authorized fixing the
K38 full-suite failure and repeating `make test-all` on xorinox, retaining
failed evidence. **PASS for the scoped xorinox checks; full runner remains
INCOMPLETE for 13 prerequisite-limited commands.** Product repairs through
`66c78fbdb57fdb31fc2666a5c6f9108e8100e2f2`; final binaries `1354d568d`,
committed test-only identity correction `c33139385`, based on Task G `c5eda4478`
(product W166 repair on devel `394f2b791`). This is a scoped continuation of
[W166](w166-close-eio-flush-deadline-20261026.md), not closure of its separate
SIGSTOP, RDMA or multi-host gates.

## Findings and changes

The October 26/27 gates on unchanged devel `394f2b791` and Task G
`4bcd52ca0` failed the two-client shared-chunk test. Their assertion output
and printed tails survived, but the original test deleted full failed
fixtures. They remain failures; complete causality cannot be reconstructed
from those tails alone. `97eb0e1d1` now keeps a failed K38 fixture by default,
records every writer's successful pwrite count and fsync count, and preserves
both the original syscall error and a subsequent close error separately.

The new original-product full-pool capture at `c5eda4478` reproduces an EIO
and an EAGAIN. The complete logs show:

- Server publication `18e27772d81ed351:4` already has three definite CAS
  losers, then encounters another unit's BUSY. The old terminal classifier
  overwrites that definite residual with BUSY/UNKNOWN. Immutable same-identity
  retries cannot repair its obsolete CAS base. Receipt lookup expires unknown.
- The same client later obtains a definite PARTIAL/STALE receipt but returns
  it to the foreground drain as an error, instead of retaining ownership and
  rebuilding the residual. Its `CLOSE-PROF` line records `rc=-14`, 28,754 ms
  waiting for the append stripe and 34,293 ms overall. The old resolver could
  start a fresh allowance after the inherited outer deadline; it is now clipped.
- The pressure path uses full fsync completion, including held-unit waits and
  new whole-inode flushes. That makes capacity reclaim wait for future writes,
  although only a bounded completed cohort is needed. This also prolongs the
  append-stripe hold. Foreground capacity reclaim and background reclaim now
  use the same finite cohort path, cycling the inode within the existing bound.

Reviewed, directly relevant changes were ported from `agent/task-h-repair`;
its live results are not substituted for this branch's gates:

| Repair | Original commit | Task G commit |
| --- | --- | --- |
| Definite receipt STALE re-enters publication; shared original completion budget; receipt lookup clipped by inherited deadline | `0c1970678` | `5334771bd` |
| Known CAS loser plus later BUSY stays STALE; no ACK or promotion of otherwise unknown work | `84d6c351c` | `743028129` |
| Pressure retains held units without recursively flushing future writes; explicit fsync still waits | `4f8f25e7c` | `9f03d4c91` |
| Foreground write capacity uses bounded cohorts too | `5c81d6070` | `08d62d686` |
| An unpublished full PUT placeholder is not evidence of a changed committed CAS base | `e7b321502` | `66c78fbdb` |

The last guard is a separately demonstrated K38 defect: two STALE unit
verdicts do not prove that a bystander's base moved when the export table
contains its own unpublished PUT. The captured node9901 identity in the
original repair's regression is synthetic test input here; no new xorinox
failure is attributed to it without a matching trace. Fresh authoritative
pull remains required to acknowledge any chunk. The real changed-base
limiter, failed-rebase bounds and existing REPORT attempt bounds stay.

The 30 s outer operation deadline, 8 s REPORT send allowance, shared existing
receipt allowance, 24 writers, 200 records/writer, byte oracle and pressure
assertion are unchanged. No new product toggle, ignored application error,
degraded durability claim or old-format support was added. Slot/held-unit
failure diagnostics are ported separately (`22cad3bc3`); deadline-scope tests
are `cc511e41e` and `73ceaebdf`.

## Evidence and limits

Xorinox, user `maik`, 16 logical CPUs, Linux
`6.12.0-211.67.1.el10_2.x86_64`, XFS HOME, GCC and libfuse 3.16.2. Three
private loopback fault-build daemons, TCP, O_DIRECT, 1 GiB quota each,
two FUSE writer mounts and a fresh reader mount. The 24 writers split over
the two mounts, each writing 200 deterministic 47,008-byte records to one
shared file (225,638,400 bytes); client body budget 32 MiB and reclaim target
1 MiB. No production stores, protected ports, other agents' clusters or
`/data1/efs` paths were operated on.

| Check | Result |
| --- | --- |
| Original product `c5eda4478`, six traced default K38 runs | 6 PASS, each 4,800 records exact on both mounts and after remount; complete logs/fixtures kept. Tracing can change scheduling. |
| Same product, driver `97eb0e1d1`, four untraced default runs | 4 PASS, same complete byte checks; these overlapped the original-product full suite and are not quiet-host timing measurements. |
| Same product, traced `--fsyncs 20` diagnostic variant | FAIL on the required pressure assertion: all 24 writers completed 200 writes and 21 fsyncs, byte checks passed on both original mounts, but no cache pressure occurred. Remount check NOTRUN after the assertion. Not an EIO reproduction or a passing K38 gate. |
| Original product full suite | `TOTAL 510 FAIL 1 NOTRUN 13 STATUS FAIL (977s) OVERBUDGET 2 BUILD 11s`; sole failure K38 EIO/EAGAIN with full fixture retained. Standalone work and the candidate build/repetitions overlapped portions of this diagnostic run, so no timing comparison is claimed. |
| Candidate `66c78fbdb`, five unprofiled default K38 runs | 5 PASS; 24,000 written records checked fully through both original mounts and after remount, zero syscall errors. Walls 64.8 / 55.8 / 39.9 / 63.4 / 44.6 s under co-running diagnostic work. |
| Four production-source negative controls, on Mac and xorinox | All four fail at their intended assertions on the original code: resolved receipt retry, STALE/BUSY classifier, finite pressure completion and placeholder CAS evidence. Fixed versions pass. No compiler/setup failure is counted as a negative control. |
| Candidate native `make test`, default `/tmp` | FAIL: direct-I/O positive-control daemon refused startup. Root XFS was 84% used, below the required 20% free reserve. Separate startup probe confirms `Storage quota: cannot establish safe headroom; refusing startup`. The unmodified test cleaned its temporary fixture; its failed output and a separate retained startup probe survive, not that original temporary store. |
| Candidate native `make test`, private HOME TMPDIR | PASS, rc 0. Same binary/code; required reserve preserved. |
| Candidate `66c78fbdb`, unprofiled eight-job `make test-all` | `TOTAL 512 FAIL 1 NOTRUN 13 STATUS FAIL (878s) OVERBUDGET 1 BUILD 1s`. K38 PASS, 34.3 s (4,800 exact records on both mounts and remount); sole failure W166 pre-fault leadership-transfer BUSY, before PUT was tested. |
| Setup-only continuation `1354d568d` + committed harness `c33139385`, same product; full eight-job rerun | `TOTAL 512 FAIL 0 NOTRUN 13 STATUS INCOMPLETE (842s) OVERBUDGET 2 BUILD 24s`, rc 3. K38 PASS 34.5 s; W166 PASS 28.5 s, both 2 KiB payloads exact through a fresh reader (REPORT close 15.22 ms; PUT close 32.77 ms). All earlier failures retained. |

Across five standalone and two full-pool fixed K38 runs, all **33,600**
written records were checked byte-for-byte through both mounts and after
remount, with zero writer syscall errors. Positive readback is exact, not sampled. Remounts are process/client-cache
checks, not OS reboot or power-loss verification. Missing RDMA, sanitizer,
Valgrind and external cluster prerequisites must remain explicit NOTRUN;
zero failures alone is not an unconditional all-host PASS. Specifically,
the 13 NOTRUN commands are nine RDMA paths, the absent external cluster-stop
driver, a unit fixture requiring a separate `EFS_FAULTS=1` build, Valgrind and
the unavailable sanitizer runtime. The fault-built loopback daemons did run;
that does not qualify the separately compiled follower unit fixture.

Two timing budgets were exceeded in the final run: geometry grow-a 121.3 s
of 120 s, D4 plateau 197.2 s of 180 s. Their assertions passed; the runner
reports these as OVERBUDGET, separately from FAIL. Neither budget nor the
D44 1,100 s target / 1,650 s aggregate budget bound was raised. The final
842 s test wall plus 24 s build is below the overall target.

## Additional setup failure and correction

The first repaired full suite's W166 REPORT case passed (13.49 ms), then
its PUT setup requested a leader transfer once and asserted success even
though the target was still catching up. This was a test setup failure;
the PUT fault and final fresh-reader checks were NOTRUN. Its `result.json`
contains only the REPORT result and must not be read as both cases passing.

`1354d568d` reuses the existing bounded `place_leaders` helper for this
second transfer, with the original 20 s window. Only the documented
pre-fault `rc=-13` catch-up response is retried. Both groups must be led by
the target before the STATUS fault is armed; permanent BUSY or any hard
management error still fails. The helper tests now exercise both transfer
directions and the 20/30 s bounds. No product behavior changed after
`66c78fbdb`. The final run rebuilt all binaries at `1354d568d`. Before the
affected test files were executing, the committed test-only `c33139385`
correction was installed: compare the transfer result against the actual
assigned member ID rather than assuming process start order. Its unit test
covers reordered assignments. Before/after file hashes, install time,
source commit and unchanged binary identity are in
`gate-fixed2/harness-c33139385/manifest.json`. No uncommitted test code was
used; the product source is identical in these revisions. Failed case 474 (229 files, 23,801,782 bytes) and the passing
pooled K38 case 446 (6,434 files, 477,877,793 bytes) are copied and
source/destination hash-verified in `gate-fixed/`, alongside the original
binaries. This additional failure remains in the evidence.

## Reproduce

Build a fresh exact archive on a Linux FUSE host with sufficient storage
headroom. Archive `c33139385`; the final qualification used its unchanged
product at build identity `1354d568d`, with the test-only overlay recorded
above. Set the identity explicitly outside Git:

```sh
make -j8 EFS_GIT_ID=1354d568d EFS_GIT_BRANCH=agent/devel-task-g \
  EFS_GIT_DESCRIBE=1354d568d all tests/faults/efsd test-bins
mkdir -p "$HOME/private-k38-tmp"
env -u EFS_RPC_PROF -u EFS_REPORT_DBG -u EFS_RECEIPT_TRACE \
  EFS_K38_KEEP=1 TMPDIR="$HOME/private-k38-tmp" \
  python3 tests/test_k38_shared_chunk_pressure.py
TMPDIR="$HOME/private-k38-tmp" make test
EFS_K38_KEEP=1 EFS_GIT_ID=1354d568d EFS_GIT_BRANCH=agent/devel-task-g \
  EFS_GIT_DESCRIBE=1354d568d EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 \
  EFS_BUILD_JOBS=8 bash scripts/make_test_summary.sh
```

Retain each runner's summary, timings, command list and failure output before
starting another gate (the runner replaces them). The runner itself sets
TMPDIR to its private source-tree `tmp/case-N`. A failed K38 fixture remains
without needing a variable; `EFS_K38_KEEP=1` also keeps positive fixtures.
Tests require no preexisting private captures. Negative controls use this
branch's tests with the pre-repair production source selected by
`K38_WRITE_SOURCE`, `K38_HOST_SOURCE`, `K38_FUSE_SOURCE` or `K38_BOUND_SOURCE`,
as documented in those tests.

## Retention

Stable host evidence: `/home/maik/efs-artifacts/taskG-20261026/k38-20261027/`.
`build-environment.json` records full source commits, binary SHA256s, actual
versions and configuration. `negative-controls/` contains each original-code
assertion failure. `repro1`–`repro6`, `untraced1`–`untraced4`, `fsync20` and
`fixed1`–`fixed5` retain separate logs; positive payload/store bulk was pruned
after copying and hash-verifying text captures. The failed `fsync20` fixture remains. `gate-baseline/` contains
the complete failed case 444 copied from the original gate with 7,928 files,
588,943,545 bytes, and source/destination SHA256 comparisons in
`k38-copy-manifest.json`. The original gate's source fixture also remains.
`gate-fixed/` holds the first unprofiled repaired run and `gate-fixed2/` the setup-corrected rerun; controllers and both
failed/successful fast-target logs are separate.

No raw artifacts are in Git. Failed stores remain for the unmerged acceptance
investigation. Passing payloads and source/build bulk were pruned with `edelete` only after
process/mount absence and closed-log copy/hash checks. The original failed
case 444 and failed W166 setup remain at their original paths, as do their
stable copies and the failed fsync-frequency diagnostic. Matching baseline,
`66c78fbdb` and `1354d568d` binaries remain. Positive stores cannot be replayed
after pruning; reproduce them from the source/commands instead. No failed
store was deleted or reformatted by the final pruning; the original fast-test
auto-cleanup limitation is stated in the results table. Details: `PRUNED.txt`, `prune-plan.json`,
`closed-text-copy-manifest.json` (2,027 verified text copies), `edelete.log`
and `final-quiescence.json` (no owned live processes or mounts).

Final text/binary `SHA256SUMS`: **2,147 files, 118,584,305 indexed bytes**;
manifest SHA256
`1e4b7d152bfd17c757708557dbb3542b2270de3666049bd2ec8af9c1e83c1a36`.
Failed store bulk is covered separately by its full source/copy manifests.
All indexing and retention stayed on xorinox; no payload export to the Mac.
Merge into devel and real-cluster/RDMA acceptance remain for the coordinator.
