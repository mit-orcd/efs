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

**Qualified source: 8151debe7810b5f5814dd18513608fefd8073bbd — PASS on AMD.**
Full normal/fault native, nineteen focused normal/ASan/UBSan and fully
instrumented actual Raft normal/early-ACK checks PASS. Three consecutive quiet
seven-client/jobs16 rounds each PASS1512/1512 core and67/67 peer, with no
skip/notrun in the selected core. Every deep60, deep100 and unusual-name case
passes the original30s budget. Each round also passes orderly shutdown, cold
acknowledged bytes, physical packed reclamation and metadata queue drain.
The focused all-four-daemon SIGKILL-after-acknowledged-fsync/cold gate PASS.

This closes the observed AMD deep-directory/unusual-name timeout acceptance
gate for this source and workload. W95 remains partial for production-format,
migration and matched packing-on/off performance decisions. Fresh quorum,
configuration/term/role/storage invalidation and serving-owner apply fences
remain; no quota, timeout, lease, wire format or release/tag/push change.

Historical baseline source: **0dcce384fa442479070a31464777f104c1fe7829**.
Memtable insertion optimization and GC shutdown correction pass normal/fault
native, fully instrumented LSM/model/GC safety gates, focused orderly packing
and acknowledged-fsync crash/cold/reclamation gates on AMD. The isolated insertion
benchmark improves 13.8x; this is not an end-to-end throughput claim.

At the historical0dcce384 checkpoint, full seven-client packing was **FAIL**: clients 2 and 6 exceed the unchanged
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

The original-budget AMD safety, quiet repeatability and focused cold/crash
acceptance above are PASS on the named source. Remaining W95 work is matched
packing-on/off throughput, durable-close latency, allocation/CPU comparison,
and the P4.1 production-format/migration decision. Single-host functional
acceptance does not establish multi-host/network/hardware failure behavior or
a production packaging rollout. The release branch/tag is unchanged.

## Next work toward W95 closure — Oct 13

**Plan only: NOTRUN.** The user requested this plan be documented and committed;
that does not approve a production format choice, default activation, migration,
release promotion or push. Continue solo on `devel`, exclusive AMD, ordinary
files/TCP/direct with `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0` and repair disabled.
Containers, RDMA and `.efs` are outside this scoped closure. Existing hardware
or release obligations remain separate work; older ORCD-wide checklists do not
add them to this AMD/TCP task.

The recommended order is matched A/B, coverage audit and only uncovered
lifecycle tests, then P4.1 decision and closure reconciliation. Do not repeat
already-passed native/POSIX/peer/cold gates unless source/configuration changes
or a new failure requires an affected rerun.

| Order | Remaining work | Completion evidence |
|---|---|---|
| 1 | Matched packing off/on on one frozen source | Three alternating pairs, same durable workload and conditions; sustained files/s, durable-close p50/p95/p99/max, CPU, allocated fragment bytes/count and metadata/WAL writes; all errors retained |
| 2 | Shared-object lifecycle coverage audit | Map current native/fault/live tests and exact-source evidence to the cases below; distinguish PASS, missing evidence and missing implementation; run only actual gaps |
| 3 | P4.1 format/rollout decision | Explicit eligibility/geometry, capability negotiation, mixed packed/ordinary behavior, activation policy and fresh-store/upgrade policy; recommendation remains unapproved |
| 4 | Reconcile W95 and its index | One current acceptance matrix links measured benefits and lifecycle evidence, records limitations and the approved decision, and removes superseded implementation blockers |

### First: matched performance and allocation

Use the same product/test inputs as qualified8151debe, or freeze and identify a
new source before both arms. A uses `EFS_PACKED_WRITES=0`; B uses1. Verify actual
client/server flags and binary hashes. Both arms retain the same files engine,
TCP/direct I/O, four4G quotas, placement/configuration,16 workload workers,
logical1–512-byte payload distribution, seed and directory geometry. Use fresh
synthetic sibling namespaces/stores; never compare an old binary or a warm
retained namespace against a fresh candidate.

Reuse the existing tiny-file workload and committed helpers
`tests/live/tiny_sustained.py`, `tiny_churn.py` and `tiny_mixed.py` as applicable;
inspect their supported modes before selecting the measurement runner. Reuse
`~/git/cluster/testing/tiny_files.py` rather than create a different workload
for each arm. Freeze the generator revision, commands, file count, seed,
durations and supported persistence semantics in the run manifest before
measurement. If existing helpers cannot measure durable-close latency,
allocation or metadata/WAL writes, add the minimal common measurement support
first and use it identically in both arms.

A setup pilot should select a capacity-safe workload large enough to observe
the sustained slow phase, not only startup. This pilot is not an accepted
sample. For the fixed-file-count workload, both arms must perform the same
operation sequence and complete the same count; record one-minute rates through
the slow phase. A separate bounded churn measurement may use equal fixed
windows, with identical logical occupancy and operation mix. Never stop the
slower arm early and present unequal work as a matched comparison.

Run three pairs in alternating order: A/B, B/A, A/B. First compare pure tiny
create/write/durable-close and deletion; then compare mixed large-writer/tiny
close load under W53, again with three alternating pairs and an explicitly
frozen large-writer load. Quiet timing excludes simultaneous builds, profilers
and other owned fixtures. Low-rate CPU/host I/O counters may be collected
identically in both arms; intrusive profiling follows a demonstrated problem
in a separately labelled diagnostic run.

Report each repeat and its variation, not just a best run or average. Include
observed cohort occupancy and singleton costs, fragment headers/padding/tails,
metadata/index/WAL overhead, peak client memory, physical garbage/reclamation,
and foreground close tails. Count application bytes separately from allocated
bytes and host writes; host-write counters do not measure NAND endurance.
Require exact acknowledged payload verification, including peer/cold checks,
and no EIO/EBUSY/timeouts or concealed refresh errors. A performance target is
not permission to weaken durability, freshness, reserve or deadlines. Decide
from measured benefit and tail regressions; do not presume a2× speedup or that
packing benefits every workload.

### Then: audit lifecycle evidence, test only uncovered cases

The current normal/fault suites and live cold/reclamation passes already cover
many ownership and shared-object behaviors. Audit these specific obligations
against existing tests before describing anything as unimplemented:

- Abandoned PENDING cancellation requires authoritative session revocation and
  rejection floors; elapsed time or transport loss must not substitute for proof.
- Lost PUT/publication/retirement replies preserve immutable identities and
  successful ACK bits, including coordinator/leader changes and cold replay.
- Shared-object GC across coordinator failure at fence/delete/absence-ACK
  boundaries preserves live neighbors and newer generations; missing members
  never count as durable ACKs.
- Quota/ENOSPC and aggregate client memory pressure refuse safely; partial
  publication and failed reclamation retain the ownership needed for retry.
- Mixed ordinary/packed reads, oversized-file promotion and unsupported peer
  capabilities retain correct fallback/refusal before ambiguous publication.

Record test names, exact source, passed assertions and coverage gaps in a small
matrix here. An absent live scenario is not automatically missing code; a unit
pass is not automatically evidence for a distributed crash scenario. Add
focused regressions/fault cases only for uncovered requirements. Fix actual
failures and rerun affected gates on the merged source, preserving original
budgets and raw failed evidence externally.

### Decision recommendation and closure boundary

Recommend **opt-in packing on freshly formatted stores for this pre-alpha**,
with ordinary files available and packing off by default. P4.1 should approve
eligible payloads/cohort geometry, immutable mapping/ledger versions, capability
negotiation and mixed-format behavior. Explicitly decide whether upgrades to
retained older stores are unsupported for this stage or need a migration plan.
Do not implement migration merely to finish an experiment if fresh-store-only
is approved; equally, do not silently treat retained stores as compatible.
This recommendation is not an approved format or rollout decision.

W95 may close when matched benefit/limitations are documented, all required
in-scope lifecycle evidence is accounted for or gaps fixed, and the P4.1
acceptance/rollout boundary is explicitly decided. If production rollout or
migration is deferred, give it a separately indexed follow-up with clear scope;
do not close W95 by leaving its existing obligations implicit. AMD synthetic
stores may be erased under the user's authorization; that grants no permission
to wipe unrelated datasets or other clusters. Do not push or activate packing
on a release/default deployment as part of this documentation task.

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


### Quiet receive rerun and small-frame follow-up

59534b9a full normal/fault native suites, ten focused normal/sanitizer checks
and the changed network deadline regression with ASan/UBSan PASS. Quiet round1
is FAIL:1489/1512 core pass,23 unchanged30s timeouts (all seven deep60,
unusual-name directories and deep100 cases, plus unusual-name roundtrip on
clients4/5). Peer67/67, all four orderly exits0 within20s, cold acknowledged
hashes, physical fragments and metadata drain PASS. Further repeat/crash gates
NOTRUN after failure. Raw fixture `root-a/gc-direct-sl85cr4t`, `recv-quiet-1.log`
and `recv-run.exit=1`; the optimization has not demonstrated end-to-end gain.

The same traces showed separate socket receives for frame length, one-byte
type and body. f0dd1a2d retains length validation before reading any type/body,
then co-receives type+payload for frames at most2048bytes, copying at most2047
payload bytes from a bounded stack buffer. Large data frames retain the existing
malloc-base direct receive with no added bulk copy. No wire changes, extra
receive buffering across messages, timeout changes or authority caching.
The actual receiver regression checks2 receive helper calls for small frames
versus3 for large frames, exact boundary2048/2049, empty/discarded bodies,
fragmented delivery, header-only invalid refusal and truncated peer. Local PASS.

Candidate f0dd1a2d exact input manifest774 files, aggregate
`9123f427256fe82eb7e5988ab5b66d5e8353b327625f22534e7dbb2fc8865d26`;
external `efs-frame-inputs.json`, source `frame-src`, build IDs f0dd1a2d/devel.
Screen `packing-frame` sequences full normal/fault native, twelve targeted
normal/sanitizer checks (including both network regressions), then three quiet
rounds and focused acknowledged-fsync crash if all quiet rounds pass. Original
4G and30/75/20/180s budgets and all disabled features are retained. Results are
pending; no throughput or timeout-closure claim is made.


### Small-frame quiet result and queued read-vote scheduling

f0dd1a2d full native normal/fault and twelve focused normal/ASan/UBSan checks
PASS. Quiet core1498/1512 PASS,14 unchanged30s timeouts: deep60 and
unusual-name directories on all seven clients. Deep100 and unusual-name
roundtrip pass in this round, but one round is no repeatability proof. Peer67/67,
all four orderly exits0, cold hashes, physical reclamation and metadata drain
PASS. Further repeated/crash rounds NOTRUN after failure. Raw fixture
`root-a/gc-direct-yr4hvp__`, external `frame-quiet-1.log` and `frame-run.exit=1`.
Normal binary hashes: efsd
`2ae5ad52600dac6783456f6c4c95eaf9aa046ed3067494596278a97e9ba2ce2d`,
efs-fuse `fd106455f641ed75c72617feafb51152b315ce66b6c064f0756026e0712c9bd4`,
efs-mgmt `3042ddfdba5ef69ef397f0e13c1e2fd707036c77fe523c012a3e574614facd64`.
These rounds compare the same quotas, hardware, concurrency and deadlines;
18→23→14 timeouts are observations, not a statistically demonstrated gain.

Code review of the traced quorum waiting path found a scheduling gap: the
handler's existing early-control path uses trylock; a busy pump sends that
fresh frame to ordinary FIFO slices. 4f54bd9a adds a bounded scan of at most256
already-admitted frames under the pump's existing core mutex, noting only
nonzero-context, header-only AE request/reply votes through the unchanged
`efs_raft_read_control`. Successful notes are marked once. Frames are never
removed, reordered or credited early; append/commit/apply stays on normal recv.
Core storage-failure, boot, term, membership, persisted-prefix, current-round
and own-apply requirements remain authoritative. A declined vote remains
eligible for a later cycle. The snapshot releases inbox_mu before core send,
and only the pump removes queue entries while holding h->mu; concurrent
arrivals append after the stable snapshot indices.

Focused actual-helper regression PASS for once-only notes, full256-frame bound,
malformed/non-read/zero-context filtering, core refusal, late arrival and
unchanged FIFO pointers/bytes/work credits. Existing FIFO regression now also
checks the note flag survives slicing. Host flow and inbox normal checks PASS.
Candidate input775 files, aggregate
`e3832068a17807886444e47c7757ff8468edcd7bf588550ae044d240624373ed`;
external `efs-votes-inputs.json`, source `votes-src`, build IDs4f54bd9a/devel.
Screen `packing-votes` runs full native normal/fault, thirteen targeted normal
and sanitizer checks, then the same three quiet rounds/focused crash sequence.
Raw output `votes-*` remains external. Live outcome pending, W95 remains open.


### Queued read-vote experiment rejected

4f54bd9a full normal/fault native and thirteen focused normal/ASan/UBSan checks
PASS, but quiet core1495/1512 PASS,17 timeouts: seven deep60, seven unusual-name
directories, deep100 on clients3/4 and unusual-name roundtrip on client3.
Peer67/67, orderly all-four exits0 and cold hashes PASS; physical/drain still
running at this checkpoint. The result provides no demonstrated end-to-end
benefit over retained small-frame code. **5568306c** reverts the pump scan,
per-frame note field and extra scheduling regression, retaining FIFO semantics
and the two small TCP receive improvements. `git diff --exit-code f0dd1a2d HEAD
-- src include tests scripts Makefile` passes after revert: product inputs are
identical to the previously safety-tested source. The experimental source and
failure evidence remain external in `votes-*`/`gc-direct-bjrqk4yg` for diagnosis;
no experimental scheduling machinery is retained.

A sequential quiet retained-code repeat (`frame-src`, same f0dd1a2d binaries,
port37310) waits for `votes-run.exit` before starting; external output
`frame-quiet-recheck.log`, markers `frame-recheck.exit`/`.finished`. No concurrent
profiling/build is performed. Its outcome is pending; no timeout is increased.


### Final diagnostic checkpoint — retained TCP code, not qualified

Retained5568306c product/build/test inputs match f0dd1a2d exactly. Full native
normal/fault and twelve focused normal/ASan/UBSan checks PASS. Two quiet rounds
use that same source and identified binaries, with `.efs` disabled, TCP/direct,
packing enabled, four4G nodes and seven clients×16 jobs. No tracer/profiler/build
runs concurrently with either round. The original30/75s cases,20s shutdown and
180s reclamation/drain budgets are unchanged.

| Exact code / run | Core PASS | Core timeout FAIL | Peer / shutdown / cold bytes / physical GC / metadata drain |
| --- | ---: | ---: | --- |
| dc4ad1ed baseline |1494/1512|18|PASS |
|59534b9a queued receive |1489/1512|23|PASS |
|f0dd1a2d small-frame, round1 |1498/1512|14|PASS |
|4f54bd9a queued-vote experiment, reverted |1495/1512|17|PASS |
|f0dd1a2d retained-code quiet repeat |1502/1512|10|PASS |

The repeat fails deep60 on every client and unusual-name directories on
clients1/4/5. No core test is skipped/notrun inside either216-case client core.
Peer67/67, four orderly zero-status daemon exits, acknowledged cold hashes,
physical fragment reclamation and metadata queue drain all PASS in both
retained rounds. The experimental votes run also completed physical/drain PASS.
The three-consecutive-pass acceptance and additional focused fsync crash gate
remain NOTRUN after failures; **W95 stays open**. Counts vary across runs, so
these data do not establish a statistical speedup. The receive changes retain
concrete queued/framing syscall-count regressions, while unproven additional
pump scheduling is removed to keep the runtime lean.

The lower-overhead diagnostic's last observed deep60 phase is `create` on all
seven clients; no leaf-write or recursive-cleanup phase was recorded. Collector
errors are empty, but missing datagrams remain unknown. The next unresolved
investigation is request-correlated fresh metadata/quorum and mutation latency
while constructing that path, plus unusual-name directory create/read/remove
phase timing. Aggregate idle futex/receive sums are not a causal explanation,
and the traces do not justify weakening freshness or durability.

Quiet repeat raw fixture `root-a/gc-direct-4wyz2rkg`; external
`frame-quiet-recheck.log`, `frame-recheck.exit=1` and `.finished`. All owned
screens, captures and private fixture processes finished; unrelated ordinary
AMD services were untouched. Raw artifacts stay outside Git. SHA-256:

- First retained quiet log: `a3d48ccbaebf2bdd793a4dc96ba3a4fbfbe065ef411bf64dc05defe11688085d`.
- Retained quiet repeat: `fad74a58bf347b694cf5c29840e9ee3bdee52bb406563a41f512dadd02529c97`.
- Full normal native: `953e1b35bbb60b41850de307710f0b3151a946a77f6d0ae6ebc1898af03b6c34`.
- Full fault native: `15df864c775f0815dc9d15c23119c8fe3df9388cb273943841bb1c9bceb41678`.

Reproduce retained quiet tests with the earlier common gc_reclamation command,
fresh ports (e.g.37110 then37310), exact f0dd1a2d build identity and no diagnostic
observation flags. Run targeted regressions with `python3
tests/test_net_receive_deadline.py` and `python3 tests/test_small_frame_receive.py`;
use the documented sanitizer compiler wrapper for ASan/UBSan. Erase only stopped
EFS fixture stores under the authorized AMD waiver if capacity requires it;
preserve the useful summaries/logs externally. No push or release promotion.


### Fresh metadata waits — exact remote proof floor

A new low-overhead diagnostic on f0dd1a2d retained the exact four4G, seven×16
core, TCP/direct/packing, commands-disabled workload and original deadlines;
only EFS_READ_PROF/EFS_RPC_PROF/EFS_RAFT_OBS/EFS_POSIX_PHASE_TRACE were enabled.
Raw `fresh-read-diag.*` and `root-a/gc-direct-d1m6voav` are external. Core1497/1512
PASS,15 timeouts; this instrumented result is diagnostic. Last observed deep60
phase is create on all seven clients (missing datagrams remain unknown).
Final cumulative read-prof averages: node1 parent fence4308us versus KV26us;
node2 parent fence7216us versus KV26us, remote proof6352us and local post-proof
apply1690us. During the busy window those means were5278,8354,8327 and2209us,
respectively. These are stage aggregates across the workload, not per-case
causal percentages. Fresh proof and catch-up dominate row lookup CPU.

Inspection found the zero-command ReadIndex RPC called host_read_index, then
sampled a later `v.applied` for its reply. That later index can include unrelated
concurrent commits beyond the proof that already covered this admission.
The requester must then apply that unnecessary suffix before reading its KV.
fc76dd52 adds an optional exact proof output to the actual admitted reader:
leader completion returns its coherent proven_index; follower completion
returns the remote index only after its own applied barrier succeeds. Errors
clear the output. Ordinary local/paired reads retain their existing predicates.
Only the zero-command remote proof reply uses the output rather than sampling
later live state. No fresh round, quorum, own-term commitment, own apply,
transaction, routing or failure fence is relaxed; the wire format is unchanged.

The focused actual-reader regression uses proof110 with later applied900 and
commit1000 and requires returned110. It also refuses older rounds, missing
quorum, insufficient own apply, term change, remote failure and requesting-host
apply failure; actual scalar admission and paired regressions PASS. This is
exact target selection, not authority caching. Native normal/fault and thirteen
focused normal/ASan/UBSan checks on exactfc76dd52 PASS. Exact input775 files,
aggregate `21e753204d36e235316902fa2f092be335670150ff35cdd1d7dcf9cc289cb705`,
external `efs-floor-inputs.json`, source `floor-src`, build IDsfc76dd52/devel.
The frozen live suite does not include the separately committed7c56c70b opt-in
unusual-name phase extension (local observer/unchanged-workload regression PASS).

First quiet core1512/1512 PASS and peer67/67 PASS, orderly exits0 within20s,
cold acknowledged hashes, physical GC and metadata drain PASS. Raw first fixture
`root-a/gc-direct-n45qfmd1`, output `floor-quiet-1.log`. Sequential repeat2/3 and
focused fsync crash are pending under screen `packing-floor`. No simultaneous
profiler/build is active in quiet runs. Do not infer repeatability from round1.
Reproduce via the existing quiet command with this exact source/build, fresh
ports37510/37520/37530 and focused37550. Original4G and30/75/20/180s budgets,
TCP/direct, packing enabled and `.efs` disabled at client/server are unchanged.


### Exact-index repeat failure and quorum-only proof endpoint

fc76dd52 full normal/fault and thirteen focused normal/sanitizer checks PASS.
Quiet rounds1/2 core1512/1512 and peer67/67 PASS, with orderly shutdown, cold
hashes, physical GC and metadata drain PASS. Client suite wall ranges were
44.5–48.9s and47.2–64.2s, versus retained f0dd1a2d repeat76.5–79.0s; these are
suite totals, not individual directory times. Round3 **1467/1512 PASS,45
unchanged-budget timeouts**, including deep/unusual-name cases and four75s
many-file cases. Peer67/67, shutdown/cold/reclamation/drain PASS independently.
Focused crash NOTRUN after failed round3. Source/budgets did not change between
rounds; two successes do not close repeatability. Raw failed fixture
`root-a/gc-direct-isvekdt3` (n3/n4 companion `root-b/gc-direct-l1bseml3`),
`floor-quiet-3.log`, `floor-run.exit=1`. Existing logs contain apply waits in the
20–70ms range during the later load. A post-core iostat snapshot showed NVMe
93.1% utilization, about10.5k writes/s and4930 flushes/s; that snapshot is a
storage-pressure lead, not a measurement of the entire failed case window.
No thermal throttling conclusion is made from raw temperature readings.

The remaining RPC path waited for leader KV apply before returning a proof,
then the data-serving requester waited for its own KV apply. 6b0324e7 introduces
an explicit proof-only mode used only by the zero-command leader index RPC.
It returns a request-fresh, matching-term quorum certificate with its exact
index; it never reads or returns KV rows. The core already validates current
term commitment, correlated round, membership/configuration, boot/persisted
prefix and storage health. A proof endpoint that becomes a follower refuses
NOT_PRIMARY rather than forwarding into a chain. Ordinary local and paired
metadata reads keep the existing own-apply predicate; remote requesters still
call host_wait_applied before reading their data. This removes duplicate
apply waiting at the certificate issuer, not the data-owner apply requirement.
No clock lease, cached authority, wire change or timeout increase.

Actual admitted-reader regression PASS: a quorum-only certificate may return
110 while leader apply is109, but the same local data read remains BUSY.
Missing/older proofs, insufficient floor, term change and proof-endpoint
stepdown refuse; remote requester apply failure also refuses. Current raw
quorum can satisfy a fresh floor while an older retained applied proof cannot.
Paired-read regression asserts its calls stay in normal data-reading mode.
Actual Raft test_read_quorum_before_apply separately verifies fresh quorum
with parked apply never satisfies read_ready/read_covers, and old replies or
stepdown cannot grant proof. Fully instrumented actual library normal and
EFS_TEST_EARLY_READ_ACK=1 runs PASS on this source, plus full normal native PASS.
Fault and thirteen focused normal/ASan/UBSan gates are still pending at this
checkpoint, followed by the unchanged three quiet rounds and fsync crash.

Exact source6b0324e7 input775 files, aggregate
`e377c1917b4d71bafff14987d9e5993d19e9fd5568b72ac9b510e5cb69c11613`,
external `efs-certificate-inputs.json`, source `certificate-src`, build IDs
6b0324e7/devel. It includes the opt-in unusual-name phase markers; the AST
regression verifies the original case I/O operations are unchanged, with default
quiet operation and nonblocking observer failure isolation. Raw output uses
`certificate-*`, screen `packing-certificate`; actual-core sanitizer outputs
use `certificate-core-san-*` and finished with exit0 before quiet measurement.
Reproduce with the common quiet command using fresh37610/37620/37630 ports and
focused37650; diagnostics remain separate and explicitly non-qualification.


### Fresh-read completion and waiter corrections

These commits address distinct waits, without granting cached read authority:

| Commit | Correction | Exact-source verdict |
|---|---|---|
| 6b0324e7 | Quorum-only issuer returns an exact index without its own KV catch-up; data owner still applies it | Full normal/fault native, thirteen focused normal/sanitizer and actual-core normal/early-ACK sanitizer PASS; quiet1497/1512 FAIL (15 timeouts); peer/shutdown/cold/reclamation/drain PASS |
| e47262cb | Typed actionable read wakeups, one notification per registered sleeper, coherent group snapshot | Live measurement interrupted and excluded: replacement build briefly overlapped; retained diagnostic, not qualification |
| 75592aad | Same proof-only readiness predicate at wake and under CV registration, closing a missed-ready race | Full normal/fault native, fourteen focused normal/sanitizer and actual-core normal/early-ACK sanitizer PASS; quiet1496/1512 FAIL (16 timeouts); peer/shutdown/cold/reclamation/drain PASS |
| 570f49ea | Freeze each coalesced caller's first sufficient completion rather than chase later callers' index | Build PASS; normal native stopped on two test-harness GCC misleading-indentation errors; fault/live NOTRUN |
| a262e71b | Brace those test wait loops, retaining strict GCC warnings | Full normal/fault native and fifteen focused normal/sanitizer PASS; quiet1498/1512 FAIL (14 timeouts); peer/shutdown/cold/reclamation/drain PASS |

The coalescer test runs actual extracted functions with real threads: first
proof100, next110, later900. A deliberately delayed caller must retain110 after
the later caller completes900. Exact75592aad code fails that regression; fixed
code passes. Frozen errors, timeout unlink and counter exhaustion are covered.
Reproduce with `python3 tests/test_readindex_coalescer.py`; the legacy comparison
accepts `EFS_RAFT_HOST_SOURCE` and `EFS_TEST_LEGACY_COALESCER=1`. Read waiter
regression: `python3 tests/test_read_waiters.py`. The actual admitted-reader test
also injects readiness between the outer check and CV registration; the old
registration predicate fails, corrected code passes.

Raw external prefixes are certificate, wake, ready, first and delivery under
`amd_efs:/data1/efs/packing-qualification-20261012`. Completed quiet fixtures:
certificate `root-a/gc-direct-7mayo4o2`, ready `root-a/gc-direct-86c06hqo`,
delivery `root-a/gc-direct-dgb0dlyo`. Wake exit130 and its measurement note retain
the excluded overlap; subsequent runners wait for predecessor cleanup before
building. Additional repeats and crash tests were NOTRUN after quiet failure.
The user authorized deletion of stopped synthetic stores; logs, manifests and
summaries remain externally. Ordinary AMD services were not modified.

Exact a262e71b input787 files (including tracked vendor dependencies), aggregate
`33e4998a425afd1d11f0970e801f8087b7273a50a0ebfa70ce4a0b28bd225d5e`,
external `efs-delivery-inputs.json`, source `delivery-src`, build IDs a262e71b/devel.
Binary SHA-256 efsd `70acf6c0d93b42d8ce5e0081665746f077c91c33ea63791cf8fc909acde8fbcf`,
efs-fuse `f9224ed604156f6d5000b36bd1d6bea7b6ca1f65a0fbda3c779dac8ad5654cdf`,
efs-mgmt `01a1fcffd6658bbe9a7466e16e5529c33e9b1c3944db1e19e82fa09fe39ecbc6`.
Use the common quiet command with fresh38010/38020/38030 ports and focused38050.

### Committed-prefix probes and heartbeat progress

429e320d changes only fresh read contacts: a zero-entry probe carries at most
the peer's already acknowledged committed prefix. Compacted hints fall back to
zero. The existing term, boot, configuration, durability and request-round checks
still decide quorum; normal data-serving reads still require their own applied
index. These contacts never clear outstanding write batches and do not reset
`hb_elapsed`, so read storms cannot postpone ordinary replication heartbeats.
This removes read contact's coupling to unrelated future WAL writes.

The actual-core `test_read_probe_committed_prefix` runs group0 and group2,
queues a locally durable but uncommitted tail, requires fresh quorum to cover
only the prior committed point without applying that tail, then starts repeated
reads immediately before ticks and requires ordinary heartbeat replication to
complete. `./tests/test_raft` and `EFS_TEST_EARLY_READ_ACK=1 ./tests/test_raft`
PASS with the change. Rebuilding the same tests with exact a262e71b raft.c yields
eight failures: probe shape, committed-prefix readiness and heartbeat progress.
That negative comparison is retained externally as `probe-negative-*`.

Exact committed input787 aggregate
`af101cde8c0e338a7b1d41c2117ce9c1c962d3e77b0602fdc088c570aaa04c82`,
manifest `efs-probe-inputs.json`, source `probe-src`, build IDs429e320d/devel.
Full normal/fault native, fifteen focused normal/sanitizer, fully instrumented
actual-core normal/early-ACK, three quiet repeats and focused acknowledged-fsync
crash gates are RUNNING/NOTRUN pending their markers. Reproduce the full native
checks with `make test`, then a separate clean `EFS_FAULTS=1` build and test;
use the common quiet command at38110/38120/38130 and focused38150. The unchanged
four4G, seven×16 core and30/75/20/180s budgets apply. No profilers/builds overlap
accepted quiet measurements. W95 remains open; production-format/migration and
matched packing-on/off performance decisions also remain separate obligations.


### Committed-prefix result and accounted reply follow-up

Exact429e320d full normal/fault native, fifteen focused normal/ASan/UBSan and
fully instrumented actual-core normal/early-ACK PASS. Quiet round1 at38110
fails1505/1512: deep60 timeout on each client; unusual-name and deep100 cases
PASS. Peer67/67, four orderly zero exits, cold acknowledged bytes, physical
reclamation and metadata drain PASS. Further quiet rounds/crash NOTRUN after
failure. External `probe-run.exit=1`, `probe-quiet-1.log`, fixture
`root-a/gc-direct-v0rt9ju3`. Binary SHA-256 efsd
`5a89be009a504546d0a53018e745fc4b5b50a7091f7078c5a4d141c9ed92c900`,
efs-fuse `9987914f4cbe622acf70dfbaa5fbec0aeea1cf499de9206e13b68dfd8d981938`,
efs-mgmt `09701a5dacdfc39ffbabb317eed43b18d94a3320ab9e0337e727c02049919a0d`.

Sequential low-rate diagnostic on the same binaries: enable
`EFS_POSIX_PHASE_TRACE=1 EFS_READ_PROF=1 EFS_RPC_PROF=1 EFS_RAFT_OBS=1` with the
common seven×16 command at38210. Fixture `root-a/gc-direct-6_mzxsu8`, external
`probe-read-diag.*`. All deep60/deep100/unusual-name directory cases PASS,
but seven ordinary I/O failures across five clients prevent acceptance:
client0 concurrent write/readdir; client1 many-files and concurrent same-dir
creates; client2 rename-to-self; client3 concurrent same-dir creates; client5
many-files. Observer overhead and varying leadership prohibit treating this
as a quiet repeat. Active metadata leader cumulative final lookup parent fence
1385us versus KV32us, local leader read1007us; these are workload stage means,
not per-directory causal percentages. Node3 accumulated100900 inbox refusals
and13 PUT errors in the diagnostic. This is a feedback/queue-pressure lead,
not proof that every refusal caused an application error. Preserve failures.

Inspection found already-known-prefix probe replies fell through to normal
receive whenever an unrelated local write suffix existed; the host also
requeued accounted replies when other inbox work existed. e301bc3f consumes
only successful same-term/boot/group/voter probe replies whose match is no
newer than the peer's already-known match. It records liveness and only the
matching fresh round's vote, without changing next/inflight, commit or apply.
Real new-prefix progress replies still return to ordered receive; pending
requests remain ordered. This preserves the existing early vote behavior,
while removing useless queue/replication work. No new quorum, lease, wire or
cached-authority rule is introduced.

Actual-core normal and early-ACK PASS; exact429e320d core fails six new runtime
assertions about known-prefix consumption. The regression also checks fresh
versus stale round authority, no unrelated commit/apply, and new-prefix reply
fallback. `python3 tests/test_read_control_host.py` verifies pending requests,
progress replies and fully accounted replies separately. Working diagnostic
build results are preliminary; exact committed input787 aggregate
`28a266dfa31899adf74bff551479b06e8285f28c06155d3d8c2d9f844e23d920`,
manifest `efs-accounted-inputs.json`, source `accounted-src`, build IDse301bc3f/devel.
Exact native/fault, focused and actual-core sanitizer gates RUNNING; quiet
rounds38310/38320/38330 and focused38350 NOTRUN until safety finishes. Same
scope and original budgets apply; no push or release promotion. Raw outputs
remain outside Git under the common external base with `accounted-*` names.


### Accounted-probe exact-source checkpoint

Exact e301bc3f full normal/fault native, fifteen focused normal/ASan/UBSan and
fully instrumented actual Raft normal/early-ACK PASS. Quiet38310 core1505/1512
FAIL: deep60 on all seven clients times out at30s; unusual-name/deep100 and all
other core cases PASS. Peer67/67, four zero orderly exits, acknowledged cold
hashes, physical packed reclamation and metadata queue drain PASS. Further
repeats and focused crash NOTRUN after failure. Raw fixture
`root-a/gc-direct-eo4sfvfk`, `accounted-quiet-1.log`, `accounted-run.exit=1`.
Executing binaries' flags were inspected: packing1, commands0/server0, repair0
on all four daemons and seven FUSE clients.

Binary SHA-256 efsd `296b57e9241d22c9ee306ae285fe7681dc3fd8daac469a17dae1627bc862ea9f`,
efs-fuse `bf962970e32ec689848035517ebba59b6d7101c4509f089ee6e1f0f3199653de`,
efs-mgmt `73981f14c7f8490d7433c921d967a9c7c00bb25b6ed96dcc9c4ba1efe67ac3cb`.

A sequential lower-overhead diagnostic enables only EFS_POSIX_PHASE_TRACE and
EFS_READ_PROF on the same binaries, port38410, `accounted-read-diag.*`, fixture
`root-a/gc-direct-hi6ef9v0`. All deep60 cases again stop during create, with no
later phase observed; all seven are30s timeouts, other core cases PASS. Missing
datagrams remain unknown. During load, node1 parent fence4291us versus KV38us;
node2 parent fence7499us versus KV42us, remote proof6670us and post-proof own
apply4839us. These stage means overlap different call populations and must not
be summed into a claimed per-case breakdown. Unlike the prior instrumented
run, no ordinary core I/O errors were observed. W95 remains open.

Next experiment changes only HOST_INBOX_SLICE_MAX8 to32 on an external copy of
this source, retaining bounded FIFO order and original admission/retirement
accounting. Purpose: test whether fewer durability cycles reduce fresh
metadata/apply waits; no timeout, quorum, wire or durability change. Working
source is `slice32-dev-src`, parent e301bc3f plus the one constant delta; it is
not exact committed-source qualification. Build and actual inbox FIFO regression
precede the common quiet workload at38510, with no concurrent diagnostic/build.
Do not retain the tuning without demonstrated benefit and final source gates.


### Bounded FIFO batching candidate

The initial working experiment e301bc3f plus HOST_INBOX_SLICE_MAX32 passes
1512/1512 core at38510, including all seven deep60, deep100 and unusual-name
cases; no core skip/notrun. It uses default quiet flags and the unchanged
four4G, seven×16 workload. Source `slice32-dev-src`, build label
e301bc3f-slice32, raw `slice32-dev.*`, fixture `root-a/gc-direct-cyl2qfvr`.
This comparison is one preliminary run; exact-source and repeated acceptance
are still required. The effect is consistent with amortizing the pump's
existing KV durability tail over more admitted FIFO frames, but no per-syscall
causal percentage or throughput speedup is claimed.

ac2133fb commits only8→32. The host still takes a bounded FIFO prefix, retains
tails ahead of new arrivals, preserves control/config boundaries, and retires
only taken credits through the existing post-KV durable checkpoint path.
No user or internal operation deadline changes. Actual inbox/read-control
regressions PASS. Exact input787 aggregate
`1502bc2bdc79f6cfe26eabf346a62879a13ec2e3ed1fd36b396c2c3b5af54d5e`,
external `efs-batch32-inputs.json`, source `batch32-src`, build IDsac2133fb/devel.
The runner waits for the preliminary fixture's full cleanup before building.
Full normal/fault native, fifteen focused normal/sanitizer and actual-core
normal/early-ACK sanitizer are RUNNING/NOTRUN; then three quiet rounds at
38610/38620/38630 and focused acknowledged-fsync crash at38650. No concurrent
build/profiler in quiet measurements. No push or release promotion.


Current focused regression commands (run from the exact named source):

```sh
for t in readindex_admission readindex_result readindex_coalescer read_waiters \
  readindex_pair readindex_completion readindex_remote_pair read_control_host \
  readindex_operation readindex_publish host_inbox peer_pool_deadline \
  host_read_deadline net_receive_deadline small_frame_receive; do
  python3 "tests/test_$t.py" || exit 1
done
```

Repeat that loop with a temporary `cc` wrapper placed first in PATH, executing
`/usr/bin/gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g "$@"`;
set `EFS_TEST_SANITIZE=1 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
UBSAN_OPTIONS=halt_on_error=1`. For the actual core, use a separate clean source
copy and compile/link `libefs.a tests/test_raft` with the same wrapper via CC,
then run `./tests/test_raft` normally and with `EFS_TEST_EARLY_READ_ACK=1` under
those sanitizer environment settings. Keep safety builds separate from the
unmodified quiet binaries and finish all builds before starting live workloads.
To reproduce the negative comparisons, overlay the prior commit's raft.c in a
separate copy containing the newer tests/test_raft.c and rebuild the core test;
the documented assertions must fail. Never run that mixed negative source as
live acceptance. All raw outputs belong outside Git.


### Exact bounded-batching repeats and transient packed flush

Exact ac2133fb full normal/fault native, fifteen focused normal/ASan/UBSan and
fully instrumented actual-core normal/early-ACK PASS. Quiet round1 at38610
PASS1512/1512 core,67/67 peer, orderly zero exits, cold bytes, physical
reclamation and metadata drain. Quiet round2 at38620 **FAIL1511/1512**: client3
concurrent_write_and_readdir reports EIO; all deep60, deep100 and unusual-name
cases PASS on every client in both rounds. Round2 peer67/67, orderly exits,
cold bytes, physical reclamation and metadata drain PASS independently.
Round3/focused crash NOTRUN after round2 failure; full repeatability not closed.
Raw first fixture `root-a/gc-direct-b26eei0a`, second `root-a/gc-direct-jhlfwovt`,
external `batch32-*`, exit1. The preliminary larger-slice run also finished
all lifecycle gates PASS, but does not substitute for exact-source acceptance.

Binary SHA-256 efsd `f465e920fbe738e242fee15a8a8c47b9d8553a110df7f7b876215ae7de00b679`,
efs-fuse `6a8e6d6f2031d253409c51e1ee2a3fdd7c857ea758eb9897c2300da5625837a3`,
efs-mgmt `2d59840deb331cdefef4ac889501a9516e0ad3df350a9481df9be2bc9cee56c3`.
Raw hashes: quiet1 `1cfd6b1260b337f1a4a8e69ab10ccfa6e0bbdd16d07d227bbf3e82bdfa99599c`,
normal native `575d5c58c070c252fd64f62b524550ec29ab451eb0435c66a899e0414d9d5f88`,
fault native `9683c1956b6a4aa37aedc3ce03f956aa674402d012bb89a0c00487a7fa9300dc`.

The round2 FUSE client3 records `flush-meta ... efs_rc=-15 ino=154178` at
06:11:57.690Z. EFS_ERR_NOT_PRIMARY is a metadata-authority refusal, not a disk
I/O error. Node3 PUT error counts also rose during that window; aggregate
counters alone do not prove one-to-one causal mapping. Inspection identifies
a concrete client defect: pack_flush_run retains the immutable object, ACK
mask and writers after pack_progress refuses authority, but returns every
status except BUSY immediately. FUSE therefore maps a retryable NOT_PRIMARY
to permanent close EIO. a2c2ee5e allows that status through the existing retry
pause and deadline. It changes neither fragment placement/identity nor ACK
requirements, and never manufactures authority or publication success.

`python3 tests/test_mounted_pack_flush_retry.py` executes the actual flush loop
and deadline wrapper. It checks NOT_PRIMARY→NOT_PRIMARY→success on the same
object77 and ACK mask5, BUSY retry, deadline exhaustion/restoration, clean
inode fast exit, and immediate IO/PROTO/STALE/INVAL/NET failure. Overlaying
exact ac2133fb pack_write.c through EFS_PACK_WRITE_SOURCE fails the new test;
fixed source PASS. The pre-existing mounted cohort test requires libefs.a and
could not link in the unbuilt Mac checkout; Linux native/focused checks will
run it against the built exact source. No warning or safety rule is disabled.

Exact input788 aggregate
`1fd54809c67fa6e0ee7b774e4551d367cb4c364e4735b0c615e69b0a9fe8f318`,
external `efs-flush-inputs.json`, source `flush-src`, build IDsa2c2ee5e/devel.
Full normal/fault native and nineteen focused normal/sanitizer (the prior
fifteen plus mounted_pack_flush_retry, mounted_pack_retry, mounted_pack_batch,
mounted_pack_parallel), plus actual-core sanitizer checks RUNNING/NOTRUN.
Then fresh quiet38710/38720/38730 and focused38750, all original budgets and
scope. Runner waits for batch32 cleanup; raw `flush-*` is outside Git. No push.


### Packed-flush repeats and per-reader quorum target

Exact a2c2ee5e full normal/fault native, nineteen focused normal/ASan/UBSan and
fully instrumented actual Raft normal/early-ACK PASS. Quiet rounds1/2 PASS
1512/1512 core and67/67 peer, orderly shutdown, cold acknowledged bytes,
physical reclamation and metadata drain. Round3 **FAIL1511/1512**: client2
only deep60 times out at30s; all other core cases, unusual-name/deep100,
peer67/67 and every lifecycle gate PASS. Thus the EIO regression is fixed in
these runs, but timeout repeatability is not closed. Focused crash NOTRUN
after round3 failure. Raw external `flush-*`, exit1; fixtures
`root-a/gc-direct-cqkj14yb`, `root-a/gc-direct-44s0sdfj`,
`root-a/gc-direct-xoohuiiy`. No accepted run overlaps any build/profiler.

Binary SHA-256 efsd `30c59d624a67d358aefa5c778d9f6479509665143df4bc5037d394b56ef9f71f`,
efs-fuse `e8917c13edf759b9b6c38a1084494c6b53561f4d24feae400f25af862aedff15`,
efs-mgmt `8b4ebbf7b1a47d75b81bbdec5f706ef0485c145e846c6593abdd52f1f490d64a`.

A deterministic additional wait is reproduced: a reader admitted for round11
observes sufficient quorum index60 while apply is50. The group's oldest
retained proof can still be round10/index55, and later raw round12/index900
can supersede round11 before this reader catches up. Once apply reaches60,
the old wake predicate still waits for900. A scheduling wake for an older
completed round can also prevent recording the later sufficient proof before
the handler reacquires its CV mutex. This is a concrete target-selection defect,
not proof that it accounts for the entire last live timeout.

8151debe stores the first sufficient proof index in the existing bounded
waiter slot and copies it back to that reader. Capture and wake occur under
cv_mu; already signalled slots can still capture their first eligible proof,
without repeat signals. A data reader must apply that captured index before
reading rows; proof-only replies still return no rows. No cross-request cache,
lease, extra helper thread, heap allocation or new wire field is added.

The coherent host view additionally publishes a local invalidation epoch.
The Raft core increments it whenever existing proof context is forgotten;
normal new read rounds do not increment it. Configuration A→B→A cannot restore
an old epoch. Role/term changes, failed storage or a missing replica refuse
retention. Epoch saturation never wraps: it disables this retained-certificate
optimization and leaves canonical fresh quorum reads available. This is not
persisted authority; host restart has no surviving waiter scopes.

`python3 tests/test_read_waiters.py` checks first index60 remains sufficient
when raw proof becomes900 and apply reaches60, including the already-signalled
scheduling case, while context/storage invalidation still wakes refusal.
Replacing only the wake predicate with the prior host_read_wait_ready behavior
fails the new runtime assertion. `test_readindex_result.py` executes the actual
reader with captured110, newer raw900 and applied110; it returns110, but changed
or unavailable context refuses. Coherent publication and completed-proof
regressions PASS. Actual `test_read_context_epoch` runs elections/configuration
transitions A→B→A and stepdown, requiring non-reused epoch identities; normal
and early-read-ACK Raft runs PASS on the preliminary working build.

Exact committed input788 aggregate
`096fcc7178f1f4e042ccb35ce355bdc2c78e09a004bfb2feede99126b5e22ecf`,
external `efs-frozen-inputs.json`, source `frozen-src`, build IDs8151debe/devel.
Full normal/fault native, nineteen focused normal/sanitizer, actual-core
normal/early-ACK sanitizer RUNNING/NOTRUN, then quiet38810/38820/38830 and
focused38850. Same common commands, four4G, seven×16 and30/75/20/180s budgets.
Raw `frozen-*` remains external. W95 stays open pending current-source acceptance
and its separate production-format/migration/performance obligations.


### Final current-source acceptance — PASS

Exact **8151debe7810b5f5814dd18513608fefd8073bbd**, with the788-input digest
above and build IDs8151debe/devel. All owned fixtures and screens have exited;
unrelated ordinary AMD services remain untouched. Logs, traces, manifests and
per-case output are external; no raw artifacts are committed.

| Gate | Verdict | Evidence / unchanged workload |
|---|---|---|
| Full native, normal | PASS | `frozen-exact-native.exit=0`, `make test` |
| Full native, EFS_FAULTS=1 | PASS | `frozen-fault-native.exit=0`, separate clean fault build/test |
| Nineteen focused regressions, normal and ASan/UBSan | PASS | Read admission/result/coalescer/waiters/pairs/control/completion/publish, inbox and deadlines/receive; mounted flush retry/cohort/batch/parallel |
| Fully instrumented actual Raft, normal and early-read-ACK | PASS | `frozen-core-san.exit=0`, including configuration ABA, stepdown, pending-suffix and heartbeat progress |
| Quiet round1, ports38810–38813 | PASS | core1512/1512, peer67/67; orderly/cold/physical/drain PASS; `gc-direct-f000jaj_` |
| Quiet round2, ports38820–38823 | PASS | same complete verdicts; `gc-direct-cgs8j9yb` |
| Quiet round3, ports38830–38833 | PASS | same complete verdicts; `gc-direct-ro016xli` |
| Focused acknowledged-fsync crash, ports38850–38853 | PASS | all four daemons actually SIGKILL after client drain; cold hashes, packed reclamation/drain PASS; `gc-direct-4zk4rwp3` |

All21 per-client TSVs were parsed: exactly216 selected core cases per client,
all PASS, including deep60/deep100/names_crazy_dirs. Total4536 core plus201
peer passes across the three quiet repeats. All four mounted-packing summaries
have no failures, cold_verified=true and physical_packed_empty=true, with
mode=direct, transport=tcp, packing1 and commands0/server0. Final runner exit0.
No profiler, build or other owned fixture overlaps an accepted quiet run.
Four4G, seven×16 and original30/75/20/180s budgets are unchanged. This is
functional/repeatability evidence, not an isolated per-fix speedup estimate.

Binary SHA-256:

- efsd: `095d885ea4fd547cd2cf43c60cb81fdf3f5506d99af6aa1031bf902eea66c65c`.
- efs-fuse: `554cebc084bf81703af2c10b0d3c0ce1c78e063dd36fba1791d27cb1686cd0b1`.
- efs-mgmt: `c9737280ac2f815c392953cec86ec55e179b16e94603f10be7779682fbb59c40`.

External gate SHA-256 under the common `amd_efs:/data1/efs/packing-qualification-20261012` base:

- normal native `frozen-exact-native.log`: `11b16ba912f49f1f9d759029a8077f31c67d280aaeae99157acd0df32284a848`.
- fault native `frozen-fault-native.log`: `edf2a66673b283b7ef89c7a1ea6a9084260d5091094978dab6fd1efd93491bed`.
- actual-core normal/early sanitizer logs (each): `6d5a0e475c33ea6abcdf753bbbfa2d556d4b0998d269589838fb7cd8d8e6f40d`.
- quiet1: `0e7f6e00fb71d177bda1bc4261893e71df7ed0cc1b17cde1c44953cad98db9dc`.
- quiet2: `3ca73b751da6f140f4e78e73d03f5ac4ffc1341bcf42a4fa2f02fb305354f130`.
- quiet3: `921d7155daaa84d8f1214b1f0ef623d8999603f8136ebca2d1c4962779a3eeb7`.
- focused crash: `c50bd0a6baa2669dfcde9f2c65c3357fa6678bad6101dd433327cd9e846c4796`.

Reproduce using the common commands above at this exact commit: clean normal
and separate clean fault builds first, then the focused loop plus
`python3 tests/test_mounted_pack_flush_retry.py`, `test_mounted_pack_retry.py`,
`test_mounted_pack_batch.py`, `test_mounted_pack_parallel.py`, all four under the
same normal and sanitizer compiler wrapper. Run the actual core from a fully
sanitized library as documented. Only after all builds finish, run three fresh
quiet seven-client/jobs16 fixtures at38810/38820/38830. For focused crash use
the common focused command at38850 with `--packing-crash-after-fsync` (without
`--posix --packing-clients 7 --posix-jobs 16`). Expected final runner and fixture
checks are those in the table. Never relax deadlines to reproduce this pass.
After the harness finally exits and unmounts owned clients, remove only stopped
synthetic node directories identified by each manifest; retain useful external
logs/summaries. Capacity cleanup is authorized on AMD, but unrelated paths and
services must not be touched. No push or production activation.
