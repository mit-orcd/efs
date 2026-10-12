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

Tested candidate: **bdbb237bcd1e4e11aa1ab3eb6fb2fceb10070a6b**, fixture isolation only; no packing product change. First full quiet lifecycle PASS; fault native and nine focused sanitizers PASS; second quiet lifecycle FAIL (two original-budget deep-directory timeouts).
Older mounted packing failures are source-specific. The newer 32-frame receive
batch fixed ordinary deep-path qualification with packing disabled; its effect
on packing remains unverified. Lower-layer packing, decoder, admission, retry,
publication and ledger tests do not establish mounted acceptance.

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
not restart with its explicit4G quota: the startup guard reported safe maximum
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
packing enabled and both command flags zero, seven clients/jobs16,4G per-node
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
