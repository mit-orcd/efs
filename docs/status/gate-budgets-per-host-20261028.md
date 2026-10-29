# Task T — gate budgets across supported hosts

**FITTED / focused repair qualification only.** The user narrowed remaining
work on October29 to focused testing. Full calibration, budget refitting and
confirmation gates are held; the task is not declared complete. Directory
candidate `35a6e41b0` passed three full gates each on intel1 and xorinox
(548 commands, zero FAIL per run), in addition to its isolated controls.
Fstor007 stopped after a separate K34 REPORT BUSY-to-write-EIO failure.
Candidate `288cb4e65` adds stopping-leader release, failed-read-hint exploration
and a complete fresh-log shutdown-marker check. Focused RDMA qualification
is pending below. No passing result is claimed for this candidate yet.
All92 bounds, slots, deadlines,1100s target and1650s margin remain unchanged
(model1504s). No merge into devel or push.

## Current focused scope — October29

Source `288cb4e650233e182b32f73b6164290b49773695` includes product ports
`68f8c7fc0` and `ea77b3a35` from Task H's reviewed stopping-leader and
read-routing repairs, plus test-only `288cb4e65`. Local deterministic native
controls passed; the read-routing negative control fails on the earlier
source and ASan/UBSan passed. These do not establish live RDMA correctness.

Run on idle fstor007, three loopback daemons with real RDMA (`mlx5_0`),
mandatory direct I/O and private FUSE3.16.2: build `make -j8 test-bins`, then
`python3 tests/test_shutdown_bounded.py`, `test_metadata_retry_budget.py`,
`test_pub_unit_setup.py`, `test_dir_migrate_visibility.py`; ten serial
`test_k34_stop_put.py`, three `test_shutdown_blocked_io.py` and three
`test_k43_coordinator_kill.py` invocations. Each Python filename is under
`tests/` and invoked with `python3`. Each live invocation uses a fresh private
TMPDIR. Existing payload, durability and deadline assertions are unchanged.
K34 forces the stopping node to lead both groups before any workload;
`EFS_K34_KEEP=1` and `EFS_K43_KEEP=1` retain fixtures pending capture closure.
Stop at the first unexpected failure, NOTRUN or competing workload. Diagnostic
times are not calibration inputs. Canonical capture is
`fstor007:/data1/efs/evidence/taskT-20261028/fstor007-288cb4e65-k34-focus20/`;
manifest and result are pending. Exact source archive SHA-256:
`08bd44e0a41b6689efc4cd0d6eb3c3ad0ed7729688b80cb813e35c1c934d96c1`.

### Completed directory-candidate full gates

| Host at35a6e41b0 | Outcomes | Test walls / build | OVERBUDGET | Closed manifest SHA-256 |
| --- | --- | --- | --- | --- |
| intel1 | 3×548,0 FAIL,11 NOTRUN each | 813/920/909s;31/0/0s build | 1/1/1 | `7215ee621b67f0ad6cf21f3ec5f251a26689ccc1aa4f5d894af165cd4aeb0e33` |
| xorinox | 3×548,0 FAIL,10 NOTRUN each | 966/903/978s;0s builds | 1/1/2 | `539d0bf1eef065d46f9f0b3d8d58ec23302fd3d252c355530987d4c7a1205c9d` |
| fstor007 | 548,1 FAIL,3 NOTRUN; stopped before run2 | 722s;23s build | 1 | `01132fd42d69f526fe6ed8e3bab4676efb67419296bda4d18f007fc302c7f8d7` |

Captures: `/data1/efs/evidence/taskT-20261028/HOST-35a6e41b0-calibration19/`.
All closed with unchanged source and no owned daemon/mount or contamination.
Fstor007 case476 failed at metadata REPORT BUSY (`-13`), surfaced as EIO
at12:54:00.774 UTC. **Evidence limit:** the old K34 finally block removed
its failed fixture and complete daemon/client logs. Only captured failure
output survives, recorded in `EVIDENCE_LIMIT.txt`. No post-error payload or
durability verification is claimed, and the original cause is not proven.
The revised K34 harness preserves failures and makes cleanup errors fatal.
Later successful runs cannot reconstruct that missing evidence.

## Refresh interrupted by a new namespace failure

| Host at b13b8867e | Full gates | Test walls | OVERBUDGET | Closed manifest SHA-256 |
| --- | --- | --- | --- | --- |
| intel1 | 3 ×547, zero FAIL,11 NOTRUN each | 940/920/882s | 1/0/0 | `6f313735124eea2ba9884dbd76570d6624dd4bb414609ff001b6c115e05c3555` |
| fstor007 | 3 ×547, zero FAIL,3 NOTRUN each | 782/881/778s | 0/1/1 | `65d6fcb0b2891578f16cf82cf3d0f11e8b4107de67dcb456c04b0305ae784f25` |
| xorinox | 547,2 FAIL,10 NOTRUN; stopped before run2 | 953s | 2 | `71d3cf1c00ddaefb42e222fc503961d304ee23e39658070f37f691014a7476fa` |

All canonical captures at
`/data1/efs/evidence/taskT-20261028/HOST-b13b8867e-calibration17/`
are finalized and copied as text-only evidence with every hash verified.
These successful observations are retained, but they cannot qualify the
subsequent product repair. Original W130/B6 failure stores stay on xorinox;
forensics and the new counterexamples are on the linked page.

Saved inventory observations from all three640265efc confirmations and these
b13b8867e runs replay against their saved references and physical headers:
60 result files/60 stable observations, zero stable missing records, and no
inconclusive changed references in this set. This does not recover the absent
mapping capture from the first63c6c9cb9 failure or establish its cause.

The35a6e41b0 full cohort is completed above, rather than still running.
Its new deterministic unit adds one executed command (548); the92 budget
rows were unchanged. No failing or contaminated run enters any future fit.

## Second confirmation and fresh measurements — October29

Exact tested source640265efcdcffa3d315055ecbe898038ff632635:

| Host | Commands / FAIL / NOTRUN | Test wall / build | OVERBUDGET | Closed capture manifest SHA-256 |
| --- | --- | --- | --- | --- |
| intel1 | 547 /1 /11 | 764s /31s | 0 | `9df5d7223bc0c21851cd0fde08fb0a20e175ec7669e0c6f1c7b6803e6755e993` |
| xorinox | 547 /1 /10 | 904s /24s | grow-b133.5s vs120s | `0fe69e0c7553553b49853bf6bd419eb08f8f237c5684484c054939ba3eb986ed` |
| fstor007 | 547 /1 /3 | 863s /23s | D4 progress205.2s vs115s; duplicate candidate68.8s vs55s | `558c21ae65cb4562ef71b4442dccab3c27c7e7ad131c937dcbdc2487a4a24fd7` |

All three report FileNotFoundError saving
`geometry/UNIQUE_FIXTURE/test_b_geometry-summary.json`. Their full-capacity
assertions reached the summary, but failed evidence persistence still makes
the command and gate FAIL. Stopped fixtures remain at case472 under each
scratch root; canonical captures, including finalized log copies and the
structured growth observations, are at
`/data1/efs/evidence/taskT-20261028/HOST-640265efc-confirmation16/`.
Closure checks find unchanged tracked source, no contention marker and no
owned EFS process or mount. These facts do not qualify the failed gates.

Correctionb13b8867e22c0a798c3cb7ea5b3ee84c0c8932a5 has the same product,
oracles, budgets and deadlines, with the directory created independently of
which phase runs. Geometry status controls, graph9/9 and docs5/5 pass.
New measurement captures are
`/data1/efs/evidence/taskT-20261028/HOST-b13b8867e-calibration17/`, with
LAN scratch `/home/maik/efs-taskT-20261028-b13b8867e-calibration17` and
fstor007 scratch `/tmp/efs-taskT-20261028-b13b8867e-calibration17`.
The pristine archive SHA-256 is
`52325b5921cf723103bb1490be0ac707594357431ec6a49f84ad57cd72bc85b8`.
Their retained controllers stop before the next run on any failure or marked
contention. They retain per-run geometry, B6 and W130 text captures. Every
host passed a fresh ownership/quiet preflight; fstor007's private FUSE prefix
is copied and hash-verified except its relocated pkg-config prefix. No
existing deployment or Task AF was changed or messaged.

The AMD focused W165 success scratch was pruned only after finalized capture
verification and zero-owned-process/mount checks. Its canonical41-file text
capture remains, with supplemental PRUNED.txt SHA-256
`60e397942b9643467888320226714ee8bf54e654c3c9fedbf3be1d294d03692e`.
All failed AMD stores remain retained.

## First fitted-table confirmation: not accepted as a complete gate

Exact source63c6c9cb967f97fc3e675f1ec96a89956d43df87, archive SHA-256
`8d8f728c30c5b5aeacd25394ea46c9cf9ee2d28994d9fc43241467d4bc4521c6`:

| Host | Commands / FAIL / NOTRUN | Test wall / build | OVERBUDGET | Closed capture manifest SHA-256 |
| --- | --- | --- | --- | --- |
| intel1 | 547 /0 /11 | 763s /31s | 0 | `59d7903e99dc01e512ed2652a59fa25756fb2c61427bfe7d10024a1e68a05955` |
| xorinox | 547 /1 /10 | 907s /24s | K22 leader admission94.8s vs90s | `171077bd0ce7d036910c2e5d9ea93fb5ffd5526d23a5efd1810de853471b1311` |
| fstor007 | 547 /0 /3 | 890s /22s | D4 progress168.8s vs115s | `32e9f0bfb3a5b10ac86fc6af8e9fd704f5eb3506b1dd994aadd4d2f087c048fb` |

Captures are on their hosts at
`/data1/efs/evidence/taskT-20261028/HOST-63c6c9cb9-confirmation15/`.
There is no contention marker; tracked sources are unchanged, and closing
checks find no owned EFS process/mount. The reduced NOTRUN counts follow the
sanitizer informational-marker correction, not newly claimed product coverage.
The two overruns are retained counterexamples to zero-overrun qualification;
no bound has been inflated from either single sample.

### Seven-node inventory observation and evidence limit

Xorinox grow-b fails at seven nodes: B1 checks988 conditions and reports5
missing packed-fragment roles for b2-1 and small-0. Exact payload read checks
had no reported mismatch. The old driver fetched rows once before its physical
scan, with no after-scan recheck. Packing/compaction can change those references
and retire their records during the scan. This is a credible capture race,
**not a proven explanation of the original failure**: its messages omit the
segment IDs, the pre-scan mappings were not retained, and the driver unlinked
its own workload on failure. The stopped stores, finalized daemon logs and
existing result JSON remain; they do not recreate that point-in-time capture.
No missing-fragment claim or retroactive PASS is inferred from the evidence.

`640265efcdcffa3d315055ecbe898038ff632635` fixes the observation method and
retention. It reads all references, scans stores, then reads references again.
Only equal references spanning the scan qualify; changed references produce a
recorded inconclusive observation and another bounded metadata-only capture.
The existing --pack-wait bounds capture; no application read/write is retried,
no EIO or malformed row is forgiven, and stable missing records fail at once.
Immutable object generations and the fixture's lack of application writes
make a retired reference unable to reappear as an ABA during this bracket.
Continuous change expires as FAIL. Reads use the driver's existing bounded
parallelism. Missing-record messages now include segment ID, generation and
group. Full before/after rows and physical record inventories are serialized
for offline replay, alongside every inconclusive observation.

Failed checks/exceptions now keep the workload and write a failure snapshot;
only successful fixtures are eligible for cleanup. Geometry artifacts are
namespaced by unique fixture directory to prevent parallel phases overwriting
one another. Deterministic controls cover a switch during scan, stable loss,
continual-change timeout, metadata read errors, JSON replay of the captured
failure and failed/explicit-keep fixture preservation. The driver/status units
and graph9/9 pass. There is no product code, budget or deadline change.

The candidate's archive SHA-256 is
`a222fef4782940c7e83099f986aaa18ded2b85e002afdda706e87ed8ed7a40eb`.
New one-gate confirmations are at
`/data1/efs/evidence/taskT-20261028/HOST-640265efc-confirmation16/`,
with private LAN scratch `/home/maik/efs-taskT-20261028-640265efc-confirmation16`
and fstor007 scratch `/tmp/efs-taskT-20261028-640265efc-confirmation16`.
Their exact retained controllers set EFS_B_ARTIFACTS, EFS_B6_ARTIFACTS and
W130_EVIDENCE_DIR to separate per-run text-evidence directories. This preserves
the structured geometry/B6 oracle results and finalized W130 logs even after
successful synthetic fixtures are pruned. All earlier failures/stores remain
on-host; Task AF has not been messaged. Results are pending, not accepted.

## Final measurement set and fitted table

[All92 per-host medians and bounds](gate-budget-medians-20261029.md).
These are the accepted whole-gate input cohorts; every command returned0,
with host limits reported as INCOMPLETE rather than full PASS:

| Host | Source | Walls s | NOTRUN per gate | Old-table OVERBUDGET per gate | Closed manifest SHA-256 |
| --- | --- | --- | --- | --- | --- |
| intel1 | d2edef6e9 | 911/944/958 | 12/12/12 | 5/3/7 | `78399947535c0fa7e240734c082f4b0898dfbdb5286857ffeb590902d3e06284` |
| xorinox | d2edef6e9 | 929/945/964 | 11/11/11 | 15/15/19 | `f19f844540611610e72a03c10a3e144f74fb69891c3119ba067d2107772be7ac` |
| fstor007 | f27251313 | 775/739/707 | 4/4/4 | 7/8/6 | `6f6756574c7a71f0eb0afef6f97df1be4c85ecc22a118f1882e32e74bd3db35b` |

On each host these immutable captures are under
`/data1/efs/evidence/taskT-20261028/HOST-SOURCE-calibrationN/`, with N12 for
the d2 LAN cohorts and N14 for fstor007. Their full source IDs, binary hashes,
compiler/FUSE/filesystem metadata, before/after tracked-source hashes,
commands, timings and guards are captured. Closing checks find no owned
EFS process or mount. Bounded idle holds reserve the three hosts pending
confirmation and are released cooperatively before the next workload.

The offline copies and derived fit are at
`/Users/mike/Documents/Codex/artifacts/taskT-20261028/fit-inputs/` and
`fit-final-92rows-20261029/` under the same raw root. Every copied input hash
was verified. Fit report SHA-256:
`e3a0c14a71262ff07ae34a5d8b42f6e77bd55261834ca1c0306051203bee72ff`.
The table's row values match the generated output exactly; only its header
was updated to describe this measurement. No source gap is hidden: LAN uses
d2, fstor007 uses the fault-only W165 boundary correctionf272. Confirmations
use one final merged source on all three hosts, including devel714d7e6fc's
ORCD runner/readiness follow-up and new fast unit.

### The target is still exceeded

The old declared schedule1533s becomes1504s (pool953 + exclusive551), not
1100s. The margin remains1650s; neither constant was raised. The synthetic
maximum-host-median schedule improves1122.0→1102.9s, still2.9s above target.
It mixes per-row observations from different hosts; it is not a real run or
a bound on future runtime. The recorded real measurement walls above all
exclude separately recorded build time. Confirmation results are pending.

The largest declared rows are snapshot import425s (xorinox median337.9),
report stepdown315s (intel1 median250.8), segment rebuild290s (xorinox228.5),
B6 slow270s (intel1 214.0), K33 growth225s (fstor007 180.0), and degraded
fsync205s (fstor007 160.9). In the modeled pool's critical chain, stop
progress175, grow-b120, B6 slow270 and degraded fsync205 are the large links;
report stepdown315 is the dominant serial/exclusive contributor. These are
where target work remains, rather than changing D44 to match the results.
Every over300s row retains its historical reason and the measured fit reason.

### Reporting and model corrections before confirmation

`f03074744` removes the word NOTRUN only from the sanitizer self-test's
unconditional successful-classification message. Real unsupported-runtime
markers, failing fixtures and errors remain. Earlier summaries always chose
that first informational marker, so they do not establish a real attach
refusal, contrary to the earlier interpretation. Their command timings stay
valid, but discarded successful stdout cannot prove detailed old runtime
coverage. The final source will report actual limits correctly. The revised
self-test passes on the Mac: TSan and ASan/UBSan controls run, while LSan is
explicitly unsupported there. Raw log:
`control/sanitizer-selftest-output-correction.log` under the local raw root.
This is gate self-test coverage, not whole-EFS leak qualification.

The recipe model also now counts four temporary C compile-and-run wrappers
that start with `set -e;`. They are shared prerequisites, so remain outside
the fast/long duplicate test, but run once in the full-gate model. No command
execution or time limit changes. The actual checker agrees with the fit's
1504s schedule. Graph9/9, refit regression and the current961-command recipe
check pass. The final archived confirmation contains547 executed commands
(one fast ORCD unit added since measurement), all92 budget rows, and the
unchanged slot reservations. Its acceptance still requires0 FAIL and
0 OVERBUDGET; every real NOTRUN remains visible.

## Authorized host substitution and current-source refresh

The user explicitly authorized AMD, xorinox, node9900, node9901 or fstor007
as alternatives to waiting for nuc/intel2. Initial probes found AMD and xorinox
quiet with supported FUSE/build dependencies. Nuc and intel2 then had Task H
controllers (including queued work); node9900 runs Task AD and an orcd-mini
client; node9901 serves the existing four-member orcd-mini deployment.
Those workloads are untouched. Fstor007 was idle but system FUSE was 3.10.2,
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
| fstor007 /547d9e1d0 calibration10 | Gates805/835/967s; third FAIL1 on B6 full | Closed/rejected for final fit; retained quota fixture and new correction below |

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

### B6 full-storage setup: failed third sample and unchanged oracle

The547 fstor007 cohort ended with805/835/967s gates; the third has1 FAIL,
4 NOTRUN and8 OVERBUDGET. It failed only B6 full mode: W0 and W1 succeeded,
then W2 and Q0–Q3 returned ENOSPC. This is not accepted calibration or a
passing quota gate. Its final node3 usage was16,736,256B under its16,777,216B
quota. The packer moved the FS seeds at00:54:25.030UTC, between the first
quota refusal at00:54:24.793 and the later writes. A refused1MiB upload also
does not prove there is no room for a smaller upload. No quota overrun or
payload mismatch was established; there was no contamination marker.

`d2edef6e94555f540f93f82d7a0f0b8024aeb2cb` keeps the exact all-ENOSPC,
no-degraded-acknowledgement and later exact-payload assertions. It changes
only how the full fault is established: wait for every P/FS seed to pack,
then for their old ordinary fragments to disappear on all three nodes;
validate those inventory paths against R0 so absence cannot pass vacuously;
fill with pinned128KiB files (one64KiB fragment per member), rather than
partially filling/refusing a multi-chunk1MiB file; require exact node3
quota headroom below64KiB. The seed settle shares the existing PACK_BOUND_S,
no test deadline is raised. The filler cap now bounds512 single-chunk files
rather than64 eight-chunk files, the same maximum data amount. No GC,
packer, quota check or success/error oracle is disabled or relaxed.

The graph's quota-setup call chain was checked against source and retained
logs. Python syntax, graph9/9 and the recipe/model gate pass; live validation
of the corrected fixture is pending. Its archive SHA-256 is
`e8f6bd54e856c9f808329c91e2ceb538aed89ded5e056fb3ba75acb65b306bcc`.
The workload changed, so even the previous clean intel1/xorinox cohorts are
historical observations, not the final fit inputs. All three selected hosts
now have fresh three-run controllers with that same source, at eight jobs.
The earlier bounded idle holds were released cooperatively before launch.

Current scratch: intel1/xorinox
`/home/maik/efs-taskT-20261028-d2edef6e9-calibration12/src`; fstor007
`/tmp/efs-taskT-20261028-d2edef6e9-calibration12/src`. Evidence on each host:
`/data1/efs/evidence/taskT-20261028/HOST-d2edef6e9-calibration12/`.
The failed previous fstor007 cohort is closed at
`/data1/efs/evidence/taskT-20261028/fstor007-547d9e1d0-calibration10/`,
64-file manifest SHA-256
`a2942abb10e9eb589f3772ac8203c27c51f7f9e9131f0ffd9297c3d542c5f509`.
Its seven B6 logs are copied there; its original stopped stores remain at
`/tmp/efs-taskT-20261028-547d9e1d0-calibration10/tmp/case-483/efs-b6-rfa0dv3o/full`
for the open investigation. Closing checks found no owned EFS process or
mount. No Task AF message was sent.

### W165 fault boundary: premature observation, corrected under EFS_FAULTS

Fstor007's first d2 gate ends546 commands,1 FAIL,4 NOTRUN,805s,7 OVERBUDGET,
with no contention marker. The only failure is W130's serve-first W165
scenario: it sees INCOMPATIBLE before issuing its direct CREATE requests.
The hook damages the row in maintenance at01:14:12.831UTC; the shared
stored-row diagnostic appears5ms later. That diagnostic does **not** identify
which reader observed it, so the test's old “prior apply” wording is not
proof that apply was the consumer. Query/maintenance reads can also observe
stored rows during that window. This gate remains FAIL, not fitted. Its
B6 full case did complete successfully after the d2 fixture correction;
that scoped observation does not make the whole gate valid calibration.

`f2725131366377f4f6710b9d5f8bd287a066b28d` changes only the EFS_FAULTS
boundary and its oracle: the split worker holds migration and atomically
publishes a target inode; after the intended CREATE passes ReadIndex, the
hook claims its arm file once, damages that matching parent row, and the
normal parent read handles it. The test still requires no prior halt,
three distinct direct CREATEs, healthy-quorum application operations without
retries, a frozen halted cursor and recovery by snapshot resync. It now also
requires an explicit successful CREATE-boundary injection diagnostic. No
normal-build admission or recovery rule changes, and no production toggle
is added. Atomic rename prevents exposing an empty ready file.

AMD was quiet for a private eight-job compile plus three serial focused
`python3 tests/test_w130_dir_fault.py` runs, TCP loopback, three nodes,
O_DIRECT, at f272:21.961/19.657/19.404s, all PASS, each consuming the hook.
No mounts or owned EFS processes remain, and2358 tracked files are unchanged.
These are scoped diagnostics; AMD's failed full gate remains FAIL.
Exact build/test controller, finalized daemon/mount logs, binary identities
and41-file manifest are retained at
`/data1/efs/evidence/taskT-20261028/amd-f27251313-focus-w165/`, manifest SHA-256
`2a589bb9b7a418512bfb7f55c4701bbf9a294f6747563e91f56da8622e2a32b6`.
The f272 source archive SHA-256 is
`442105245f94b762198a933c04f8a335be034834d0afd475e5a9b282f12006dd`.

The failed fstor007 d2 gate is closed at
`/data1/efs/evidence/taskT-20261028/fstor007-d2edef6e9-calibration12/`,
32-file manifest SHA-256
`62ab21e16d4bbdd9d5b11ffebb1ffecee8dab5a4ccfda5b54351e36d75dee3a9`.
Its original stopped W165 stores remain at
`/tmp/efs-taskT-20261028-d2edef6e9-calibration12/tmp/case-506/efs-w130-li6i1s7c/w165-serve`;
closing scans found no owned process/mount. No failed case was retried inside
that cohort. Fstor007 now has three fresh serial f272 gates, raw evidence
`/data1/efs/evidence/taskT-20261028/fstor007-f27251313-calibration14/`,
scratch `/tmp/efs-taskT-20261028-f27251313-calibration14/src`.

Intel1/xorinox continue their unchanged d2 cohorts rather than editing a
running source. Their successful W165 runs exercised the intended requests
without a premature halt; f272 makes that boundary deterministic. Sources
must be reported separately. All92 budget commands and slots remain the
same; final fitted-table confirmations on every selected host must use f272
plus the table, explicitly qualifying the new hook on intel1/xorinox too.

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
stop before a next run on any failure or contamination. Historical source and evidence paths (superseded):

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

The superseded intel1/xorinox timing sources remain frozen458; the
superseded fstor007 series uses547.
These have the same92 budget commands/slot recipe, but458 does not prove
547's new late-full-proof oracle. Final fitted-table confirmations must use
f27251313 plus the budget commit on all three selected hosts. No running source
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
no stores or payloads were exported. Intel1 and xorinox had visible two-hour bounded
idle reservations, with no EFS workload. Both were cooperatively released
before the d2edef6e9 cohorts; they never authorized stopping another campaign. No message was sent to Task AF.

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

1. Finish the isolated directory qualification and three fresh35a6e41b0 gates per authorized substitute
   host (intel1, xorinox, fstor007), accepting no failed or contaminated gate.
   The earlier Nuc/AMD failures remain open and are not qualified by substitution.
2. Replay every retained structured growth inventory observation against its
   saved references and physical headers. This does not recover the missing
   references from the first failed confirmation or prove its cause.
3. Refit the92 rows from complete clean cohorts using
   `scripts/refit_test_budgets.py --host-runs HOST RUN1 RUN2 RUN3` for each host
   in one invocation. Preserve slots and reason prefixes,1100s target and
   1650s margin. Report the modeled excess and any guard failure honestly.
4. Run the resulting table through confirming full gates: zero FAIL and zero
   OVERBUDGET are expected; host-limit NOTRUN remains visible. A failed
   confirmation remains failed, with investigation before further changes.
5. Finalize/verify manifests, commit the concise evidence and prune only closed
   successful scratch via `~/bin/edelete --force --delete`, after ownership and
   mount checks. Keep raw captures and open investigation stores. No merge into
   devel or push is authorized by this task.
