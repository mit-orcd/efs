# W95: TCP/direct-I/O small-file packing qualification

## Scope and current state

User authorized root-only work on `devel`, exclusive AMD use, ordinary files
storage engine/TCP/direct I/O with small-file packing enabled. Client
`EFS_COMMANDS=0` and daemon `EFS_COMMANDS_SERVER=0` throughout. Containers,
RDMA and query/cache work are outside this task. No push or release/tag changes.

The mounted packing fixture previously enabled command/query features by default.
It now forces both command flags off and selects the original core POSIX suite,
independently of inherited command-enabled environment. Its summary records the
packing and command flags. Physical packing, mutation, peer, cold-byte and
reclamation assertions and original deadlines remain unchanged.

Current candidate: **eb2fad1b76e5311087076f70d2c3544a62825edf**, with a bounded
metadata insertion buffer, shorter FIFO receive slices and reserved ReadIndex
peer connections. The two-RPC follower experiment was reverted. Targeted safety
gates pass, but full live repeatability remains **FAIL**. Full current-source
normal/fault native reruns PASS.
The earlier deep-directory failures are not closed by a single passing run.
See the final checkpoints below for each tested SHA and its exact verdict.

Historical baseline source: **0dcce384fa442479070a31464777f104c1fe7829**.
Memtable insertion optimization and GC shutdown correction pass normal/fault
native, fully instrumented LSM/model/GC safety gates, focused orderly packing
and acknowledged-fsync crash/cold/reclamation gates on AMD. The isolated insertion
benchmark improves 13.8x; this is not an end-to-end throughput claim.

Full seven-client packing remains **FAIL**: clients 2 and 6 exceed the unchanged
30s deep-directory deadline (1510/1512 core PASS; peer67/67 PASS). All four
daemons then stop cleanly within 20s. That full run's cold restart is **NOTRUN**
after an explicit 4G reserve refusal, separately from the core timeout failures.
Fresh focused cold/crash gates subsequently PASS after generated-build cleanup.
No timeouts, quotas or storage reserve rules were weakened. Historical failed
candidates below are retained; later documentation-only commits do not change
the tested product/test inputs.

## Reproduction

On a Linux AMD host with compiler/dependencies, FUSE, Python 3 and screen, check
out the named `devel` commit from the result checkpoint. Build with `make -j8`.
Use two existing root parents on the designated EFS test filesystem, never an
existing live storage directory. AMD permits private FUSE mounts under `/tmp`;
mounts under arbitrary `/data1` subdirectories are denied by AppArmor. Create
empty `/tmp/efs-pack-focused-a` and `/tmp/efs-pack-focused-b` first. Four fresh private nodes use explicit 4G quotas.
The harness retains raw output in its unique `gc-direct-*` directory; keep that
outside Git. Ensure ports 34840–34843 are unused and use an isolated empty mount
allowed by the host's FUSE policy. No profilers or competing builds during quiet
qualification.

```sh
EFS_PACKED_WRITES=1 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_REPAIR_EXPERIMENT=0 \
python3 tests/live/gc_reclamation.py --mode direct --transport tcp \
  --root /data1/efs/packing-qualification/root-a \
  --root /data1/efs/packing-qualification/root-b \
  --mount /tmp/efs-pack-focused-a --peer-mount /tmp/efs-pack-focused-b \
  --port 34840 --fixture-quota 4G --mounted-packing-only --disable-efs-commands
```

The focused run creates 64 concurrently fsynced 512-byte files, requires actual
packed fragments, verifies peer bytes, promotes/grows one file, truncates another,
unlinks one, checks unaffected neighbours, restarts all four nodes, verifies
acknowledged bytes, deletes survivors and requires physical packed reclamation
and metadata queue drain within the original 180-second deadlines.

Then repeat on fresh fixture directories/ports with
`--posix --packing-clients 7 --posix-jobs 16`. Expected: 216 core cases per
client (1512 total), original 30-second per-case deadlines, peer suite 67/67,
clean shutdown, the same cold hashes and physical reclamation. Harness `finally`
cleans owned processes/mounts; retain failed fixtures externally for diagnosis.
Inspect cleanup output before deleting only those private fixture directories.

## Remaining gates

Focused mounted lifecycle; full seven-client quiet acceptance; meaningful fault
and sanitizer regressions for identified defects; repeated current-source cold
and shared-object reclamation; matched packing-on/off throughput, durable-close
latency, allocation and CPU comparison. Record exact executed commit, binary
hashes, host/configuration, verdicts, raw location and limitations here.

## AMD checkpoint: first complete quiet run

AMD Ryzen AI Max+ 395, kernel 7.0.0-38-generic, GCC Ubuntu 15.2.0-16ubuntu1.
Four fresh daemon processes on the same physical host/filesystem, explicit 4G
quotas, seven FUSE clients, TCP/direct/files, packing1/commands0/server0 and
experimentalrepair0. This is single-host functional qualification, not a
multi-host network or hardware failure test. `/proc` environment inspection
verified those flags on all four executing daemons and seven executing clients.

Source archive SHA-256:
`93b62a4b7ad8948d9b16f617e4ecaa254079216423da58bbb68cd2c6e2dd3a0b`.
Build: `make -j8 EFS_GIT_ID=bdbb237bcd1e EFS_GIT_DESCRIBE=bdbb237b EFS_GIT_BRANCH=devel`.
Full normal native: PASS. Nine focused ASan/UBSan actual-function/extractor gates:
PASS; these do not instrument all linked code or entire live daemons. Repeat
with `CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g'`,
`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`
on tests/test_{mounted_pack_retry,mounted_pack_batch,mounted_pack_parallel,
mounted_pack_admission,mounted_pack_capability,packed_gc,packed_gc_budget,
packed_terminal_publication,packed_publication_progress}.py after building libefs.a.

Normal executable SHA-256:

| Executable | SHA-256 |
| --- | --- |
| efsd | d848d3e829207a59048e926e8f962bbf0a2daa138ef558c6593bbf8195589ecc |
| efs-fuse | 641973504868e510a2a650ca4678251c4a9f9579fe1dd8f39cc43ddc57eb5680 |
| efs-mgmt | 4bf48c97c06e6560f0b4acf8c5579d66bb7ad536ac3685ee79aa3501f6f8f05e |

Focused runs `gc-direct-opfswayu` and `gc-direct-jq2i84ot` PASS. First full
seven-client fixture `gc-direct-1xsa9gh2` PASS: each client216/216, total1512/1512;
peer67/67, acknowledged warm/peer/cold mutations and neighbour bytes, physical
packed-object empty, metadata queues drained, orderly daemon/client shutdown.
Original 30-second case and 180-second reclamation deadlines remain unchanged.
No query workloads, tracing or concurrent builds during live qualification.
The initial64 files contain32768 payload bytes and produced33 packed fragment
files/270336 file-length bytes (8.25×). This excludes checksum/metadata files,
filesystem allocation rounding and NAND writes; it is not a throughput or matched
baseline result. Both successful focused runs show the same lifecycle guarantees.

Raw output remains outside Git on `amd_efs` under
`/data1/efs/packing-qualification-20261012` and
`/data1/efs/packing-qualification/root-a/gc-direct-*` (second storage parent root-b).
Fixture summaries include binary/source identities, core TSVs and cold/physical
verdicts. The command recipes above reproduce without requiring those captures.

### Preserved setup failures and limits

The initial filtered archive omitted historical documentation link targets and
stopped make test at the link gate. Actual referenced files were deployed and
the unchanged full native suite passed; no placeholder files/check bypass used.
The first live attempt failed before serving because its generated `/data1`
mount path was denied. The retry uses permitted private `/tmp` mount paths.

An earlier seven-client attempt passed all1512 core and67 peer checks but could
not restart with its explicit 4G quota: the startup guard reported safe maximum
1.97GiB. It does not certify cold recovery. A cleanup attempt first refused
because expected manifests were absent; no storage was removed by that attempt.
Only eight inactive storage roots (`evidence/n1` through `n4`) from two completed
private fixtures, `amd-release-final-1e3a4638-ordinary-20261012` and
`amd-v031-15e2a531-20261012-live`, were then removed after terminal-result and
process-ownership checks. Their logs/verdicts and the new packing failures remain.
Free space increased184→198GiB; successful rerun kept4G quotas and reserve policy.
All these failures are setup/headroom evidence, not proof of a packing data defect.

## Additional acknowledged-fsync crash gate

The default lifecycle still requires orderly status-zero daemon shutdown. An
additional `--packing-crash-after-fsync` mode kills all four private daemons by
SIGKILL after fsynced packed/mutated bytes and drained clients, asserts each actual
SIGKILL exit, then applies the same cold-neighbour and physical-reclamation
oracles. It is valid only with `--mounted-packing-only`. Add this option to the
focused reproduction above, using fresh roots and unused ports. This tests
process-crash recovery after acknowledgements; it does not simulate power loss,
mid-flight failures or missing-node repair. The additional gate passed on the verified 951f417d experiment; this is process-crash evidence, not quiet workload qualification.

## Repeatability failure: qualification remains open

The second quiet seven-client run (`gc-direct-xohqe2w2`) has two core failures:
clients1 and5 `dir_deep_nesting` exceeded the unchanged30-second deadline;
other1510 checks pass. A passing first run does not qualify packing. Peer/cold
checks continue independently; subsequent physical reclamation cannot convert
this failed core run to PASS. Native/fault and nine focused sanitizers remain
PASS for their scope. Next: bounded original-workload phase/RPC diagnostics with
packing enabled and both command flags off; distinguish creation, fsync and
recursive cleanup before changing product scheduling or ownership. No deadline
increase, reduced concurrency or stale authority reuse is justified.

## Bounded receive-batch experiment (not accepted)

The same-source instrumented run `gc-direct-856vxbp0` passes1512 core checks,
67 peer checks, cold bytes and physical drain. It remains diagnostic rather
than quiet acceptance. Seven bounded deep-case records show directory creation
19.059–19.962s, leaf write0.242–0.445s and recursive cleanup1.292–1.470s;
total21.054–22.011s. Representative leader LOOKUP averages1831us, of which
1768us is parent ReadIndex and32us KV work; full-run counters are not causal
per-case measurements. These observations point to metadata read/progress under
packing load, not a proven cohort fsync bottleneck. No cached proof/TTL change.

Candidate experiment increases the existing bounded FIFO prefix32→64 while
retaining the256-frame inbox, byte credits, ordering boundaries, durability and
fresh read-proof admission. Hypothesis: amortize durability/progress cycles
under extra packing metadata traffic. This can worsen single-cycle latency;
keep only after focused inbox/read safety checks and improved repeated quiet
full workload results. Restore32 if benefit is not demonstrated. No production
rollout or release qualification transfer is authorized by this experiment.


## Experiment verdict: discard 64-frame batching

Verified candidate `951f417d5ef80b15d9b57b1603ce39f531acd204` passed normal
native tests, eight inbox/read-authority ASan/UBSan gates and the additional
acknowledged-fsync SIGKILL/cold/physical-reclamation gate. Its first quiet
seven-client run failed: clients3 and5 exceeded the original30-second
`dir_deep_nesting` budget (1510/1512 core checks passed). Peer67/67,
cold bytes and physical/metadata drain passed independently. Planned quiet
repeats2/3 were NOTRUN after that failure. Per-client suite durations49.4–53.0s
show no demonstrated improvement. Restore the32-frame limit; retain the crash
test. Packing remains unqualified.

Deployment verification mattered: an old documentation-target archive also
contained source files and initially overwrote the experiment. Those native
results are excluded from candidate qualification. Re-extract the candidate
archive last, verify all779 code/test/build inputs against its Git snapshot,
then clean-build. The corrected input manifest aggregate SHA256 was
`0552d52daa2b8cd4a2a07b79eb640c9a229a63826b4e8e7b13d81c4df4072354`;
corrected daemon SHA256 was
`e5752a86ae2cef740647d60ae5315fe295798a435a2b660bc4811029edd7c288`.
Raw logs and manifests remain outside Git under the AMD qualification directory.

Reproduce using the commands above at951f417d: run `make clean`, identified
build and `make test`; run the focused fixture with
`--packing-crash-after-fsync`, then the seven-client fixture on fresh roots and
ports. For sanitizer tests which treat CC as one executable, use a compiler
wrapper invoking `/usr/bin/gcc -fsanitize=address,undefined
-fno-omit-frame-pointer -g "$@"`; set both CC to the wrapper and put its `cc`
name first in PATH. Gates: host_inbox, readindex_completion, readindex_publish,
readindex_operation, readindex_pair, readindex_remote_pair, read_admission and
session_read_proof. Restore normal compiler settings before live testing.

Next diagnostic: profile the baseline32 daemon CPU during the same seven-client
workload, with bounded deep-case phase tracing. Earlier creation consumed19–20s
while the leaf write consumed less than0.5s; fresh metadata ReadIndex waits
remain the leading measured suspect. Diagnostic instrumentation is not quiet
acceptance. Preserve deadlines, concurrency and read authority.


## Baseline CPU diagnostic after discarded trial

Run `gc-direct-991q63kx` used the unchanged bdbb237b baseline32 product/binaries,
packing enabled and both command flags zero, seven clients/jobs 16,4G per-node
fixture, TCP/direct. Three client suites failed (2,4,5), each on dir_deep_nesting at30s (1509/1512 passed); peer67/67, acknowledged
cold bytes, physical fragment reclamation and metadata queue drain passed.
The owned fixture exited1 and cleaned its processes/mounts. This instrumented
run is diagnostic, not quiet acceptance; raw data remains external.

Reproduce the seven-client command above with `EFS_POSIX_PHASE_TRACE=1`,
`EFS_PACK_PROF=1`, `EFS_RPC_PROF=1`, `EFS_READ_PROF=1`. Once all seven private
FUSE clients mount, attach only the four fixture daemon PIDs:
`perf record -F 199 -g --call-graph dwarf,8192 -p PID1,PID2,PID3,PID4
-o /outside-git/packing.data -- sleep 35`; then
`perf report --stdio --no-children --percent-limit 1 -i /outside-git/packing.data`.
Do not attach normal services or run another fixture simultaneously.

44,456 samples included222 lost samples/16 lost chunks, so percentages are
approximate and profiling perturbs acceptance timing. Self CPU: memmove7.82%,
kv_mtab_pos1.92%, merge_scan1.74%, CRC1.52%. The sampled memmove stack reaches
kv_mtab_set through committed LSM batch/apply and Raft receive. Inclusive kernel
cost includes futex13.66% and writev8.77%; inclusive figures overlap ancestors
and must not be summed. The current sorted memtable shifts entry/prefix arrays
for each inserted key while holding the metadata mutex. This is a measured
optimization target, not proof of the timeout cause or justification to weaken
fresh ReadIndex admission. Next: measure/reduce insertion movement with exact
ordering/atomic-batch/failure regressions, then rerun quiet original budgets.


## Committed memtable batch merge candidate

W95 candidate replaces repeated committed-batch insertion shifts with a prepared
sorted batch and one backward merge. Single-item writes retain the existing
path; update-only batches use binary-search replacement without walking the
whole table. Duplicate keys retain the last original operation. Preparation and
capacity allocations finish before committed entries change. WAL order, apply
serialization, durable-error poisoning, metadata dirty hints and read proofs
remain unchanged. On RAM apply allocation failure the durable record is retained
and the store refuses further writes until recovery, as before.

Local regression PASS: `python3 tests/test_kv_mtab_batch.py`; sanitizer PASS:
`CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer -g'
python3 tests/test_kv_mtab_batch.py`. Checks2000 mixed binary-key model batches,
PUT/DEL duplicates, exact bytes/accounting/order, every26 allocation refusals,
and reduction of repeated suffix movement. The regression is part of `make test`.
AMD durable LSM/native/fault and live packing qualification are pending at this
candidate checkpoint; no timeout or authority change, no release promotion.


### First merge candidate: safety PASS, quiet packing FAIL

Exact source `43c2f17f4ba478ecf73494ebedfa5875c0f8546a`: full normal/fault
native PASS, fully compiled-and-linked ASan/UBSan LSM suite PASS, memtable
model/allocation sanitizer PASS. Verified778 build/test/dependency inputs,
aggregate SHA256 `efacd22e321c85ad417c7a3325539d8704632423775a3b90297e2ae1ae97b1ad`.
Daemon SHA256 `88bf4a31a5c24cd75f6561d73f3334f4ebe1a085175675f04599c431f17ec9e5`.
Quiet fixture `gc-direct-1gdx3sqm` FAIL: all seven deep-directory cases exceeded
30s (1505/1512 core PASS). Peer67/67, cold bytes, physical packed reclamation
and metadata queue drain PASS independently. Planned repeats and crash rerun
NOTRUN after the failure; owned fixture stopped. Four daemon command lines
confirmed direct I/O/4G and all eleven daemon/client environments had packing1,
commands0/server0. TCP is selected by EFS_TRANSPORT, not a command-line token.

Refinement: avoid comparing/dereferencing every unrelated old key in the merge.
Binary-search the untouched prefix and move each intervening contiguous range
once; ranges do not overlap with later source prefixes. The model, all 26
allocation refusals and ASan/UBSan checks still pass locally. AMD qualification
of this refinement is pending; earlier gates do not transfer to the new source.
Use `python3 tests/measure/kv_mtab_insert.py --keys 20000 --batches 1000
--batch-size 16 --repeats 5` for an isolated, alternating sequential/batch
comparison. This measures memtable insertion only (zero-byte values), not
end-to-end packing throughput or latency. Raw outputs stay outside Git.


### Contiguous-range candidate: core PASS, shutdown FAIL

Exact source `0dde0e2240259c16e80cdc044e8fb2990f314c58`: normal native and fully
compiled/linked ASan/UBSan LSM/model gates PASS. Verified779 inputs, aggregate
SHA256 `9fdd2a372107a3d3c69456298828bd662cbb6b1b12ee7a737c9694105c3cdc16`.
Five alternating AMD microbenchmark pairs: sequential median0.028021222s,
batch median0.002087781s (13.42x) for 20000 existing keys plus1000 front batches
of 16 zero-byte-value keys. This is insertion cost, not cluster throughput.

Quiet fixture `gc-direct-tx79i4x9`: core1512/1512 PASS, peer67/67 PASS; whole
run FAIL because node2 exceeded unchanged20s orderly shutdown. Cold/reclamation,
subsequent quiet repeats, crash and final fault-native gate NOTRUN by the runner;
cleanup stopped all owned processes. Actual four daemon/seven client environments
verified packing1/commands0/server0/EFS_TRANSPORT=tcp; daemon flags direct/4G.

Node2's reap_sweep consumed26111ms while issuing repeated400ms apply waits after
peers stopped. The outer inode loop checked gc_running, but the inner64-lane
loop continued after cancellation. Fix: synchronize all GC stop flag accesses,
check cancellation between lane proposals and before finalization, and refuse
new GC proposals after stop. Keep the pump/peer pool alive for the existing
in-flight operation; do not delete unfinished durable markers or weaken ACKs.
Regression `python3 tests/test_gc_reap_shutdown.py` tests cancellation at every
local/remote lane boundary with OK/BUSY outcomes and subsequent marker replay;
existing GC verdict test also rejects proposals after stop. Local normal and
ASan/UBSan PASS; AMD integration at the new source is pending. Deadlines remain
30s core/20s shutdown/180s physical and metadata drain.


## Final integrated AMD checkpoint

Exact executed source `0dcce384fa442479070a31464777f104c1fe7829` includes the
contiguous-range merge and GC cancellation correction. Verified 780 source,
test and dependency inputs; aggregate SHA256
`5558865f42b9e8e8fcb3065b1624658403b75712a9810b36c7372390d126d09f`.
Harness product-source digest
`bdd097e8ecb1e0e28c7b16488459a95b2fea1361f9f745345c6b4072848cda6f`.
Normal binaries rechecked unchanged after testing:

| Executable | SHA256 |
| --- | --- |
| efsd | f76027879f6307c83ef24d6c938bc830e4eb8af737b24471e482be17228126ab |
| efs-fuse | 3e2f53b7bd151db62c285662efa5c728514a0c8b73d9866cdd03b50b72ead139 |
| efs-mgmt | 68d78b5b777bf255fb917154a907c568e788ab7bae6c6364ad447ea4ad1712ed |

Same AMD395/kernel7.0.0-38/GCC15.2 environment, four private daemon processes
on one host, files/TCP/direct,4G per node, packing1/commands0/server0/repair0.
Seven-client workload: jobs 16 per client, original 30s core cases;20s daemon
shutdown;180s physical and metadata drain. No profiler or competing build in
quiet measurement. Focused durability workloads use two FUSE clients and64
concurrently fsynced512-byte files plus ordinary growth/truncate/unlink.

| Gate on final tested source | Verdict |
| --- | --- |
| Full normal native `make test` | PASS |
| Full fault native `make EFS_FAULTS=1 test` | PASS |
| Fully instrumented/linked ASan/UBSan `test_kv_lsm` | PASS |
| Memtable model/all 26 allocation refusals, sanitizer | PASS |
| GC lane cancellation and local/forwarded verdict sanitizers | PASS |
| Quiet seven-client core | FAIL: 1510/1512, deep-directory clients2/6 timeout30s |
| Quiet peer suite | PASS: 67/67 |
| Quiet all-four orderly shutdown under retained GC backlog | PASS:status zero within 20s; no forced kill |
| That full run's cold/reclamation | NOTRUN:4G quota exceeded safe max3.91G at restart |
| Planned quiet repeats2/3 | NOTRUN after first failed full run |
| Fresh focused orderly packed/ordinary mutations, cold bytes, physical/metadata drain | PASS (`gc-direct-25rcqk55`) |
| Fresh all-four SIGKILL after fsync/client drain, cold bytes and physical/metadata drain | PASS (`gc-direct-tgeq4otc`) |

Quiet failed fixture `gc-direct-mb5oi_tr` remains externally with its failure
logs and runtime stores. Its full lifecycle is not accepted. Successful focused
reports explicitly record TCP/direct and packing1/commands0/server0. Crash
acceptance covers process death after acknowledgements; power loss, missing-node
repair and multi-host networking remain outside this task. Broader W95 format,
migration and matched end-to-end performance acceptance are not closed.

Five alternating isolated insertion pairs on this final source:

| Method | Median seconds | Min–max seconds |
| --- | ---: | ---: |
| Original sequential insertion |0.028853582|0.027963574–0.041234950|
| Contiguous batch ranges |0.002086209|0.002082502–0.003035597|

Median ratio 13.8x for 20000 existing keys and 1000 front batches of 16 zero-value
keys. This removes repeated suffix movement; it does not prove that insertion
was the sole source of mounted latency. The remaining deep-directory blocker
needs fresh metadata ReadIndex/progress/GC scheduling measurements at this
source, with the same budgets and no authority/TTL shortcuts.

### Reproduce final checks

Check out the exact SHA above. In separate normal/fault/sanitizer source
checkouts (avoid mixing instrumented objects):

```sh
make clean
make -j8 EFS_GIT_ID=0dcce384fa44 EFS_GIT_DESCRIBE=0dcce384 EFS_GIT_BRANCH=devel
make EFS_GIT_ID=0dcce384fa44 EFS_GIT_DESCRIBE=0dcce384 EFS_GIT_BRANCH=devel test
python3 tests/measure/kv_mtab_insert.py --keys 20000 --batches 1000 --batch-size 16 --repeats 5
# Separate fault checkout: clean; same build/test commands with EFS_FAULTS=1.
```

For the sanitized checkout, use a single executable compiler wrapper which
executes `gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g "$@"`.
`make clean; make -j8 CC=/absolute/path/to/wrapper tests/test_kv_lsm`, then run
`./tests/test_kv_lsm` with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. With CC set to that wrapper, run
`tests/test_kv_mtab_batch.py`, `tests/test_gc_reap_shutdown.py` and
`tests/test_gc_verdict.py` under the same sanitizer options. The instrumented
LSM library is linked into its test, not just the test translation unit.

Use the focused live reproduction above with the normal binaries; use fresh
unused ports35260 (orderly) and35270 (add `--packing-crash-after-fsync`). For
seven clients use35210 plus `--posix --packing-clients 7 --posix-jobs 16`.
Ensure enough free filesystem capacity for the unchanged4G quota at both start
and cold restart. Harness-created roots are fresh and retained; `finally`
cleans owned processes/mounts. Inspect status-zero shutdown and exit verdicts.
Never classify the expected reproduced full-workload failure as PASS because
independent lower-layer checks pass. No old-binary compatibility work is needed.

Raw outputs, manifests and input maps are external on AMD under
`/data1/efs/packing-qualification-20261012/`, prefixes `final-*`; fixture reports
are under `/data1/efs/packing-qualification/root-a/gc-direct-*`. No raw capture
is committed. Useful integrity hashes:

- full quiet failure log: `b0be87f16cb551a3cc57fe3a718dff17b2ec7ad8f4d8c46d74e52ef4bc85a952`
- focused orderly log: `bf55be245fc7c8a0a55749a5f2ea70e6e4bfb48811151a6810509944ddf77eb6`
- focused crash log: `417f5ef022eefb21225b036a7212266163f42ee76805a249f9596e1a4cc252ba`
- normal native log: `10275f3f95c995b2f098cc7947b80de7869377c098bca6e782ce4a3c297aaa8f`
- fault native log: `03b707afcb14ba3132be102c19696c20f9ef106b521b4d9cb137f78c229b6fe3`

Cleanup removed only generated builds from completed private checkouts and
runtime stores of two earlier verified PASS fixtures, retaining their logs,
manifests and reports plus all failed/current stores. No normal service or
unrelated path changed. All owned qualification screens/processes are stopped;
no push, tag or main/release promotion.

## Deep-directory proof scheduling follow-up

Root-only AMD work on `devel` retains files/TCP/direct I/O, packed writes,
client `EFS_COMMANDS=0`, server `EFS_COMMANDS_SERVER=0`, four explicit4G quotas,
seven clients/jobs16, and the original30/20/180-second case/shutdown/drain
budgets. No reserve, freshness, TTL, concurrency or test-shape relaxation.

The current-source diagnostic before these changes (`0dcce384`, product/test
inputs also used by documentation checkpoint330fac5c) retained five deep-case
failures in `/data1/efs/packing-qualification/root-a/gc-direct-r_x13bjs`.
Creation took24.920–28.421s; the two cases that completed spent another2.573–2.575s
in recursive cleanup. Failed cases have no completion record: their cleanup
length is unknown. These observations identify the exhausted case budget rather
than a stuck leaf write. Instrumented results are diagnostic, not quiet acceptance.
The CPU recording lost1012 samples/25chunks; its global/inclusive costs cannot
be attributed exclusively to deep creation or added together. The node3
cumulative read counters included71438 remote proofs averaging2909us; these
are whole-fixture observations, not per-case latency or helper-saturation counts.

A wake-predicate/single-notification experiment was committed as `c759376b`.
Its focused native/ASan/UBSan tests and full native suite passed, but the
seven-client live gate failed all seven deep cases. One run was confounded by
an owned `du` inventory that had remained active; it was stopped and that result
is explicitly excluded as a quiet comparison. A subsequent quiet retry also
failed all seven. Peer/cold/physical/metadata checks passed independently.
`9252bfc6` reverts the experiment; it is not an accepted performance fix.
The failed/raw evidence remains outside Git. No failed runtime stores were
deleted: automatic approval review rejected that cleanup. Only reproducible
generated binaries/objects in inactive older source copies were cleaned to
provide headroom; logs, source, manifests and runtime stores remain.

`a863af5a` starts a locally led proof before synchronously waiting for the other
leader's proof. Mixed leadership no longer needs the eight-helper pool, whose
saturation previously fell back to serial proofs. Both-follower cases retain the
bounded pool/scalar fallback. This preserves fresh pre-send admissions and
ordinary term/quorum/applied-prefix checks. Full normal/fault native and targeted
ASan/UBSan gates passed. Quiet round1 passed1512/1512 core checks; round2 still
failed one deep case (1511/1512). Peer67/67, orderly shutdown, cold bytes and
physical/metadata drain passed in both. Round3 and the planned additional crash
gate were NOTRUN after the failure. The overlap change alone is not qualification.

`4d1f0e33aa9ea0b8bfd8df2b279fc35071a44bb7` additionally records bounded
next-round demand under the existing Raft mutex. The pump begins that requested
round when the older pending round completes, avoiding the reader wake/lock/kick
handoff before sending probes. Two fields per group coalesce concurrent demand;
no pending ACK set is reset, no completed old proof covers a new admission, and
term/role/replica changes discard demand. There is no new read authority, lease
or permanent worker allocation. The regression executes the actual helpers for
112-reader coalescing, unchanged pending ACKs, fresh prefix, no idle probe loop,
term/role/removal and send-error retry. Existing real read-proof tests continue
to check own apply, post-send refusal and context invalidation.

That next-round experiment passed native/sanitizer safety and one quiet full
round, then failed all seven deep cases on quiet round2 (1505/1512). A separate
instrumented diagnostic also failed all seven. Commit `611ef6d4` reverts it;
next-round demand is not retained as an accepted fix.

`661f5282253f28314b18d0ae433e8620a4010475` lets the existing strict Raft
read-control guard acknowledge a safe probe before unrelated queued replication
work. Pending frames still enter the normal ordered receive/apply path. Term,
leader, membership, matched-prefix and durability checks remain mandatory.
Full native, six targeted ASan/UBSan regressions, and the fully instrumented
Raft suite with normal/early-ACK modes passed. Quiet round1 still failed one
deep case (1511/1512); peer, cold bytes, orderly shutdown and reclamation passed.
The other planned quiet/crash/fault runs were NOTRUN after that failure.
An all-actor diagnostic failed three deep cases (1509/1512), with the remaining
peer/cold/reclamation checks passing independently. The bounded35s/99Hz recording
had zero lost samples; 99.62% of sampled CPU was in daemons and memmove took6.89%
self cost, including large committed-table insertion. These are diagnostic
sampling costs, not end-to-end throughput or exclusive per-case attribution.

## Bounded metadata insertion buffer

Candidate **4734e1a40e29ab76c52682b17b4ad24a64f4649d** reuses the sorted
memtable implementation as a newer insertion buffer. It merges into the large
main table at1024 keys or256KiB. Gets and ordered/prefix/resumed scans select
newer buffered entries first, including tombstones. Flush materializes both
before segment publication/WAL reset; snapshots account for both tables before
allocating and preserve their original isolated view. WAL sequence/apply order
and durability remain unchanged. This changes no disk or wire format.

The deterministic distributed-key regression starts with8192 keys and adds4096
keys in128 batches of32. Pointer/prefix movement falls from20794768 to2112048
bytes (9.85x less), with identical entries and ordering. This is a copying-work
measurement, not a live performance claim. The model also covers2000 mixed
binary-key batches, all26 preparation allocation refusals, and independent
merge-capacity refusals that preserve both owned tables.
The real-LSM regression covers buffered replacements/tombstones, point and
ordered/prefix/resumed reads, snapshot budget/isolation, manual flush, and a cold
WAL tail. Initial working-tree regression passed; exact-commit gates follow.

Build identity: `EFS_GIT_ID=4734e1a4 EFS_GIT_DESCRIBE=4734e1a4
EFS_GIT_BRANCH=devel`, GCC15.2, AMD395/Radeon8060S,32 logical CPUs,
Linux7.0.0-38-generic. All781 tracked Makefile/src/include/tests/scripts/deps
inputs verified remotely; sorted path/hash manifest digest
`88dc08c580af4fb3ca72edacb0153b212f294f0f310780bf096a977b6f5a4ed3`.
Raw output is outside Git in
`amd_efs:/data1/efs/packing-qualification-20261012/insertion-*`.

Exact-source normal/fault native and fully instrumented LSM/model sanitizer
checks PASS. Quiet round1 (`gc-direct-6vt8o53m`) passed1512/1512 core and67/67
peer, cold bytes, physical reclamation, metadata drain and four orderly exits.
Round2 (`gc-direct-iv76fiyw`) is FAIL:1498/1512 core PASS, with seven deep-directory30s
timeouts, four names_crazy_dirs30s timeouts, plus EIO in concurrent_write_and_readdir,
rename_to_self and dir_many_files. Peer67/67 and cold/physical/metadata/shutdown
checks passed independently. Planned round3 and additional crash were NOTRUN
because the qualification runner stopped on round2's final failure.

A separate diagnostic (`gc-direct-nwpez910`) passed all1512 core plus lifecycle
checks. Its35s/99Hz all-daemon/client recording lost zero samples; memmove self
cost was2.67% (previous661f recording6.89%), while kv_mtab_pos rose to3.01%.
These separate sampled runs are descriptive, not a controlled throughput result.
Deep creation took25.352–26.795s, cleanup2.049–2.267s, and complete worker
budgets28.488–29.669s. Live-phase pump mutex hold maxima reached136786us.
Cold restart elections are ordinary term changes, not evidence of live leader
churn. Whole-fixture remote proof/catch-up counters cannot attribute a case's
failure. Buffer correctness/copying improvements do not close live repeatability.

Binary SHA256, normal4734 build:

- efsd: `b79807ad44202426bf035e42cc14fb40f059fe632182b0d971040076f696ea06`
- efs-fuse: `35093a7d4a31882eb0ee86dcd6ef332fe05e8d6e810a600ae81704a69af268da`
- efs-mgmt: `1a12677c78975e9b417e944300922257f90f781f83234e2f11fb7122c2a2b3f2`

Use the reproduction commands above with seven clients/jobs16 and fresh ports.
Native: `make -j8 && make test`; fault native: a separate clean build,
`make -j8 EFS_FAULTS=1 && make EFS_FAULTS=1 test`. ASan/UBSan: a separate clean
build with a compiler wrapper adding `-fsanitize=address,undefined
-fno-omit-frame-pointer -g` to every compile and link; run test_kv_insertion_buffer,
test_kv_lsm, test_kv_snapshot_tail, test_kv_seg_index, test_kv_reader_gate and the
Python memtable model. Set `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
UBSAN_OPTIONS=halt_on_error=1`.
For diagnostic reproduction, set `EFS_POSIX_PHASE_TRACE=1 EFS_READ_PROF=1
EFS_RAFT_OBS=1`, wait for the committed phase stream's `event=create`, and record
only owned fixture daemon/client PIDs using `perf record -F99 -g --call-graph fp
-p <comma-separated-PIDs> -- sleep 35`; keep resulting artifacts outside Git.
Do not use instrumented runs as quiet acceptance.

## Shorter FIFO receive slices

Candidate **77c1d0d813a62c3cad074eeafb73da82ac97fb89** lowers the hard
inbox prefix from32 to8 frames so apply/read progress publishes between shorter
lock-held receive batches. Existing global control/configuration/read fences,
per-group message order, queued-tail precedence, deferred durability credits
and backlog eventfd wakes are unchanged. The actual FIFO scheduler regression
passes. Same-source full normal/fault native and seven targeted read/FIFO sanitizer
gates PASS. Initial quiet core passed1512/1512 and peer67/67 with four clean
exits (`gc-direct-m20wtyfa`), but cold restart was NOTRUN after storage refused
4G against a1.50GiB safe maximum. This is a reserve refusal, not verified cold
bytes or a deep-case failure. No quota/reserve relaxation was made.

Only inactive generated build outputs and verified PASS fixtures' node stores
were reclaimed; logs, manifests, reports and every failed store remain.
A fresh quiet retry (`gc-direct-w05_ax1b`) then failed1499/1512 core: seven
30s deep-directory timeouts and six30s names_crazy_dirs timeouts. Peer67/67,
cold bytes, physical reclamation, metadata drain and orderly shutdown passed
independently. Other planned quiet/crash repeats were NOTRUN. Shorter slices
alone are not accepted as closing live repeatability.
Build with `EFS_GIT_ID=77c1d0d8 EFS_GIT_DESCRIBE=77c1d0d8 EFS_GIT_BRANCH=devel`.
Verified781-input manifest digest:
`6f7caf7320a755deb09ab75c16e7bc525ea606d00d031dd0586ff44da42ebaf6`.
Raw output: `amd_efs:/data1/efs/packing-qualification-20261012/slice-*`.
Same live reproduction, fresh ports36110,36120,36130; no timeout increases.

## Bounded follower proof overlap

Candidate **1cb3b20345a1c07cb32d11f917a9ebc7ba0b294e** allows two remote
ReadIndex RPCs in flight per group instead of one. Existing caller threads own
both; no detached workers or unbounded RPC fanout. A reader still records an
admission floor beyond every already-started RPC. The next fresh request can
reach the leader while an older RPC returns. Results publish only in increasing
local admission order, so a late older reply cannot replace newer proof/error
state. The leader's existing fresh-round/term/quorum/applied-prefix guards and
follower catch-up checks remain unchanged; no cached proof grants authority.

The actual threaded coalescer regression checks a held first request, concurrent
second request, blocked third admission, maximum two owned RPCs, out-of-order
completion, newer errors, late older replies, a new request after completion,
and counter exhaustion. Full normal native and eight targeted ASan/UBSan
read/FIFO/coalescer regressions and full fault-enabled native PASS. The quiet
live run failed:1493/1512 core cases passed. Peer and packing lifecycle checks
passed independently. The experiment was reverted in **6950e001**; the window
regression and Makefile entry were removed with it. Later repeats were NOTRUN.
Build identity: `EFS_GIT_ID=1cb3b203 EFS_GIT_DESCRIBE=1cb3b203
EFS_GIT_BRANCH=devel`. All782 inputs verified; digest
`74772e838760bb139ba5db2bf235e1bb53fe405ca3ed8ab63a659b93c94fd449`.
Raw output: `amd_efs:/data1/efs/packing-qualification-20261012/window-*`.
Same live reproduction, fresh ports36310,36320,36330; concurrency, quota and
deadlines remain unchanged. Run `python3 tests/test_remote_read_window.py` for
historical focused reproduction at1cb3b203; that historical Makefile includes it. The inactive
qualification source checkout was reused; no effort to preserve old binaries,
as authorized by the user. Exact input/binary hashes and raw verdicts remain.


## Reserved ReadIndex connections: timeout regression remains open

Candidate **eb2fad1b76e5311087076f70d2c3544a62825edf** retains32 ordinary
peer connections and reserves two additional slots for ReadIndex requests.
Mutations cannot borrow those slots; release/drop signals the matching waiter
class. Existing RPC deadlines, TCP timeout restoration and failure ownership
remain unchanged. Saturated ordinary-pool regressions demonstrate independent
read admission, but this does not establish pool starvation as the cause of
the live directory timeout. The two-RPC coalescer experiment is absent.

Same AMD395/kernel7.0.0-38/GCC15.2 environment and four private4G nodes, seven
clients/jobs16, files/TCP/direct/packing1/commands0/server0/repair0. Build:
`make -j8 EFS_GIT_ID=eb2fad1b EFS_GIT_DESCRIBE=eb2fad1b EFS_GIT_BRANCH=devel`.
Verified781-input manifest digest:
`fc0db7bdbdd51c3c9a29f40e77e115ad3e16a127aed93df672121a83cea03ec2`.
Executable SHA-256:

| Executable | SHA-256 |
| --- | --- |
| efsd | e11f66e86d9827f9a39056fc9a1109eccac6da558d3aac4f4ccac5c48c762af5 |
| efs-fuse | 801314ce5d123d4eaa2860ce7c043b6152213bdd15254e1c9a75cf13b6bcab32 |
| efs-mgmt | 31d6cdd92df06c44baccf8e1ac9de661bbf61e030b08cd5ab756d218050d6bfd |

Nine focused regressions PASS normally and under ASan/UBSan: ReadIndex pair,
completion, remote pair, host read control, operation, publication, FIFO inbox,
peer-pool deadline and host-read deadline. Run the corresponding
`python3 tests/test_*.py` scripts; sanitizer reproduction uses the compiler
wrapper and options documented above. These extracted tests are not fully
instrumented live daemons. Full normal/fault native reruns PASS.

Quiet fixture `gc-direct-wyam0tcz` (**FAIL**):1492/1512 core PASS,20 directory
timeouts at the original30s deadline: all seven dir_deep_nesting and
names_crazy_dirs cases, plus six dir_deep_nesting_beyond_64 cases. Client6
passed beyond64. No EIO was reported in this run. Peer67/67, acknowledged
cold bytes, physical packed reclamation, metadata queue drain and all four
orderly exits within20s PASS independently. Further quiet repeats and the
current-source crash repeat were NOTRUN after that failure. Suite durations
were71.9–75.4s. Cold admission had6.54GiB safe capacity against4G; this failure
was not the earlier reserve refusal. No deadline, quota or authority was relaxed.

Use the full seven-client reproduction above with fresh ports36410–36413.
Raw logs, binary/input manifests and failed stores remain externally at
`amd_efs:/data1/efs/packing-qualification-20261012/proof-*` and
`/data1/efs/packing-qualification/root-a/gc-direct-wyam0tcz`
(with companion root-b `gc-direct-flkol6tj`). No raw captures are committed.

The reviewed GC reap path already selects active lane bits; it does not issue
all64 sweeps for each tiny file. Logs also show repeated GC passes visiting
thousands of tombstones. That is a diagnostic lead, not a proven timeout cause
or a completed fix. Existing profiles show fresh ReadIndex dominates lookup
wall time. Next acceptance requires a demonstrated scheduling/lookup fix and
repeatable quiet full runs on its exact source, preserving the30s cases. W95
and its production-format/migration obligations remain open.

Full eb2fad1b safety reruns completed with exit0 for `make test` and a clean
`make -j8 EFS_FAULTS=1` / `make EFS_FAULTS=1 test`. Native log SHA-256:
normal `640a0d7ac2f479b845b5f1f0bf68340181d841281f079fda7ea53edc0006eab9`;
fault `4eb3f06baeabcb0c94234dfc65f4c722e6c6a07c66bcea77dc59c2d28cd972e8`.
These safety results do not change the failed quiet packing verdict.
