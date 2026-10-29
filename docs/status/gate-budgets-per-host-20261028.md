# Task T — gate budgets across supported hosts

**INCOMPLETE / fitting cohorts pending.** Final correctness candidate
`547d9e1d0` contains the B6 proof-headroom repair and preserves a full,
on-time three-fragment proof when a speculative degraded mark fails.
Intel1 completed three clean eight-job gates on frozen `458f75e21`;
Xorinox also completed three clean458 gates; fstor007's `547d9e1d0`
series is running.
Nuc and AMD completed failed gates and are excluded from calibration.
All failures remain open evidence. No fitted budget, slot or D44 target
change has been made. Task T is not merged or pushed.

## Authorized host substitution and current-source refresh

The user explicitly authorized AMD, xorinox, node9900, node9901 or fstor007
as alternatives to waiting for nuc/intel2. Fresh probes find AMD and xorinox
quiet with supported FUSE/build dependencies. Nuc and intel2 have Task H
controllers (including queued work); node9900 runs Task AD and an orcd-mini
client; node9901 serves the existing four-member orcd-mini deployment.
Those workloads are untouched. Fstor007 is idle but system FUSE is 3.10.2,
below the required build API; using it would need the already demonstrated
private newer FUSE prefix. AMD is Ryzen AI MAX+395 (16 cores/32 threads),
Ubuntu/ext4 with FUSE3.18.2; xorinox is Ryzen9 7940HS (8 cores/16 threads),
RHEL-family/xfs with FUSE3.16.2. Both calibration pools stay at eight jobs.
No CPU-count equivalence or results for unmeasured hosts are inferred.

The selected calibration hosts are now intel1, xorinox and fstor007,
under the user's explicit substitution authorization. Nuc and AMD failed
on the refreshed source; neither supplies successful-cost observations.
Intel2 remains occupied by other work. The new intel1 cohort covers all
92 budget rows, including the three newer tests and K43, so historical
intel1 observations are not silently pooled with its fresh measurements.
Substitute-host success cannot qualify nuc/AMD or erase their failures.

Devel moved 147 commits beyond the prior freeze. Task T now merges exact
`486aa3a46`, including upstream publication-fence/deadline/index repairs,
truncate-GC fairness, Task W gate coverage and W170. Product source follows
that devel snapshot; Task T retains its fitter, marked-sample refusal,
self-contained owner regression and failure retention. Current devel's
bounded publication setup helper replaces Task T's earlier equivalent
helper call. Runner regressions retain both exclusive scheduling and
external-TMPDIR cases. The current recipe has 92 budget rows and passes its
static model (1533 s/1650 s margin); D44 target1100s is unchanged.
Old-source scoped passes above/below do not qualify the refreshed source.

## Current-source failures, repair and replacement cohorts

Source `4e088356021e7fdbaf573fb94603d0f0d34439e8` is Task T's resolved
merge of devel486aa3a46. Its product files match that devel snapshot exactly;
Task T keeps measurement tools, source-bound owner checks and fixture
retention. Archive SHA-256:
`3524c5a12b9362ce21c5a3de573a115c104ad00cbbee2dec3e9cce10085a6348`.

| Host / attempt | Result | Disposition |
| --- | --- | --- |
| xorinox /4e0883560 calibration6 | Guard recorded Task AF starting after preflight; Task T interrupted | Rejected;17-file manifest verified; repeated ownership scan0 processes/0 mounts; other task untouched |
| AMD /4e0883560 calibration6 | 546 commands,2 FAIL,3 NOTRUN,735s,8 OVERBUDGET,build23s | Rejected; GVFS reopening a test mount caused clean-unmount refusal; B6 slow F0 fsync EIO at60.0s |
| fstor007 /4e0883560 calibration7 | 546 commands,1 FAIL,4 NOTRUN,787s,8 OVERBUDGET,build22s | Rejected; same B6 slow F0 fsync EIO60.0s; no contamination marker |
| fstor007 /458f75e21 calibration8 | Guard refused before run1 because another campaign began | No test or timing collected; prerequisites/source staged only |
| intel1 /458f75e21 calibration8 | Three gates:792/988/818s;546 commands each,0 FAIL,12 NOTRUN;4/5/6 OVERBUDGET | Accepted timing observations; final547 confirmation still owed |
| AMD /458f75e21 calibration8 | 546 commands,1 FAIL,3 NOTRUN,825s,9 OVERBUDGET | Rejected: recurring GVFS busy-unmount refusal; no unchanged retry |
| nuc /458f75e21 calibration9 | 546 commands,5 FAIL,12 NOTRUN,1276s,34 OVERBUDGET | Rejected; stopped before run2; failures detailed below |
| xorinox /458f75e21 calibration9 | Three gates954/988/1001s;546 commands each,0 FAIL,11 NOTRUN;14/14/15 OVERBUDGET | Accepted timing observations; final547 confirmation still owed |
| fstor007 /547d9e1d0 calibration10 | First gate805s;546 commands,0 FAIL,4 NOTRUN,7 OVERBUDGET | Second gate running; cohort not yet complete |

At the user's direction **no message was sent to Task AF**. The xorinox
interruption stopped its spawning controller first, then repeatedly scanned
and drained only Task T's root-owned processes; no other task was signalled.
The interrupted root stays for the open overlap investigation. Nuc later
became genuinely free: Task H's prior controller recorded COMPLETED and
exited, with no remaining observed test jobs; xorinox also became quiet.
Nuc's completed failed cohort is excluded; the chosen substitute cohorts
remain explicitly authorized by the user. AMD's unrelated desktop opener is not disabled,
ignored or claimed fixed, and its failed gates never enter the fit.

The fstor007 attempts use private local `/tmp` scratch and raw evidence under
`/data1/efs/evidence/taskT-20261028/`,96 visible CPUs with eight test/build jobs,
mlx5_0 and mandatory O_DIRECT fragment I/O. Its system FUSE3.10.2 is too old:
the supplied node9901 `/data1/efs/deps/libfuse`3.16.2 package was copied
read-only into the private scratch prefix. Header/library bytes and relative
symlinks were verified unchanged; only the pkg-config prefix was relocated.
Dependency archive SHA-256:
`b42c7df2e4d3aedd8f9d775c587454ef2f3d34c03086d127723713fed4013dde`.
No package or existing deployment changed. This is real-HCA coverage in the
failed baseline, not full fixed-source RDMA qualification.

### B6: proof starts too late when the third COMMIT exhausts the sweep

Both original current-source hosts fail identically: B6 deliberately delays
one member's COMMIT65s. The sweep now shares one60s absolute deadline with
metadata, but joins all COMMIT workers before issuing any degraded mark.
Two fragments are already durable; at60s the remaining worker times out and
the sweep refuses metadata because its budget is exhausted. The record stays
and fsync returns EIO. The retained AMD client log shows fragmentsPDD and
“sweep deadline exhausted; obligations retained.” No payload corruption is
inferred from that refusal; the failed gate does not complete its later
verification phases.

`458f75e2139c0559f59bc0e29b28ab772a566303` preserves the original COMMIT
and whole-sweep deadlines. Healthy fsync still waits for all workers and
needs no degraded mark. In the final reserved quarter of the sweep (at most
15s), two valid fragments per recorded stripe permit metadata proof while
the slow third COMMIT remains in flight. A bounded notification pipe and
release/acquire completion flags avoid reading unfinished worker results.
Every worker is joined before its stack-owned job or pipe is released.
Epochs, ACK watermarks, publication closure, fresh absence checks and durable
marks retain their existing requirements. Fewer than two valid proofs or a
failed mark without a valid full on-time proof stays EIO; no public flag or alternate product mode is added.

The deterministic regression holds all replies, advances the existing test
clock into the production proof window, then releases only the two valid
nodes. It requires the mark before the third worker drains and requires
fsync not to return until that worker finishes. It also checks mismatched
storage verifier and failed-mark cases retain obligations. The new test against
the original4e0883560 implementation fails at `wait_for_marks`, as expected;
the corrected whole durability unit passes native,ASan/UBSan and TSan.
The broader source-bound verifier passes16 native steps and12 ASan/UBSan
steps on the arm64 Mac. No leak qualification is claimed.

Reproduction uses the committed full `tests/test_data_commit.c` and portable
`tests/review_graph_20261028/verify.py`; the real B6 gate is unchanged:

```sh
python3 tests/review_graph_20261028/verify.py --out NEW_NATIVE_DIRECTORY
python3 tests/review_graph_20261028/verify.py --sanitize --out NEW_SANITIZER_DIRECTORY
EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py
```

Raw local root `/Users/mike/Documents/Codex/artifacts/taskT-20261028/`:

| Scoped evidence | SHA-256 of closed `SHA256SUMS` |
| --- | --- |
| `control/headroom-negative` | `46b0b8faf6fd12468bb2f482b01bdaf4cdf414a4cc3df6c96a6215447a9a5137` |
| `headroom-458f75e21-native` | `b9798715f7b48cfb7a5e61cb3cf50f4b42bb0fb3da3f2557219bc8c77fd613eb` |
| `headroom-458f75e21-asan-ubsan` | `6a961dc73eed3b01197e657a8cc79ea6171985e4f7c1afb47780d6f7774f1f5f` |
| `headroom-458f75e21-tsan` | `cd0839a64ba771270f0f81b2d7e9b8d058de05816963be5d1ca66a63a66f1430` |

Repaired source archive SHA-256:
`b7a530603a46032a7269fb4d8b8454e6e7785640a059e86326a9fa4e19110d74`.
All cohorts retain exact controllers, before/after tracked-source hashes,
per-run build identity, commands,timings,failures and guard evidence. They
stop before a next run on any failure or contamination. Source paths and
progress, while active:

- intel1:`/home/maik/efs-taskT-20261028-458f75e21-calibration8/src`, evidence
  `/home/maik/efs-artifacts/taskT-20261028/intel1-458f75e21-calibration8/`.
- nuc,xorinox:`/home/maik/efs-taskT-20261028-458f75e21-calibration9/src`, evidence
  `/home/maik/efs-artifacts/taskT-20261028/HOST-458f75e21-calibration9/`.
- AMD rejected candidate:`/home/maik/efs-artifacts/taskT-20261028/amd-458f75e21-calibration8/`.

### Final proof refinement and source boundaries

`547d9e1d07c8a3f233c283ae44bdcef5e90dab18` adds a conservative full-proof
fallback: a failed speculative degraded mark does not defeat three valid
COMMIT proofs completed before the original sweep deadline. Each worker
release-publishes its completion timestamp; the common proof predicate
checks storage-verifier epoch, ACK watermark and deadline. The original
entry certificates remain unchanged, and live entries are checked again;
new ACKs or replies arriving after the deadline cannot rescue old proof.
Every worker is still joined before releasing its job or notification pipe.
The 65,536-chunk index regression uses455,108 probes, below32*N.

The new late-full-proof regression fails against458 and passes against547;
after-deadline completion and insufficient proof remain EIO. Native16/16,
ASan/UBSan12/12 and the TSan durability unit pass on the arm64 Mac. Closed
local manifests under the raw root above:

| Evidence | SHA-256 of `SHA256SUMS` |
| --- | --- |
| `final-proof-547d9e1d0-native` | `f3a0efa8b7ee70150ff28ecc940329cb4783ac51c8523c7be0b9ea982b502a94` |
| `final-proof-547d9e1d0-asan-ubsan` | `8492eaeba8a79c1aba282d55ce53a9f0216e3e9e4c24aae0b513c5caf7c59eea` |
| `final-proof-547d9e1d0-tsan` | `427d718f6a0e8849943ddee7f08b538de56db5d8b6e17cad02a580aeccaec1c0` |
| `control/late-proof-negative` | `9a97867532aaad72d006d3de31b1b86272afb3593818d97b191dcf439ed62b74` |

The intel1/xorinox timing sources remain frozen458; fstor007 uses547.
These have the same92 budget commands/slot recipe, but458 does not prove
547's new late-full-proof oracle. Final fitted-table confirmations must use
547 plus the budget commit on all three selected hosts. No running source
is edited. Fstor007's547 archive SHA-256 is
`76c2d2cab22dd4da53f727998a4387a9537773e8383b32945b635e785a73335c`.

### Failed refreshed host coverage remains open

Nuc's eight-job458 gate fails five assertions: K38 shared-chunk pressure
reports REPORT BUSY converted to flush EIO; clean unmount fails session
revoke at phase finish; operator follower catch-up sees commit724/applied486;
D4 records one create EIO (1023/1024 files), an11.2s foreground write+fsync
over its10s bound, and too few space samples; publication-abort setup records
one3.15s operation over its unchanged3s bound. Those failures are retained,
not labelled merely slow, not fitted and not repaired by host substitution.
AMD's only failure is the recurring busy unmount. No desktop service was
changed. Both controllers stopped before run2. Their2358 tracked files are
unchanged; closing scans show no owned EFS process or mount left.

Closed text evidence was copied on-host and every file hash verified under
`/data1/efs/evidence/taskT-20261028/`:

| Evidence directory | SHA-256 of `SHA256SUMS` |
| --- | --- |
| `intel1-458f75e21-calibration8` (intel1) | `e3d7e288f62a0ef99c5a077c1057e4c2640868a9274d5dd6271366f5cfa23461` |
| `nuc-458f75e21-calibration9` (nuc) | `c2aaf9231d0a4452e7223a9898c90f02620d08dc5ca56fa6f5bd157bef48df72` |
| `xorinox-458f75e21-calibration9` (xorinox) | `9000a011965dba0767d423402fb5e24858bc9ec19e09bc074ef96cd352db7dfa` |
| `amd-458f75e21-calibration8` (AMD) | `4e348c987aba1c1d4067234afe9ee1c948bdb3d5710e7bec54edfc795c5634ff` |

Failed scratch fixtures stay in their original roots for open investigation;
no stores or payloads were exported. Intel1 and xorinox have visible two-hour bounded
idle reservations, with no EFS workload, pending final confirmation. Each must
be cooperatively released before that gate; it never authorizes stopping
another campaign. No message was sent to Task AF.

Original failed roots remain unchanged for open investigation:
AMD:`/home/maik/efs-taskT-20261028-4e0883560-calibration6/tmp/case-482/efs-b6-4x0lapse`;
fstor007:`/tmp/efs-taskT-20261028-4e0883560-calibration7/tmp/case-482/efs-b6-35g9ox4h`.
Closed baseline evidence manifests were verified:

| Host / evidence root | Files | SHA-256 of `SHA256SUMS` |
| --- | ---: | --- |
| AMD:`/home/maik/efs-artifacts/taskT-20261028/amd-4e0883560-calibration6/` |34| `5683732e691966ae4917419e458f5f4f031a60aa87c2f6815fe8c9da39247d77` |
| fstor007:`/data1/efs/evidence/taskT-20261028/fstor007-4e0883560-calibration7/` |34| `03e3cd84a9a2198e51e09990ee0264f86027fb1608c3e4d7085eec0da89d4cf4` |
| xorinox:`/home/maik/efs-artifacts/taskT-20261028/xorinox-4e0883560-calibration6/` |17| `7b18a2bf121f22d5fa724ff2a10a108512561876f23a8eda21851f1932da7088` |

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
| intel2 / `6e6b95b26`, run 1 | 530 commands, 1 FAIL, 12 NOTRUN, 913 s, 3 OVERBUDGET, build 31 s | K43 fsync EIO after coordinator kill; rejected, fixture retained |
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

## Intel2 K43 publication-fence failure and authorized integration

Fresh source `6e6b95b2633c44acf9e961772b9d545a988867b3` includes devel
`424d0db8f`, Task T tooling/retention and Task H's runner-only isolation;
it does **not** contain Task H's product-repair branch. After the previous
owner's explicit release and quiet checks, its first full intel2 gate ran
530 commands in 913 s: **1 FAIL, 12 NOTRUN, 3 OVERBUDGET**, build 31 s.
Only K43 failed. The controller stopped before run 2 and all 2,304 tracked
source files remained unchanged. No failed timing is a calibration input.
Archive SHA-256: `3bf85a270f54615319c91d34b59116ab68bee1a0ac04176448f109fd6cc2706d`.

K43 used its unchanged six-node loopback TCP fixture and 16 MiB baseline.
Baseline payload verification succeeded before the fault. The test parked
publication after PREPARE on node 2, SIGKILLed that coordinator, then failed
on the writer's fsync EIO. The client logged two durable fragments but no
durable degraded marker (`NOT_FOUND`, -3) for several stripe obligations.
There was **no post-fault payload verification**: this is neither proof of
corruption nor proof of successful durable publication. The hypothesis is
unclosed publication authority for abandoned replay PUTs, matching the core
repair below; integration qualification must establish whether it fixes this
failure rather than relying on Task H's broader branch results.

The original fixture stays unchanged at
`intel2:/home/maik/efs-taskT-20261028-6e6b95b26-v3/src/tmp/case-472/efs-k43-hvxyg4sb`.
Evidence root is
`intel2:/home/maik/efs-artifacts/taskT-20261028/intel2-6e6b95b26-v3/`:
`fail-472.log`, all seven client/daemon logs copied under `failure-k43/`,
binary hashes and build identity, and `fixture-manifest.json` inventorying
all original fixture files. That inventory is preservation evidence, not a
payload oracle. Every evidence manifest hash was rechecked; SHA-256 of
`SHA256SUMS` is `3d8a4cebc3081010f07b37d227b32f646d842a71f7996d1333c7ade2feba58b2`;
fixture manifest SHA-256 is
`2c2588383d95b663891eaa9769b1064a80123d0b1d4e8d7b39feeda04f314d52`.
Final ownership closure records zero owned processes and zero owned mounts.
No failed store or payload was copied to the Mac.

With explicit user approval, `f99cfc72e` ports the core of Task H
`214c541aa68a1c6611746365a1fc03a7a5543ec4`: an unreferenced, no-longer-putting
PUT needs durable SETTLE or exact terminal ticket state, followed by a fresh
metadata absence check, before fsync may retire its stripe obligation.
BUSY, missing ticket state and transport errors prove no closure. FileID and
object matching remain exact. There is no encoding change and no relaxed
fsync verdict. Unit regressions cover those negative outcomes, a late fence
requiring the second lookup, and generation mismatch.

The first portable verification attempt retained a harness failure: the newly
added ticket-owner test linked an absent prebuilt `libefs.a`. All other scoped
checks passed, but the attempt remains FAIL. `cb363ba8a` makes that test link
its real common helpers directly and honor sanitizer/memcheck plumbing.
On exact source `cb363ba8a7fa2bd9779b8642ca467cf63e718100`, all **15 native**
steps and **11 ASan/UBSan** steps of the portable verifier pass on the arm64
Mac (Apple clang; no leak claim). Both include the real ticket owner,
receipt/resolve tests, full data-commit test and Raft memory/disk tests;
native additionally covers frame, transaction retry, recipe and defect
regressions. Commands, compiler/platform, per-step logs and source hashes
are retained outside Git:

```sh
python3 tests/review_graph_20261028/verify.py --out NEW_NATIVE_DIRECTORY
python3 tests/review_graph_20261028/verify.py --sanitize --out NEW_SANITIZER_DIRECTORY
```

Local raw root:
`/Users/mike/Documents/Codex/artifacts/taskT-20261028/`.
Closed `SHA256SUMS` digests:

| Directory | Verdict | SHA-256 of manifest |
| --- | --- | --- |
| `qualification-publication-fence-native` | FAIL: missing library prerequisite on f99cfc72e | `af5df73da666df3e1b94025924200ae10a6e49e9585dd750b24e6d5f41866cf6` |
| `qualification-publication-fence-asan-ubsan` | FAIL: same prerequisite | `06d2055ecf9a1ec5fbed0a9ed9132a738a22d185e7ef5a61e2021bd58f1a6340` |
| `qualification-publication-fence-portable-native` | PASS scoped, cb363ba8a | `85a19f443fadb260a8bad0babb2dcda99fbce30cec8da46041b5c7f0bcb23ac9` |
| `qualification-publication-fence-portable-asan-ubsan` | PASS scoped, cb363ba8a | `60e109cc97b260a66dfbd45dd038ac06c74890ecb7430a0d0c50da83fd285c41` |

The fixed source archive's SHA-256 is
`adc45a0f3656555bb95237a3a35a2b971aa38bffec4c1c59d692894c4d171508`.
Required-host Linux pools and confirmations remain unqualified. On xorinox,
after two quiet checks, the same archived source built successfully and the
native plus ASan/UBSan owner checks passed. **One six-node K43 run passed**:
both overwrite clients completed after coordinator SIGKILL, no recovery-age
abort was observed, and a fresh mount read the final 16 MiB payload exactly.
The K43 subprocess took **55.854 s including cleanup** (the test's earlier
8.9 s PASS print precedes cleanup). All 2,304 source files remained unchanged.
This is a scoped cross-host result, not a same-host before/after proof for
intel2's failure, nor a substitute for its full calibration pools.

Commands on a fresh archived tree, with explicit source/build ID and private
TMPDIR, in order:

```sh
python3 tests/test_put_ticket_owner.py
EFS_TEST_SANITIZE=1 ASAN_OPTIONS=detect_leaks=0 python3 tests/test_put_ticket_owner.py
make -j8 all tests/faults/efsd
python3 tests/test_k43_coordinator_kill.py
```

Every command exited 0. Build took 17.651 s; native owner 0.615 s and
instrumented owner 2.571 s. Xorinox is an AMD Ryzen 9 7940HS (8 cores/16 threads), Linux x86_64.
The K43 fixture uses loopback TCP, mandatory O_DIRECT fragment I/O
(D42, no mode flag) and 1 GiB quota per node; the test's 16 MiB phases and deadlines were
unchanged. Raw logs, build/binary hashes, environment, exact bounded
controller and source manifests are retained at
`xorinox:/home/maik/efs-artifacts/taskT-20261028/xorinox-cb363ba8a-publication/`.
All 14 manifest entries were verified; SHA-256 of `SHA256SUMS` is
`ea02f0e0fe97e804cbee5787d9c7250743d2739bc74df648af63c59c71e7be11`.
Successful K43 fixture cleanup follows the existing test; no failed intel2
fixture was touched. A broader eight-job Linux integration gate at
`xorinox-cb363ba8a-scoped-full/` reported a **pre-fault setup failure**:
`test_pub_unit_forward_abort.py` received `raft-step-down rc=-13` (target
not yet caught up) and asserted immediately, before the publication workload.
The frozen gate completed **530 commands, 1 FAIL, 12 NOTRUN, 910 s,
15 OVERBUDGET, build 11 s**. K43 passed in that pool (56.9 s), but the whole
gate stays FAIL and no timing from it is a calibration input. Its 2,304 source
files remained unchanged, no contention marker was recorded, and final
ownership closure found zero owned processes and mounts. All 27 evidence
manifest entries were verified; `SHA256SUMS` SHA-256 is
`051e73519241d0a725dc6efbb1877cd2914bb9033f4bd848aa07cdcf34172760`. Preserved
`fail-471-preserved.log` has SHA-256
`db783eeac9ed509dd85015847d149a18fb88b2332189c96059764cb01d74604f`.
That old test's finally block removed its fixture even on failure, so the
raw assertion survives but that setup's stores do not. This is separate from
the retained intel2 K43 fixture and does not qualify an abort scenario that
never began.

`608ea0ea6` adopts only the corresponding harness hunk from Task H
`bee20f99d`: use the already present `place_leaders` helper, which retries only
documented pre-fault BUSY within its existing 30-second setup deadline and
fails on other errors; retain failed fixtures. The twelve measured rounds,
three-second bound and all payload/abort assertions stay unchanged. This
avoids adding a second leader-placement helper. Nine graph checks and the
syntax check pass. One fresh live run on exact source 608ea0ea6 **PASS**:
all 12 rounds had the forwarded PREPARE refusal, write+fsync max **0.42 s**
within the unchanged 3.0 s bound, no recovery-age ABORT, every file exact.
Build took 17.801 s and the test 17.855 s including cleanup. Command:
`python3 tests/test_pub_unit_forward_abort.py`, after the same explicit-ID
`make -j8 all tests/faults/efsd` build in a fresh private archived tree.
The topology is three loopback TCP daemons, mandatory O_DIRECT fragments,
1 GiB quota per node. All 2,304 source files remained unchanged; final checks
found zero owned processes and mounts. Raw evidence:
`xorinox:/home/maik/efs-artifacts/taskT-20261028/xorinox-608ea0ea6-forward-abort/`.
All 12 manifest entries were verified; `SHA256SUMS` SHA-256 is
`82598abceedcf2ff85b3b0502d81ffb78167b36198638971b45f49414555f6b9`.
This targeted result does not turn the preceding full gate into PASS or
qualify a full gate on 608ea0ea6. The archived cb363ba8a tree was not edited during its run. Candidate source
`608ea0ea65134d319cc681fdf9e81ca5984fe338` archive SHA-256:
`0b97f7c169ebf11f1c85a68d01117c3491aa1e9b215f235b995c100781fb0430`.
Nuc and intel2 remain reserved by Task H until its explicit release.
The last intel2 check still found Task H's active full suite and queued
next attempt; no replacement Task T cohort was launched there.

After both xorinox runs closed and repeated zero-owned-process/mount checks,
`~/bin/edelete --force --delete` pruned only Task T's closed scratch roots
`/home/maik/efs-taskT-20261028-cb363ba8a-xorinox` and
`/home/maik/efs-taskT-20261028-608ea0ea6-xorinox`. Their source/build/runtime
paths are historical and reproducible from the named commits. All cited
logs, results, manifests and exact controller sources remain under the
separate evidence roots. Cleanup receipt and `PRUNED.txt` are under
`xorinox:/home/maik/efs-artifacts/taskT-20261028/xorinox-publication-cleanup/`;
its three-file manifest SHA-256 is
`195da9bf9b22edf69f3420b3ef5433568a00bdb8ef5fd9c56315db45705aa9da`.
No Task T run remains active on xorinox; the original intel2 K43 fixture
and all earlier unresolved nuc/intel2 evidence remain untouched. This is an integration of one core fix, not a
claim to qualify Task H's larger repair branch. The canonical graph was rebuilt
and all nine graph checks passed after the source changes.

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
2. Fixed candidate 608ea0ea6 is archived after the user-authorized core fix
   and bounded setup correction. Its scoped passes do not qualify full pools.
   After the host is released,
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
