# Private Intel older REPORT / newer PUT ownership — 2026-10-14

**Private Intel TCP/direct I/O qualification PASS on the listed gates;
W100 CLOSED by human acceptance on 2026-10-14.** Follow-up to
[operation-ID admission and span REPORT ownership](intel-opid-report-correctness-20261014.md).
Fix commit `28df9c7ebda450ff30261bce6514f0bb62f3f712` adds the ownership guard and regressions; parent
`5e3e8f3d260e86e9bb4d5c9f94c593f959292687` contains the preceding corrections.

## Failure and cause

During fresh isolated recovery/GC qualification of5e3e8f3d, Intel1 core failed
`perm_ftruncate_after_chmod` with EIO (215/216); Intel2 core216/216 passed.
Both isolated peer suites67/67 and subsequent fencing, physical reclamation,
restart, zero-quota and empty-inventory checks completed. Intel1's aggregate
stays FAIL; its controller continued independent gates and re-raised the original
POSIX failure. This is a stale-generation EIO, not proof of the original ORCD
REPORT BUSY EIO's cause.

A retained-file probe reproduced ftruncate EIO on each physical host:
2/500 cases each,16 workers,15.773/13.284 seconds. Diagnostic Intel1 client
with dcache tracing reproduced192/500 failures in43.711 seconds. Tracing
changes interleaving; these counts/durations are not a speed comparison.

The diagnostic trace for inode98441 shows:

- 14:59:44.161Z: original eight-byte PUT recorded at snapshot1.
- 14:59:44.168Z: newer full truncate-tail snapshot4 clears `dirty`, while
 `object_seq` still names snapshot1's completed PUT.
- 14:59:44.172Z: the older REPORT succeeds. Completion checks the old
 object identity plus `!dirty`, then clears ranges/pin and frees the body.
- 14:59:44.175Z: snapshot4's PUT completes, but its cache body has gone.
 The STALE classifier repeatedly reports an uncommitted object; replay has
  no body and cannot rematerialize it. The application receives EIO.

`dcache_note_committed`'s completion ownership test was unchanged by5e3e8f3d;
this is a separate pre-existing gap exposed by further qualification. A snapshot
clears dirty before its PUT finishes. Matching the last completed object is
therefore insufficient authority to release the current body.

The correction releases ranges, eviction pin and body only if the matching
REPORT also covers the newest captured snapshot (`snap_seq <= object_seq`).
The older publication may be acknowledged while the newer snapshot remains
owned. No receipt-window, retry/deadline, quota, storage-format or compression
change is involved. Committing the newer snapshot releases its body normally.

## Source, profile and targeted qualification

Parent5e3e8f3d plus the small `write.c` guard, extended
`tests/test_client_memory.py`, and `tests/live/chmod_truncate.py`.
Build label `5e3e8f3d-fix2`, frozen directory
`/data1/efs/w100-private/devel-opid-fix2` on both hosts. Normal GCC
`-O3 -g -fno-omit-frame-pointer -march=native -mtune=native` builds.

Same four-node private two-host Intel fixture/16 GiB per-node quotas as the
linked report: ports27432/27433/direct I/O, TCP;
`.efs` client/server and expiry disabled. The corrected
four daemons/two main clients reuse the same stores; production is untouched.
Normal live trace is optional operation-ID logging; a separate owned client
uses dcache tracing. Diagnostic clients are drained after their workloads.

The memory regression executes the production completion function against a
newer in-flight snapshot with the older object still installed. It requires the
new body's bytes, ranges and eviction pin to survive the old ACK and cache
reclamation, excludes another late old ACK after the newer PUT lands, then
requires exact newer ACK cleanup. Normal and ASan/UBSan/leak runs PASS on both
Intel hosts. Selecting previous production source compiles and executes, then
fails precisely at the retained-body assertion on both hosts.

Mounted fixed runs (all16 workers, no application retries):
Intel1 1000/1000 and Intel2 1000/1000 PASS in29.455/29.778 seconds;
separate traced Intel1 client500/500 PASS in16.631 seconds. These overlap
native qualification and each other; they are correctness/load evidence, not
quiet performance measurements. Every accepted file has exact EOF3 and `ABC`.

## Reproduction

Existing private source/build/SSH/FUSE prerequisites from the linked report.
Preserve original limits; do not replace a failed verdict with a passing repeat.
From each candidate source tree on Intel:

```sh
python3 tests/test_client_memory.py
ASAN_OPTIONS=detect_leaks=1 python3 tests/test_client_memory.py --sanitize
EFS_CLIENT_MEMORY_WRITE_SOURCE=/old/src/client/write.c python3 tests/test_client_memory.py
python3 tests/live/chmod_truncate.py /PRIVATE/MOUNT --files 1000 --workers 16
```

The old-source control must execute and fail its assertion, not fail compilation.
The mounted probe keeps every attempted file. Its sequence is exclusive create,
write `ABCDEFGH`, chmod0444, ftruncate3 through the existing descriptor, close,
chmod0644, exact stat/read. On failure it records the phase and close error;
it never retries an application operation or closes a recycled descriptor.
For a successful fixture after a cold restart:

```sh
python3 tests/live/chmod_truncate.py /PRIVATE/MOUNT --files 1000 --workers 16 --verify /PRIVATE/MOUNT/chmod-truncate-UNIQUE
```

## Evidence and baseline cleanup

Raw output outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-opid-report-20261014/`.
Old native control logs are `newer-snapshot-old-negative-intel{1,2}_efs.log`;
trace is `chmod-dcache-trace.log`; mounted outcomes are
`chmod-truncate-{probe,fixed}*`; source archive is `fix2-source.tar`.
Original isolated fixtures: Intel1 `/data1/efs/gc-direct-n9n_zi12`,
Intel2 `/data1/efs/gc-direct-5e8lx9gp` (all owned processes stopped).

The old diagnostic client correctly refused drain because the demonstrated
missing bodies left unresolved publications. Before cleanup, an independent
fixed client saved **all1500 baseline probe files**, full published bytes,
inode/mode/size/SHA256 manifest, outside EFS at Intel1
`/data1/efs/w100-private/results/chmod-baseline-saved-20261014`.
Only those three uniquely owned probe directories were then unlinked normally;
the old clients drained cleanly. No force-discard, mkfs, production shutdown
or removal of other fixtures was used. Failed published EOFs/bytes remain in
that external archive for inspection.

## Follow-up qualification (same candidate)

Full native `make test` PASS on both hosts. First seven-client core1512/1512
PASS. All42,393 traced identities complete, none outstanding at collection,
maximum observed lifetime0.780s; no final admission BUSY, invalid REPORT,
pub-reject, stale-generation error or stalled-publication event in that run's
bounded client/daemon logs. This does not measure every transient local refusal.

Fresh independent four-node fixtures on each Intel host (loopback TCP/direct,
4 GiB per-node quotas, disabled optional features) PASS their complete combined
recovery/GC controller: core216/216, local peer67/67 each, guarded ownership,
operation IDs, integrity, namespace, allocation, tiny churn, full-node reroute,
physical reclamation, fencing and crash/replay. Both aggregate exits0.
Raw fixtures: Intel1 `/data1/efs/gc-direct-7u96onob`, Intel2
`/data1/efs/gc-direct-3dxcums1`. These are separate local fixtures, not an
additional physical cross-host peer qualification.

Final fault clients (`5e3e8f3d-fault2`, EFS_FAULTS=1, separate build trees)
PASS two rejected fsyncs followed by exact recovered EOF in both directions,
512B and8MiB+1. Fault controls resetOFF and clients drain/stop in finally.
A verified private node3 paused10s passes128 fsync files, both peer byte checks;
maximum write-case4.055s. Original30s operation bound retained. Fault/pause checks
overlap independent standalone recovery/GC, not a performance measurement.

Source/input manifest (208 tracked src/include/deps C/header/assembly and
Makefile inputs), SHA256
`92f8264ea2c826ab689964fb2b69a59df621aca1d767c1a07cde066cd119f500`:
zero mismatches against28df9c7eb and both normal/fault source trees on both
hosts. Normal binary SHA256:

|Host|efsd|efs-fuse|
|---|---|---|
|Intel1|`a586849a0590d3c18abc972d9687bc68d589b9fb811f3dce48629a04650a507f`|`06a880258bc772a4a658fe8f09ef6380905151b1cc95104503a7562beeb5701b`|
|Intel2|`351c37992da12e407abad81283be7e665a9633264c6cdb0f43aeccbc11030348`|`3ebf5ba676da9e44785f3f3e7db18cb597ad85a1cb0b1a8a718b081f32448f7b`|

Commands for broader reproduction use the controllers in the linked report,
substituting `devel-opid-fix2` and fresh run IDs. On each Intel host, the isolated
combined controller is:

```sh
env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0 EFS_RAFT_FLOW=0 timeout --kill-after=10s 1800s python3 tests/live/gc_reclamation.py --mode direct --transport tcp --disable-efs-commands --fixture-quota 4G --posix --posix-jobs 16 --continue-posix-failure --guarded-leases --opid --integrity --namespace --allocation --tiny-churn --full-node
```

The pause controller now accepts the owned runtime explicitly and validates its
node3 storage before pidfd signalling:

```sh
python3 tests/live/private_intel_queue_pause.py --source /data1/efs/w100-private/devel-opid-fix2 --runtime /data1/efs/w100-private/runtime-deadline-posix
```

It independently resumes after10s, including exception paths. Never substitute
an installed production runtime. Retain byte fixtures for cold verification.

## Final Intel verdict and state

|Gate on28df9c7eb production inputs|Verdict|
|---|---|
|Full native suites, both hosts|PASS|
|Targeted normal + ASan/UBSan/leaks and executing old-source controls|PASS; old controls fail at the intended assertions|
|First seven-client cohort|1512/1512 PASS|
|Three further cohorts on the same mounted clients|4536/4536 PASS; all21 TSVs complete|
|Final physical cross-host peer suite|67/67 PASS; concurrent append step5.17s within unchanged15s limit|
|Fresh isolated combined recovery/GC, each host|PASS; core216/216 and local peer67/67 each, all subsequent gates/cleanup complete|
|Mounted chmod/ftruncate oracle|2500/2500 PASS; cold2500 exact `ABC`/EOF3 files verified from each host|
|Two-fsync refusal/recovery, both directions|PASS|
|Owned daemon10s pause,128 fsync files and peer/cold bytes|PASS|
|Final cold earlier tiny/MD5 fixture|14,000/14,000 exact files verified from each host|
|Cold small/sparse EOF fixtures|8 files/host, including every hole byte, PASS|
|Optional extras, each host|1 PASS,4 SKIP,0 FAIL: mmap-private passes; shared mmap ENODEV and three disabled .efs checks skip|

There are28 complete TSVs across the final four concurrent rounds:6048/6048
PASS. The first cohort uses one pool; five extra clients are then created for
three further same-mounted-client rounds (the two main clients persist).
Seven mounts are split4/3 across the two physical hosts,16 core jobs each,
unchanged30s minimum case budgets/longer decorators and900s suite outer bound.
No application retries, dropped failures or deadline changes were introduced.
Earlier5e3e8f3d failures remain recorded in the linked report.

The post-stability diagnostic window contains127,236 completed traced operation
IDs, none still outstanding at collection, maximum1.231s; this window also
includes the subsequent peer fixture preparation. Both bounded final captures
have zero final operation-ID admission BUSY, invalid REPORT, pub-reject,
report-loop exhaustion, stale-generation error or
stalled-publication events. Transient local admission retries are not counted.
These are observations of the captured windows, not a guarantee of every future
workload's latency or an isolation of the cause of every historical timeout.

Peer setup initially hit a transient flock refusal immediately after clean
client retirement. Inspection found no remaining holder and the mount was down;
only the launcher was retried, before any peer case ran. No stale lock was
removed or forcibly unlocked. The versioned
[peer runner](../../tests/live/intel_peer_ssh.sh) is byte-for-byte the wrapper
used in the passing suite (SHA256
`63ee383d295e21d0f089f0265aaff40da1e9476918ddb5310401ff24d67f3e05`).
It runs **on Intel1**, executes its local alias directly, and SSHes to Intel2,
with the original15s step timeout. Reproduction after preparing UNIQUE on A
and draining/remounting B with the same profile:

```sh
# From the Mac checkout, install the versioned wrapper on Intel1:
scp tests/live/intel_peer_ssh.sh intel1_efs:/data1/efs/w100-private/peer-ssh
# From Intel1's candidate source, prepare then drain/remount B:
python3 tests/posix/posix_2client.py --prepare /data1/efs/w100-private/runtime-deadline-posix/mnt --parent UNIQUE
# After B has been remounted with the same profile:
python3 tests/posix/posix_2client.py --remote intel1_efs intel2_efs --mnt /data1/efs/w100-private/runtime-deadline-posix/mnt --parent UNIQUE --ssh /data1/efs/w100-private/peer-ssh --results /data1/efs/w100-private/results/UNIQUE.tsv
```

Final normal private restart reuses every store, disables diagnostic trace
logging, and resets fault controlsOFF. From each host:

```sh
python3 tests/live/tiny_md5_publication.py --role verify --mount /data1/efs/w100-private/runtime-deadline-posix/mnt --relative receive-load-80b7f8e33686 --files 2000
```

This14,000-file fixture was written/MD5-qualified on5e3e8f3d, then cold-verified
again on28df9c7eb; the14,000-file writer run was not repeated on28df9c7eb.
The2500 chmod/truncate files were written and cold-verified on28df9c7eb.
Their retained roots are `chmod-truncate-bpdzi2rd` (1000),
`chmod-truncate-79rgs25x` (1000), `chmod-truncate-1ywux5sa` (500), under the
main export. The last originally used the diagnostic mount; verify it through
the main mount after restart. The pause fixture is `queue-pause-6bf1f2fc8e86`.
Full cold sparse checks include both earlier and final two-fsync fixtures:
`w100-eof-_3jkyadu`, `w100-eof-ysjp6dlf`, `w100-eof-oqwou_d4`, `w100-eof-81ead89f`. For each sparse file, verify length8MiB+1 and the
entire zero-filled image with `L` at1MiB and `H` at8MiB; small files are512 `S`
bytes. Cold oracles retain300s tiny/120s per chmod-fixture bounds.

Final processes:

|Host|Private daemons|Main FUSE|
|---|---|---|
|Intel1|3712434,3712435|3712682|
|Intel2|3941875,3941876|3941974|

Only these private processes/mounts remain, normal binaries with the hashes
above; TCP/direct/files,16 GiB quotas, optional features OFF, .efs absent and
command policy mask0. Diagnostic/extra/fault clients, standalone fixtures and
test controllers are stopped. `/proc` snapshots verify flags and absent trace
settings; production PIDs/executable hashes/arguments/mounts are unchanged from
the pre-deployment inventory. No production redeployment occurred.

The tested Intel matrix now passes. **W100 is CLOSED by human acceptance on
2026-10-14.** The user accepts this qualification and directs that a future
stall encountered through another path or workload be tracked as a new issue.
The original ORCD REPORT BUSY/EIO and broader sustained/profile acceptance are
limitations of this evidence, not outstanding W100 closure gates. No ORCD,
RDMA, buffered-I/O, optional-feature, universal timeout repair or speedup claim
is made here. Existing failed checkpoints are preserved, not relabelled PASS.
