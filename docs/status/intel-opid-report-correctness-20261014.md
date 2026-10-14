# Private Intel operation-ID admission and REPORT ownership — 2026-10-14

**Targeted native corrections PASS; loaded POSIX FAIL; W100 remains OPEN.** Follow-up to the
[seven-client profiling investigation](intel-seven-client-profile-20261014.md).
This work fixes correctness, without widening the receipt window, changing RPC
or test deadlines, increasing quotas, or tuning throughput.

## Reproduced causes and corrections

1. **Admission while holding an inode edge stripe.** Lease OPEN/CLOSE admission
   called the waiting operation-ID allocator under that stripe. An older retained
   identity can need the same stripe to complete and release receipt capacity.
   A real-thread regression fills the unchanged 16-receipt window, completes all
   but its oldest operation, and makes that owner contend for the stripe. Old
   production code waits until the contender's 500 ms deadline. The correction
   attempts lease admission once; local capacity refusal allocates no identity.
   The caller drops the stripe before retrying within its existing deadline.
   Pending CLOSE admission also follows this path. Unknown submitted requests
   retain their exact identity/body; completed higher sequences do not ACK past
   the unresolved oldest sequence. Namespace admission keeps its existing waits.
2. **A partial REPORT borrowed a newer full overwrite's base.** Diagnostic-only
   deployment reproduced retained 10-byte spans with `base_gen=UINT64_MAX`.
   The server correctly rejects unconditional partial writes. The PUT mapping
   was retained, but its REPORT could read the mutable dirty cache's newer base.
   The correction captures the span's base with the PUT and its fallback object,
   then restores it with that exact mapping. Full-image base progression is
   unchanged. Server rejection remains strict; reason/record diagnostics now
   identify malformed publications instead of accepting them or dropping dirty
   ownership.

`test_lease_admission_lock.py` executes the production allocator/lease begin
with real mutex contention. `test_open_busy.py` exercises actual FUSE OPEN and
pending-CLOSE retry, requiring the stripe to be unlocked between local refusals.
`test_report_span_base.py` executes production capture/readers/REPORT selection,
covering newer UNCOND overwrite, late older PUT exclusion, successor identity,
empty base, dcache fallback and committed-object exclusion. All three have
compile-and-execute old-source negative controls; assertion failure, rather than
compilation failure, is required. The two new regressions are in `make test`.

## Source and environment

Parent devel `5d580e3c0` (production parent `37ff1b4d45d048ed55f9585b89935c2e4928289f`),
plus this checkpoint's production/test/controller changes. Diagnostic baseline
`5d580e3c-diag1` contains logging only; candidate `5d580e3c-fix1` contains both
corrections. Both hosts build normally with GCC, `-O3 -g`, frame pointers and
native CPU target. Separate fault clients use `5d580e3c-fault1`, `EFS_FAULTS=1`;
they are never installed over normal executables.

Private runtime `/data1/efs/w100-private/runtime-deadline-posix`;
source trees `/data1/efs/w100-private/devel-opid-{diag1,fix1,fault1}` on both
Intel hosts. Intel1 `.207` serves nodes1/2; Intel2 `.209` nodes3/4,
ports27432/27433. Two 16 GiB files-engine stores per host under
`/data{1,2}/efs/w100-private-deadline-posix/nN`, direct I/O. TCP;
`.efs` client/server OFF, command policy version1/mask0, packing OFF,
containers unused, expiry OFF. Main mounts seed the opposite physical host.
Production ports17432/17433, processes, stores and mounts are outside this work.

## Qualification

Before deploying the correction, the diagnostic-only code passed one complete
seven-client cohort (1512/1512), then three more cohorts on the same mounts
(4536/4536). Seven independent clients each run216 core cases with16 jobs,
up to112 parallel test cases. Unique namespaces, original30s case budget and
longer existing decorators,900s suite outer bound. No strace/perf in these runs.
Node2 accumulated56 unconditional-span rejections across those runs despite
passing POSIX outcomes. Passing TSVs therefore do not excuse malformed REPORTs.
The older traced timeout/EBUSY run is not a controlled performance comparison.

The initial diagnostic baseline did not reproduce the earlier long admission
stalls after the private restart. The corrected three-cohort run still has
timeout/EBUSY failures. All121,319 traced operation identities completed, none
remained outstanding at collection, and the longest observed lifetime was
2.117s. There were26 final admission refusals across seven clients: refusal
is not itself evidence of a stranded identity. The real-thread negative control
proves the lock dependency; it does not prove that every historical timeout or
the ORCD report-loop EIO has this cause. W100 aggregate/ORCD acceptance remains
OPEN. Do not transfer these results to RDMA/ORCD or buffered I/O acceptance.

| Gate | Verdict / evidence |
|---|---|
|Full native `make test`, both Intel hosts|PASS; final exit0, normal binaries linked afterward|
|Three targeted production-code regressions, both hosts|PASS normally and ASan/UBSan with leak detection; old-source executables reach the intended failing assertion|
|Seven-client cohort0|1479/1512 PASS;33 FAIL|
|Same mounted clients, cohort1|1473/1512 PASS;39 FAIL|
|Same mounted clients, cohort2|1479/1512 PASS;33 FAIL|
|All corrected cohort daemon logs|Zero `pub-reject`, `invalid REPORT` or `pack-invalid` events|
|Unchanged diagnostic clients on the same aged stores/running fixed daemons|1475/1512 PASS,37 FAIL;14 unconditional-span REPORT rejections return|

Initial native attempts exposed incomplete source staging (missing tools) and
an extraction fixture boundary around the new helper. Both were repaired;
these failed attempts are retained. The complete checkout then exposed stale
architecture outputs from the preceding status update; regeneration repairs
that real documentation gate. Final complete `make test` passes on both hosts.
Expected negative-control messages inside passing tests are not suite failures.

The corrected aggregate is4431/4536 PASS,105 FAIL (83 timeouts,22 EBUSY
outcomes; no wrong-byte or application-EIO outcome in these TSVs). Failed outcomes include
names/deep directories, concurrent creates/appends, timestamp-many-write
and errno checks; raw per-case causes are retained. An additional diagnostic
comparison replaces **clients only** with the unchanged diagnostic source,
keeping all four corrected daemons and stores running. That comparison again
fails loaded POSIX (37 failures) and reproduces14 invalid10-byte span REPORTs
on node2. Restoring corrected clients does not format or restart daemons.
It supports separation of the REPORT defect from general loaded timeouts;
this sequential, stateful comparison is not an isolation of every timing
variable or a proof that the correction cannot affect latency.

## Reproduction and evidence

Prerequisites: Linux/FUSE build dependencies, Python3, SSH aliases
`intel1_efs`/`intel2_efs`, existing owned runtime/stores, private firewall rules
and matching source built on both hosts. Raw evidence stays outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-opid-report-20261014/`.
No payloads, daemon logs or copied source dumps are committed.

On each host from the frozen candidate source:

```sh
EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 make -j8 EFS_GIT_ID=5d580e3c-fix1 EFS_GIT_DESCRIBE=5d580e3c-fix1 EFS_GIT_BRANCH=devel test
python3 tests/test_lease_admission_lock.py
python3 tests/test_report_span_base.py
python3 tests/test_open_busy.py
ASAN_OPTIONS=detect_leaks=1 CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer' python3 tests/test_lease_admission_lock.py
ASAN_OPTIONS=detect_leaks=1 CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer' python3 tests/test_report_span_base.py
ASAN_OPTIONS=detect_leaks=1 CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer' python3 tests/test_open_busy.py
```

For negative controls, select pre-correction production source with
`EFS_OPID_SOURCE=/old/src/client/inode_rpc.c`,
`SPAN_READER_SOURCE=/old/src/client/write.c`, or
`EFS_FUSE_SOURCE=/old/src/client/efs_fuse.c` respectively. Preserve the current
fixture/headers and require the old executable to reach and fail its assertion.
Private start/resume/stop uses `scripts/intel_w100_private.py`; resume existing
stores rather than formatting them. Extra concurrent mounts are stopped in the
controller's `finally`; retained byte fixtures remain available for cold checks.

From the Mac, after draining all private extra clients, switch the existing
fixture without touching production or discarding writes:

```sh
python3 scripts/intel_w100_private.py stop deadline-posix devel-opid-diag1
python3 scripts/intel_w100_private.py resume deadline-posix devel-opid-fix1 --quota 16G --opid-trace
python3 scripts/intel_package3_tests.py concurrent --rounds 3 --run-id opid-fix1-20261014 --output /OUTSIDE/GIT/fix1-concurrent --source-root /data1/efs/w100-private/devel-opid-fix1 --mount /data1/efs/w100-private/runtime-deadline-posix/mnt --seed-port 27432 --remote-output-root /data1/efs/w100-private/results --extra-mount-root /data1/efs/w100-private/fix1-extra --opid-trace
```

Choose a fresh run ID/output directory for a new reproduction. Only the first
stop uses the diagnostic source; subsequent stop/resume uses the fixed source.
Record daemon/client log offsets before each workload and inspect appended
records for `pub-reject`, `invalid REPORT`, `pack-invalid` and
`opid-admission: BUSY`. Do not count historical errors in append-only logs as
new candidate failures. Keep each TSV, including failed cases, and original
limits. Trace logging is opt-in; its durations are not a speedup measurement.

## Checkpoint limits

At the first code checkpoint, the14,000-file writer/peer-MD5 run is still in
progress. Cold-byte, two-fsync, daemon-pause and fresh full recovery/GC follow-up
are NOTRUN on this candidate at that checkpoint; earlier passing runs do not
substitute for them. Main corrected clients are restored after the diagnostic
comparison. The four corrected private daemons remain running. Production
process identities were checked unchanged before deployment.

Production-input manifest (198 C/header/Makefile inputs) SHA256:
`84822bb32cdf10e297ae768c1370eb66dda62ca583906a802c596063a21f0524`;
zero mismatches on both normal and fault source trees on both hosts.
Normal executable SHA256:

|Host|efsd|efs-fuse|
|---|---|---|
|Intel1|`4afaed68afeee91c0c3d1b750da8a7eda1f8929ac9938f011ddb55b74274b46c`|`73fdc620e3f2e8a156621ad35b18d1ce9ce920497340a7a14d405334b3c02e8e`|
|Intel2|`a3bdb35b7467df262ca5b48753f6c778ffbedaf39b728d902b27224647bd75ae`|`cc17a27bdfceb09c51714babad93a7cff16524ec9264bbbca2cb1d0f1bd36ded`|

## Qualification after the first code checkpoint

On5e3e8f3d/fix1, seven writers plus seven peer MD5 crawlers complete14,000
fsync/close/rename files in387.707s, all workers PASS. After a private cold
restart, each host verifies all14,000 exact files PASS. The two-fsync omission
regression passes both directions (512B and8MiB+1); cold reads on both hosts
also verify every byte of both sparse fixtures, including holes. One private
node paused10s:128 fsync files and both peer/cold byte checks PASS.

Cross-host peer POSIX remains66/67: concurrent append exceeds its original15s
step bound. Fresh isolated four-node recovery/GC on each host completes physical
reclamation, zero quota/inventory, fencing and crash/replay gates; local peer
67/67 each. Intel2 core216/216 PASS; Intel1 core215/216 retains a new
`perm_ftruncate_after_chmod` EIO, so its aggregate is FAIL. Controllers preserve
that failure while completing independent later gates.

That EIO is reproduced and repaired in the
[newer-snapshot ownership follow-up](intel-report-newer-snapshot-20261014.md).
These results qualify the first code checkpoint; the follow-up records its
own exact-source reruns. Production remains untouched. This does not closeW100.
