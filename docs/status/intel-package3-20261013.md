# Intel package 3 — ordinary files, October 13, 2026

Current checkpoint: [EOF/admission wrap-up](intel-eof-admission-checkpoint-20261013.md).
The human coordinated hold pauses this dependency work. Corrections04f4c8c7 and
1b3844d7 are committed but not installed; original1a737143 daemons retained and
main mounts restored. Candidate mounted/loaded qualification remains NOTRUN.

W97/D30/W106 and shared-core dependencies remain OPEN. User explicitly assigned
Intel the disabled-feature profile in [the three packages](three-work-packages-20261013.md).
This supersedes the experimental container sprint on this cluster.

## Deployment and native prerequisites

Exact deployed product: `1a737143e5437e0f4315e86b7ef51e38a989b811` (`devel`).
Intel1/Intel2 Core Ultra 7 256V, eight CPUs and approximately 15 GiB RAM per host;
Rocky Linux10.2, two storage roots per host. Four files-engine daemons at
10.0.0.207/209 ports17432/17433; quotas400G each. `/data1/efs/mnt` on each host
seeds the opposite host17432. Existing roughly92GiB/node payload and metadata
were preserved. No reset, force-unmount or root change. The separate four-node
container fixture and its seven mounts were cleanly stopped, stores retained.

TCP/direct I/O, explicit `--storage-engine files`. Actual `/proc` args/environment
on all four daemons and both clients verify `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0`, `EFS_EXPIRY=0`.
Client virtual `.efs` absent; durable administrator command mask changed from
3/version0 to0/version1 and verified after orderly restart. Ordinary operational
deadlines remain active; disabling expiry means the optional expiry-date feature.

Devops correction `a45dbfc`: Intel defaults commands off; validated feature
settings exported and forwarded in every distributed startup phase; daemon
storage engine explicit files; idempotent startup refuses changed daemon server
command/expiry or client command/packing settings instead of silently accepting
an old process. `test_start_mode.py` and identity checks PASS locally (Linux
/proc part skipped locally); actual startup-mode integration PASS on both Intel
hosts. Final client idempotence extension uses the same inspected environment;
post-deployment runtime verification PASS. No product behavior correction is
claimed for a configuration change.

Complete `make -j8 test` PASS on both hosts at the exact product source.
First attempts failed the architecture link gate because the deploy copy omits
historical results. Private native trees were supplied the original tracked
results from `git archive 1a737143`; complete reruns exit0. Preserve first FAIL
and corrected PASS; do not classify missing test inputs as a product failure.
Normal suite includes relevant synthetic fault regressions. It is not a claim
that the entire separately fault-enabled build suite passed.

Six affected harnesses PASS with ASan/UBSan on both hosts:
write_acceptance_gate, close_retire_fastpath, close_retire_batch, close_effects,
retained_create_authority, flock_description_release. Compile through
`CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'`,
`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`.
Harnesses extracting actual production helpers retain their usual stubs;
close-effects links the normal library, so this is not a fully instrumented
entire-product claim.

## Reproduction and pending mounted gates

Deploy a clean `devel` snapshot using the corrected `devops/nuc-efs` scripts:
`EFS_CLUSTER=intel EFS_TRANSPORT=tcp EFS_DIRECT_IO=1 EFS_COMMANDS=0
EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_START_TIMEOUT=300`.
Build both hosts before stopping services. Globally drain both clients before
stopping any daemon, then activate the built source and run `start.sh`.
Do not remove storage or rerun formatting on existing stores.
Deploy `scripts/intel_package3_verify.py` to
`/data1/efs/devops/intel_package3_verify.py` and execute on each host. This
asserts actual running features, storage flags, quota, mount and persisted policy.
For a full native copy include tracked docs/results assets as well as runtime
source; run `make -j8 test` under the four disabled feature variables.

Driver:
`python3 scripts/intel_package3_tests.py core --output OUT --run-id UNIQUE`.
Repeat with `extras`, `runtime`, `concurrent`, `latency --seconds 600`.
Concurrent creates five temporary clients in addition to the two real mounts,
uses4+3 clients/jobs16, then cleanly drains only those temporary mounts.
Core/extras use existing30s case budgets and16 jobs on both real mounts
concurrently, unique tags, original suite-specific deadlines and900s outer
bound. Raw TSV/log/JSON output must remain outside Git. Failed test directories
are retained by the original suite, not deleted to conceal failures.
Peer suite: prepare a unique parent on Intel1, wait for Intel2 visibility, then
run `python3 /Users/mike/git/devops/nuc-efs/intel-smoke2.py ''`
from the driver. The helper loads the deployed suite and runs its actual remote
steps on both production mounts with existing120s peer-step deadlines. Its TSV
path is printed; keep raw output outside Git. The preparation is automatic.

W106 small-fixture preparation: `python3 scripts/intel_package3_fork_fixture.py
prepare /data1/efs/mnt/package3-fork-UNIQUE --count 128` on Intel1. Compile
`gcc -O2 -pthread tests/live/efs_direct_read_fork.c -ldl -o /tmp/read-fork`.
Run `/tmp/read-fork ROOT 0 128 128` then `EFS_DIAG_FORK=1 /tmp/read-fork ROOT
0 128 128` on each host. Existing default large-fixture geometry is unchanged;
optional count/stride permits1024 files/256MiB total, exact independent payload
formula, all four64KiB reads and EOF verification. Runtime libcrypto SHA256 is
loaded without development headers. `verify` checks every byte/path/EOF;
repeat after orderly restart, then `delete` removes only the named owned tree.
This smaller diagnostic does not close original ORCD scale/platform acceptance.

Mounted POSIX, peer, snapshot/rotation latency, loaded EOF, fork/direct-read,
physical GC and complete deterministic cold integrity gates are in progress or
NOTRUN; no passing verdict is inherited from earlier container/AMD/ORCD tests.
No push, merge, release promotion or excluded-feature activation.

Raw evidence outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-package3-20261013`.
Original frozen source/archive:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-devel-20261013`.

## Initial mounted results and metadata-admission candidate

Exact product1a737143: two simultaneous jobs16 core runs PASS432/432
(44.7/47.6s suite times). Cross-host peer PASS67/67 (78.4s); extras each1/5 PASS,
zero failures/four legitimate skips (disabled virtual commands and unsupported
shared mmap; private mmap PASS). Extras briefly overlapped the beginning of
peer execution; this is recorded loaded execution, not a quiet latency control.
Seven clients4+3/jobs16 normal concurrent **FAIL1511/1512**: only Intel1 temporary
client1 dir_deep_nesting30s timeout; other six216/216 PASS, controller92.541–
101.226s. All five temporary clients cleanly drained, no forced discard.

Capability-preserving phase-only diagnostic (expensive profilers OFF) also
**FAIL1511/1512**, this time Intel1client2 dangling-symlink unlink EBUSY;
all seven deep cases PASS. Observed deep creation11.150–23.265s, before leaf/
recursive cleanup. The nonblocking observer loses some events under load;
missing completion events and incomplete call counts are not evidence of an
unfinished passing case. Analyze only `test=dir_deep_nesting`, separately from
the beyond64 test. This is diagnostic evidence, not normal acceptance or an
isolated causal improvement. No normal timeout or BUSY failure is closed.
Reproduce with `concurrent --phase-observer` and a fresh run-id/output directory.

Small W1061024-file/256MiB no-fork and5ms fork-thread read diagnostics PASS on
both hosts, every file/all four64KiB parts. Existing ORCD scale/kernel finding
remains open; do not transfer bounded Intel evidence to its platform gate.
Warm deterministic256x512KiB/4worker lifecycle PASS: create2.903s; read30.016s/
6911ops; churn30.015s/6076ops; verify256/256 hashes1.070s. No errors, warm
read integrity PASS. The test does not inventory all pre-existing production
payloads, for which this chat has no immutable historical baseline.

Candidate: use pre-mutex read admission tracking for ordinary files as well as
containers. Files previously registered no ticket and could not share a probe
sent AFTER their request arrived while waiting for the mutex; they conservatively
waited for another round. Registration watermark is captured before sends;
late arrivals still require a new round. Term/round/own-apply/quorum predicates,
RPC/case deadlines, voters, durable storage and core batch flags unchanged.
Counter saturation falls back to the existing conservative admission path.
No inbox consumption, storage-engine activation or query permission changes.
This is a plausible shared metadata-latency improvement, not a measured fix yet.

Local `test_container_read_admission.py` normal+ASan/UBSan PASS, extended to
files early/late admission while retaining container, term/round/failure/
saturation and1024 concurrent-ticket uniqueness controls. Exact1a737143 old
production helper fails intended file_ticket==4 assertion with the new fixture.
`test_readindex_pair.py` (including existing sanitizer/negative controls) and
`test_readindex_result.py` PASS. Both-host full native, affected fault/sanitizer,
new-source deployment and loaded requalification are NOTRUN for this candidate.
Baseline600s normal snapshot-pressure/foreground/resource gate is running on
unchanged deployed1a737143. Complete it before native builds or redeployment.

## Baseline pressure and candidate prerequisite correction

1a737143 baseline600s pressure PASS on both hosts: foreground600 cycles each,
all512-byte payloads checked, zero failures, existing1s syscall limit retained.
Maximum create/write/fsync/close/unlink ms: Intel1 75.377/0.286/108.295/0.258/
109.476; Intel2 150.393/0.311/369.747/0.294/81.568. Snapshot xattr pressure
64551(group0)/64392(group2) verified updates, zero errors, maximum452.413/
472.124ms. These normal-budget loaded samples do not reproduce or close the
original5.569/22.044s tails. Fresh captured daemon logs contain no snapshot/
rotation marker, so a triggered full rotation is NOT established by this run.
Use the standalone retained-log rotation/replay fixture and retain broader
snapshot-pressure qualification as open if no trigger proof is captured.

Candidate2b927c1b first complete native attempts FAIL at documentation checks,
not a reported runtime regression: the historical-results symlink resolves
outside the candidate tree and is correctly rejected; my status README update
also required regeneration of architecture-full.md/architecture.html.
Regenerate the artifacts using `python3 docs/gen-architecture-full.py`; local
`python3 docs/check-architecture.py` PASS all5 checks. Supply real files (a
same-filesystem hardlinked copy is suitable for unchanged historical results),
not an escaping symlink, in native test trees. Both-host complete candidate
native reruns are pending. First failed native evidence is retained, and no
candidate was deployed before its prerequisites passed.

## Rejected ordinary-files admission experiment

Frozen7241653012c64b8cdb70dc09f32b1b77c556ffb6 has the same853 product/test
inputs as2b927c1b; full normal native and admission ASan/UBSan PASS both hosts.
Orderly deployment and actual disabled-profile checks PASS, all1024 retained
fork-fixture files checked through both mounts and all256 lifecycle hashes
PASS after upgrade. These are upgrade integrity, not new-source same-source
cold acceptance.

Normal seven-client/jobs16 candidate run FAIL1472/1512 PASS:40 failed cases, broad deep/name/append/mtime timeouts plus
EBUSY in create/unlink and mkdirat/unlinkat. This is a failed loaded result,
not accepted merely because native correctness controls pass. A subsequent
rollback control also fails; the worsening is not isolated to this code change. Reject the
ordinary-files tracking extension and restore the exact prior production
read-admission behavior; retain all source/test/failed evidence in history.
Restore Intel binaries to1a737143 under package3 feature settings, preserve all
storage and cleanly drain clients. No force, reset or timeout/concurrency change.
Further liveness diagnosis is required before another candidate.

Correction to the earlier baseline log search: `raft-snap` was omitted from its
search expression. Fresh captured baseline logs actually contain successful
snapshots in BOTH groups, six voter advances192–287ms and six background exports
approximately643–651MiB,903–2299ms. Foreground1s gate PASS during them. Thus
normal snapshots were triggered; original5.569/22.044s foreground tails remain
unreproduced in this bounded run. Do not confuse exporter duration with a
foreground blocked interval or claim the original stall is globally fixed.


## Rollback control and measured helper saturation

Rollback1a737143 actual disabled-profile/membership checks PASS; no storage reset
or forced stop. Fresh normal seven-client control FAIL1461/1512 (51 failures).
This prevents attributing the earlier40 failures entirely to the rejected
admission change. Read-admission behavior remains reverted while investigating
state/GC/routing effects. Bounded post-run tails show heavy incremental
lane-sweep/reaping; BUSY lane-sweep is normal partial progress, not by itself
a physical-GC failure or permission to discard state.

Diagnostic source1a737143, EFS_READ_PROF=1, no Python observer, original jobs16/
case deadlines: FAIL1473/1512 (39). This is NOT normal acceptance. Actual daemon
profile flags verified; fresh offsets recorded and bounded tails collected
after timed workers and temporary-client drain. n2 final cumulative pair
parallel14471/scalar2414; its LOOKUP parent proof mean25.372ms, remote proof
mean6.893ms plus own-apply wait1.880ms. Leader n1/n4 freshness mean3.594/3.846ms,
maximum329ms. Counts demonstrate helper saturation but do not establish that
capacity alone explains all observed failures. Local KV lookup/getattr means
are tens of microseconds in this diagnostic. Profiling explicitly turned OFF
again via a clean all-host restart before normal qualification.

Next candidate: initialized ordinary-file hosts use the existing fixed32
paired-proof helpers instead of8. Maximum32 persistent128KiB stacks (4MiB per
process, previously1MiB for files); no queue or per-call thread creation.
Existing scalar fallback on saturation/initialization failure, independent
fresh proofs, inherited deadlines, failure propagation and joined task/host
lifetime unchanged. No admission tracking, quorum, voter, storage, feature
or deadline modification. This is a measured-capacity candidate, not accepted
yet. Source now also contains independently merged query/quota changes from
other packages; freeze and qualify its exact full inputs rather than silently
treating it as the old1a737143 image.

Actual production pool fixture `test_readindex_remote_pair.py` normal+ASan/UBSan
PASS locally for files and containers, full32 occupancy, overflow scalar path,
partial/failed initialization, distinct deadlines, first-proof failure waiting
for bounded second task, no growth during reuse and128KiB stack bound. Old
1a737143 production capacity fails the new files-capacity assertion. Both-host
full native/build, new-source live qualification and same-source cold/physical
gates are pending. Optional validated read-profiler forwarding committed in
devops `a140b51`; unset settings preserve normal defaults.

## Human scope correction: correctness only

The 32-helper candidate `328153c9` completed both-host native checks, but was
not deployed or accepted. Human clarification prohibits performance tuning.
`5f8ec779` restores ordinary-file capacity8, with fixture checks passing. Intel
performance investigation is paused; production remains baseline1a737143 with
the package3 disabled-feature profile and profiling OFF. Existing loaded
failures remain OPEN. The active work is now [checked query-sort cleanup and
actual temporary peak accounting](query-sort-peak-correctness-20261013.md) for
ORCD acceptance under unchanged16/32GiB limits, with no further compression.

## Authorized cross-host network diagnostic

[2026-10-13 network workloads](intel-network-workloads-20261013.md) PASS for
32×64MiB and20,000×4KiB files, opposite-host byte verification, small-file
rename/unlink and normal fixture/client cleanup on deployed1a737143. TCP
retransmissions35/51 and NIC hardware receive misses10/6 were observed.
No timeout/reset increment or logged transport retry; seven metadata RPC
backoffs per large writer. Cause remains OPEN; no tuning or claim of broader
seven-client, ORCD or physical-reclamation acceptance.
