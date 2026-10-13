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

Current candidate: **59534b9ae9eb47694214fd3ee4740e23758921d5** adds a
queued-byte TCP receive fast path. Local absolute-deadline regression PASS;
exact-source AMD full safety is RUNNING, quiet live repeats NOTRUN until safety
finishes. Previous dc4ad1ed quiet core **1494/1512 PASS,18 timeouts**, peer67/67,
cold acknowledged bytes, physical reclamation and metadata drain PASS. Three
repeat rounds and the additional crash gate were not completed after that
failure. Packing repeatability remains FAIL; no deadline or authority change.

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


## Atomic scalar admission and bounded view publication: qualification pending

**3393b129693b46747261caf360d6e892bdd6d1ea** captures the scalar reader's
current core commit/term and next-round admission, then starts an idle round
under the same pump-mutex acquisition. A pending round still forces the new
arrival to wait for the next round. Failures and round-counter exhaustion remain
failures. The waiter retains quorum/term/applied-prefix guards and its deadline.
The new actual-helper regression checks100 post-send arrivals, fresh commit
floors, next independent rounds, role refusal, storage failure and overflow.
Full normal/fault native and ten focused normal/ASan/UBSan tests PASS on AMD;
quiet live acceptance FAILS:1492/1512 core PASS,20 timeouts in
`gc-direct-70f5973i`: seven deep60, seven names_crazy_dirs, three deep100 and
three names_crazy_roundtrip. Peer67/67, cold bytes, physical reclamation,
metadata drain and four orderly exits PASS independently. Further quiet and
crash repeats were NOTRUN after this failure. Normal/fault log SHA-256:
`981940ef882ec857cd4e53701db3e01a8c09478dee143fb6a81ef1f210a60b19` and
`f3f143113601df57573810566e0e22ee264fa18a2d799fe79506448465170e01`.
Manifest782-input digest:
`6ff41a2e317a7f9c8636018230986d9d7642abdcc8cc4607487ddb9bd0324ca5`.

**61ba3c1695ef8ee5f96be8522825248622c2db42** additionally collects Raft
state under the existing pump mutex before marking the published view sequence
odd. Quorum getters take the shared disk-log mutex, so the former odd window
could include a blocking getter and make lock-free readers spin. The new odd
window covers only the atomic field copy; identical published state makes no
sequence/stamp writes. Readers see the preceding coherent snapshot while state
is collected; it cannot cover arrivals after that proof's send. Fresh admission
and the existing quorum/term/apply guards remain the authority requirements.

The actual publication regression holds a quorum getter blocked in another
thread, requires an even sequence and a readable old coherent view, rejects
post-send admission, then verifies the new term/round after release. Both this
regression and the unchanged-publication check fail against3393b129 and PASS
with61ba3c16. Local admission/pair/completion/publication regressions PASS.
AMD full normal/fault native and ten targeted normal/ASan/UBSan regressions
PASS. Quiet fixture `gc-direct-x8u82cbf` FAILS:1492/1512 core PASS,20 timeouts
(seven deep60, seven names_crazy_dirs, five deep100, one names_crazy_roundtrip).
Peer67/67 and all four status-zero exits within20s PASS. Cold restart was
NOTRUN:4G exceeded the3.88GiB safe maximum. Thus this run verifies neither
cold bytes nor post-restart reclamation/drain. Reserve and quota were unchanged.
Further quiet/crash repeats NOTRUN. Manifest782-input digest:
`a032a9e53391a999f9d1851d9d7e400b38f203cc37c2c7c3b9a6a1370c9a523f`.

Build each candidate with its short SHA in `EFS_GIT_ID`/`EFS_GIT_DESCRIBE`
and `EFS_GIT_BRANCH=devel`, then run full `make test`, clean fault-enabled
build/test and the focused admission/publication plus existing read/FIFO/pool
regressions normally and with the sanitizer wrapper above. Live reproduction
is the same seven-client/jobs16/4G fixture with fresh ports36610 for3393b129
and36710 for61ba3c16. Three quiet rounds and the acknowledged-fsync crash repeat
are required, not assumed. Every client/server command feature remains off;
files/TCP/direct/packing remains selected, and30/20/180s budgets are unchanged.
External raw locations: `amd_efs:/data1/efs/packing-qualification-20261012/admit-*`
and `view-*`; failed stores are retained. No raw outputs are committed.

An instrumented eb2fad1b diagnostic (`gc-direct-mnesv935`) separately failed
1498/1512 core: all seven deep60 and names_crazy_dirs cases timed out. Peer,
cold bytes, reclamation, metadata drain and orderly shutdown PASS. Node1's
cumulative leader ReadIndex mean was3257us; node2's remote proof mean5946us
and post-proof apply wait1472us. These whole-run counters are diagnostic, not
exclusive deep-case timing or evidence that either new change closes the gate.
Instrumentation included phase, read, RPC and Raft observations; it cannot be
counted as quiet acceptance or compared as an unconfounded throughput result.


## Fresh quorum scheduling without waiting for preceding apply

Candidate **dc4ad1ed424986b01e070040ca5a1a8f89c3f65f** uses the existing Raft
core support for retaining a completed quorum while its own apply target is
pending. Host scalar and paired admissions can start the next fresh round once
that prior quorum is complete; a current unproven round still cannot be
superseded. A reader whose own proof is complete waits for that proof's own
apply, rather than starting repeated replacement rounds. The sleep predicate
lets a later admission start its fresh proof after quorum completion. No rows
are returned until the unchanged host_view_covers term/quorum/round/index/apply
predicate succeeds. No added remote RPC window, threads, cached authority,
protocol change, deadline or concurrency relaxation.

The actual scalar/pair regressions cover pending-unproven refusal and a new
round after quorum with own apply still pending. Existing actual-core parked
apply/floor/early-ACK tests exercise retained proofs and rejection of unapplied
read authority. Local admission, pair, completed-proof and coherent-publication
regressions PASS. Exact-source AMD full normal/fault native and ten targeted normal/ASan/UBSan checks PASS.
Fully instrumented actual Raft tests PASS normally and with early read ACKs. Live acceptance is NOTRUN pending capacity for unchanged4G fixtures.
Manifest782-input digest:
`e6be8db84569c3727919d49dbc863dc5f005d6336b59b112a46decb74665fee9`.
Build with `EFS_GIT_ID=dc4ad1ed EFS_GIT_DESCRIBE=dc4ad1ed EFS_GIT_BRANCH=devel`.

Run `make test`, clean fault-enabled build/test, the ten focused read/FIFO/pool
Python scripts with the sanitizer wrapper, and a fully instrumented libefs.a +
tests/test_raft build. Run actual Raft once normally and once with
`EFS_TEST_EARLY_READ_ACK=1`, retaining ASan/UBSan abort/leak options above. The
live command remains the same seven-client/jobs16 fixture, fresh port36810,
4G per node and30/20/180s budgets. Raw output lives externally under
`amd_efs:/data1/efs/packing-qualification-20261012/quorum-*`.

The user subsequently authorized deleting any AMD EFS cluster test data as
needed. Cleanup removed 97 stopped synthetic node stores identified by fixture
manifests, retaining parent logs and manifests; available capacity increased
from188GiB to220GiB. Unrelated services were untouched. The earlier proposed
archive transfer was rejected before execution, and no archive was exported.
No quota or storage reserve policy was changed.

Current dc4ad1ed safety checkpoint: both full native suites exit0, targeted
read/FIFO/pool normal and sanitizer gates exit0, and fully instrumented actual
Raft normal/early-ACK checks exit0. Original-budget live qualification is RUNNING
in detached screen `packing-quorum-live`: three quiet seven-client rounds,
followed by the focused acknowledged-fsync crash gate if all rounds pass.
Normal binary hashes below match the running candidate. No live pass is inferred
from safety checks or space cleanup. Raw progress is in `quorum-quiet-1.log`,
subsequent round logs, and `quorum-live.exit`/`quorum-live.finished` markers.

Normal executable SHA-256:

| Executable | SHA-256 |
| --- | --- |
| efsd | 62b61827e438afc44cfabfe49fc75783a596214c742ce8c121c78eb88973fee9 |
| efs-fuse | fa0b03098d9fbb6b90884e998d4497eda174037598636dce10be1b113b83a1c4 |
| efs-mgmt | efe8893cf94e43155c6f575da6b9b104240ed0ae79d2c6a8c8d798f0d2158784 |

Full native log SHA-256: normal
`4f84e7b5803abe3f5dd724a5f64193498c749b2afec5294720bad0db25aed6b6`;
fault `70d3b9fcb170f1b47e174e386c77ebd22e5d0b3bf496afd9ed0f41d39eb2e5b7`.
To reproduce the fully instrumented actual core, clean a separate source copy,
build `libefs.a tests/test_raft` with `CC` pointing to the gcc sanitizer wrapper
(which appends `-fsanitize=address,undefined -fno-omit-frame-pointer -g` at both
compile and link), then run `./tests/test_raft` normally and with
`EFS_TEST_EARLY_READ_ACK=1`; set `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`
and `UBSAN_OPTIONS=halt_on_error=1` for both. Compile/link command logs are
retained in `quorum-core-san-compile.log`; executable output in
`quorum-core-san-{normal,early}.log`. Safety PASS is not packing acceptance.

Fully instrumented actual Raft output SHA-256 (both normal and early-ACK
outputs, independently executed):
`6d5a0e475c33ea6abcdf753bbbfa2d556d4b0998d269589838fb7cd8d8e6f40d`.
Preceding61ba3c16 quiet executable identities (`view-binaries.txt`):
efsd `a292cab995ac9ead66efc212ca328ffa87e7eda5a4c863e10fa1d25938a2d4a2`;
efs-fuse `4862cf5fdae672dd143458a00e33c317a0c2f759d032fda9c92e61abd1ad06a1`;
efs-mgmt `3405d151081f55ea5881d2ff1fb0f9adddb5a7aa7ba93f35571c110a814c2ac1`.


### Syscall diagnosis and queued-byte receive candidate

At the user's request, dc4ad1ed was traced during the unchanged seven-client,
16-jobs/core workload on AMD (four4G daemons, TCP/direct, packing enabled,
`.efs` disabled at client and server, repair experiment disabled). `strace -ff
-ttt -T` covered all four daemons and all seven FUSE processes from startup
through80s after the seventh mount, including the30s directory timeout window.
The traced core passed1438/1512 with74 timeouts: instrumentation introduced
additional failures, so this is diagnostic evidence, never qualification.
Peer, cold acknowledged-byte checks, reclamation and drain still passed.
Raw captures remain outside Git at
`amd_efs:/data1/efs/packing-qualification-20261012/quorum-strace-*`.

FUSE clients issued roughly42–45k poll calls and73–77k recvfrom calls each.
Daemon2 recorded about2.77million futex calls, including repeated condition
wakeups/mutex contention. These are aggregate thread counts, not elapsed wall
latency; long idle receive/condition waits must not be called request stalls.
Traced individual fsync/fdatasync maxima were below10ms. A separate49Hz dwarf
stack sample (`quorum-stack-*`) found ext4 journal sync spin contention plus
TCP send/receive and condition-lock work. Daemon2 do_fsync accounted for13.85%
inclusive CPU samples; product callers included KV hold release and Raft log
batch barriers. No durability barrier was removed based on those observations.

One confirmed avoidable cost is `efs_recv_all` polling before every bounded
receive even when the remaining frame bytes are already queued. Candidate
59534b9a first tries MSG_DONTWAIT after checking the same absolute deadline,
then polls only on EAGAIN with the remaining budget recomputed. It retains
partial-read handling, EINTR retry, EOF refusal, expired-before-read refusal and
shared fd flags; framing and all read/quorum authority requirements are unchanged.
The actual-function regression additionally proves three queued single-byte
reads issue zero polls. Idle and slow-drip deadline regressions remain PASS.
This is a syscall optimization; timeout closure is not inferred from it.

Exact candidate input manifest:772 tracked source/test/build files, aggregate
`bce8c5e88b9d655467ca54d0c8e3b218cfb7f1cf5d75fc12e92e48069c16cb49`,
external `efs-recv-inputs.json`; build IDs are59534b9a/devel. The normal/fault
native, ten focused sanitizer checks and three quiet repeats plus focused crash
gate are sequential in detached screen `packing-recv`. Raw logs/markers use
`recv-*` in the same external directory. No pushes or release promotion.

Reproduce syscall or stack diagnostics from the tested source with strace/perf
installed and same-user ptrace/perf permission. Use an empty output directory
outside Git, separate from the test roots:

```sh
python3 tests/live/packing_diagnostic.py --capture strace \
  --output /data1/efs/packing-diagnostic-new \
  --root /data1/efs/packing-qualification/root-a \
  --root /data1/efs/packing-qualification/root-b --port 36910
```

Use `--capture perf` and fresh output/ports for the separate49Hz stack capture.
The committed helper preserves4G and30/75/180s case/drain budgets, traces only
the selected source binaries/private four-port range, detaches captures after
80s of concurrent work, and lets the fixture finish cold/reclamation cleanup.
Inspect attach stderr for capture failures. For acceptance, run the documented
quiet gc_reclamation command without this helper or observation flags. Test
stores may be erased after processes stop under the user's AMD data waiver;
retain diagnostic summaries/reproduction and external logs as useful.
