# Intel receive persistence correctness — 2026-10-14

W100 / REPORT BUSY and native close/fsync EIO follow-up. **Native gates PASS;
private live qualification IN PROGRESS. Concurrent acceptance FAIL; W100 OPEN.**
Base devel `7659df951` already includes queue guard `1ac603128`; no merge is
required for that guard. ORCD branch changes are not merged or deployed here.

## Problem and correction

An ORCD observation found many host-mutex waiters while its owner was in
`fdatasync(raft.log)`. That later sample does not prove that a particular
8-second REPORT BUSY/EIO was caused by the sampled syscall. Intel qualification
must distinguish a reproduced native defect from that unproven causal link.

A receive owner now prepares and freezes its group, publishes stable state
without read authority, releases the host mutex for its synchronous store
operation, reacquires it and completes ACK/apply only after successful
persistence. Cohort commands, decoded frames, inbox credits, KV hold and TLS
deferral remain owned by the same pump stack. Stop joins that owner before free.
Other groups and handlers may acquire the host mutex during the wait. The pump
still waits for disk I/O; this is not an asynchronous worker redesign.

Ordinary follower append/conflict and hard-state fallback also require their
own persistence boundary even if an unrelated submitter owns a shared sync
hold. Rotation refuses BUSY while a sync pins the active log descriptor and
watermark. A failed sync fences the group without certifying its uncertain
suffix. No wire/storage format, queue/window, RPC timeout, retry budget,
compression, or operational feature policy changes.

## Reproduction and native evidence

Two Intel hosts, Linux/XFS; candidate source staged separately at
`/data1/efs/w100-private/receive-persist-candidate`, not running daemon source.
Build identity `7659df951-persist4` represents the accompanying uncommitted C
snapshot; the later frozen deployment commit must be recorded separately.
Full native build/test on both hosts: **PASS**:

```sh
make -j8 EFS_GIT_ID=7659df951-persist4 EFS_GIT_DESCRIBE=7659df951-persist4 EFS_GIT_BRANCH=devel all
env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_RAFT_FLOW=0 make -j8 EFS_GIT_ID=7659df951-persist4 EFS_GIT_DESCRIBE=7659df951-persist4 EFS_GIT_BRANCH=devel test
CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g' ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_receive_persist.py
CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g' ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_raft_sync_rotation.py
```

Targeted ASan/UBSan/leaks: **PASS both hosts**. Actual core + actual extracted
host callback test covers cohort, singleton, multi-entry, conflict and higher-term
hard-state paths, each success/error; real interloper mutex acquisition, other
group mutation, rejected frozen-group protocol/read authority and exact ACK/apply.
Real disk tests check ACK coverage while another sync hold remains outstanding,
crash at ACK, hard-state vote persistence and cold exact bytes. Actual wrapped
fdatasync tests pin rotation, preserve watermarks and fence an injected EIO.

Negative controls use base `7659df951` source against the new tests on Intel1:
old rotation fails its runtime BUSY assertion; old core fails physical durable
coverage for follower ACK/apply and the vote hard-state ACK. Both compile and
execute before failing, so these are meaningful regression controls. The old
core also fails the actual host mutex handoff assertion. Unacknowledged suffix
survival after process crash is allowed; no power-loss simulation is claimed.
Earlier native runs exposed test adapter omissions and expected barrier-count
changes; final full native and sanitizer runs above include their corrections.

Raw logs (not committed):
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-receive-persist-20261014`.
See `intel{1,2}_efs-persist4-{build,native,sanitizer}.log`,
`intel1-old-negative-controls.log` and `old-core-negative.log`.

## Private live acceptance plan — pending

Required profile: TCP/direct/files, .efs client/server OFF, packing OFF,
containers OFF, expiry OFF, flow default OFF. Existing four private nodes,
16 GiB each, two hosts, ports27432/27433, opposite-host main mounts. Preserve
all stores and production17432/17433 processes/mounts. Use
`scripts/intel_w100_private.py stop/resume merged-aa508c820 SOURCE --quota 16G`;
never mkfs or discard a busy mount. The currently installed source remains
`1ac603128` until the separately recorded deployment.

`tests/live/tiny_md5_publication.py --source PRIVATE_SOURCE --files 2000
--output OUT` creates seven fsync/close/rename writers and seven opposite-host
MD5 crawlers on two main FUSE clients. Ready names appear only after successful
fsync/close; crawlers verify immutable bytes and exact EOF. No app retries.
All14,000 fixtures are retained; `--role verify --relative FIXTURE --files 2000`
checks them after a cold private restart on both hosts. A14-file harness smoke
passed on old1ac; it does not qualify this correction or establish a speedup.

Still required: frozen baseline/candidate load, canonical seven-FUSE-client
concurrent POSIX, failure/recovery and two-fsync refusal/retry; live persistence
pause and graceful owner shutdown; cold payload verification; final normal
profile and production identity checks. Native fault coverage does not replace
these live gates. Existing queue-guard depth and shared-pwrite deadline FAILs
remain in [the queue report](intel-queue-guard-20261014.md).

## Frozen live correction checkpoint

Normal private deployment: `a58e93942` (core fix `a0daf0ae3`), built on
Intel1 at2026-10-14T10:24:27Z and Intel2 as recorded in version RPC artifacts.
The four tested native C/header SHA256s exactly match the frozen deployment.
Four16GiB private stores retained, TCP/direct/files; both .efs settings,
packing and expiry0; containers unused. Production processes unchanged.

Seven writers + seven peer MD5 crawlers,2,000 files/writer, same script SHA256
`ddedf7293f55d1763f6358781ed13d4548b8cdf06e4fc64baba4ce9234c0b0d1`:

| Source | Verdict | Files | Seconds | Retained fixture |
|---|---|---:|---:|---|
|7659df951 baseline|PASS|14,000|316.615|receive-load-6f4520b28e7b|
|a58e93942 correction|PASS|14,000|327.871|receive-load-af3884c49f2c|

No EIO reproduced in either run. Durations are evidence, not a performance
improvement claim; fresh prefixes/leader/cache/storage state differ. Neither
run proves the original ORCD EIO fixed or establishes its syscall cause.

Live persistence pause/shutdown: **PASS**, `tests/live/private_intel_receive_persist.py
--source /data1/efs/w100-private/devel-a58e93942 --fault-source
/data1/efs/w100-private/devel-a58e93942-fault --output OUT`.
Normal/fault binaries share identical C; only fault build enables the gate.
Select follower-only node3, stop through checked private PID/storage identity,
start fault binary with original arguments and the pause file, then independently
disarm after10seconds. Marker PID proves the callback entered after publishing
stable authority/releasing h->mu, before executing its actual persistence call.
RAFT_STATUS (source takes h->mu) completes in0.219724seconds end-to-end over SSH
while gate remainsPAUSE. SIGTERM retirement is still waiting/process alive
500ms later whilePAUSE; timer release lets shutdown complete and normal node3
restart. The same pump retains ownership until join; this is not a power-loss
or an unmodified hardware-stall experiment.128 files/16workers fsync oracle
passes in1.537seconds. This fault run overlapped standalone GC fixtures on the
hosts, after the seven-client run had finished. First controller launch failed
with a Python f-string NameError before any signal; corrected and rerun, raw
first log retained. No daemon correctness failure is hidden by that repair.

Seven-client canonical concurrent POSIX: **FAIL,1475/1512**, seven FUSE clients
(4+3),16jobs/client, original30s/case,900s outer bound, unique persista58 tags.
All five extra clients drain/retire. Failures: names_crazy_dirs7,
concurrent_appends_two_proc7, mtime_monotonic_many_writes7,
dir_deep_nesting4, concurrent_creates_same_dir4,
dir_deep_nesting_beyond_64=3, concurrent_create_unlink_two_proc3,
names_crazy_roundtrip1 (all timeout), unlink_recreate_new_ino1 (EBUSY).
No budget/assertion/application retry changed. No matched7659 seven-client
baseline has yet been run; older1ac observations do not establish a causal
regression/improvement comparison. W100 and loaded acceptance remain OPEN.

Thirty-second read-only /proc queue samples during that concurrent run:
pending ordered frames/group/peer never exceed1 across all four nodes
(node2/node3 observed maxima0); max nent1/peer;
flow remains0. Sampler uses binary-only GDB type inspection, never ptrace
attach. Queue bounds do not guarantee successful bounded-latency POSIX.
Full recovery/GC, two-fsync and final cold verification still pending.
