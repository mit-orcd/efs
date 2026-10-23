# Intel receive persistence correctness — 2026-10-14

**Correctness/failure gates PASS; loaded acceptance FAIL; W100 OPEN.**
Follow-up to [W100](w100-client-retry-eof-20261013.md) and the
[queue guard](intel-queue-guard-20261014.md). The queue guard `1ac603128`
already belongs to devel; no prerequisite merge was needed. This change does
not merge or deploy ORCD branch changes, touch ORCD, or change production Intel.

## Source and correction

Base: devel `7659df951`. Core fix:
`a0daf0ae3db1b0bb6ef72f88e8fcd0d7c92257ee`. Frozen normal private deployment:
`a58e9394231e5780cb0e3c2c8f98517298683c1e`; controller repair/evidence:
`b8e50f7ae` (no C/header changes). Version RPC confirms a58e93942/devel,
both hosts built2026-10-14T10:24:27Z. The four modified native-tested C/header hashes match that
frozen deployment. No push or rolling upgrade is claimed.

A later ORCD sample found host-mutex waiters while its owner was in
`fdatasync(raft.log)`. It does not prove that a particular earlier8-second
REPORT BUSY/native close/fsync EIO was caused by that sampled syscall. Neither
Intel writer/checksum run below reproduces the original EIO.

The receive owner freezes its group and publishes stable state with no read
authority before releasing the host mutex around its synchronous store call.
It reacquires serialization before ACK/apply, requiring successful persistence
and unchanged ownership/term/boot. Cohort buffers, inbox credits, KV hold and
TLS deferral stay on that pump stack; shutdown joins the owner before free.
Other handlers/groups can acquire the host mutex. The pump itself still waits
for disk I/O; this is not an asynchronous worker redesign. Leader/election,
configuration and snapshot paths retain their existing serialization.

Ordinary follower append/conflict and hard-state fallback now require their
own persistence boundary even while another submitter owns a shared sync hold.
Log rotation refuses BUSY while a sync pins the active descriptor/watermark;
automatic rotation waits for a later append. Failed sync fences without
certifying its uncertain suffix. No wire/storage format, queue/window,
RPC deadline, retry budget, compression or operational feature policy changes.

## Environment and results

Private Intel1 .207 nodes1/2 and Intel2 .209 nodes3/4, ports27432/27433,
two16GiB stores/host under `/data{1,2}/efs/w100-private-merged-aa508c820/nN`.
Runtime `/data1/efs/w100-private/runtime-merged-aa508c820`; opposite-host main
mounts at runtime `/mnt`. TCP, direct I/O, files storage, .efs client/server0, expiry0, flow default0. Root commands
policy version1/mask0; synthetic .efs absent on both mounts. No quota increases.
Standalone recovery fixtures use four loopback nodes/host and4GiB/node.

| Gate | Verdict / evidence |
|---|---|
|Full native build + `make test`|PASS both Intel hosts|
|Targeted ASan/UBSan/leaks|PASS both hosts: actual core/host callback and disk sync/rotation|
|Old-source negative controls|Old7659 core fails durable ACK/apply and vote-boundary checks; old rotation fails runtime BUSY assertion; old core fails actual host mutex handoff|
|Seven writers + seven peer MD5 crawlers, baseline7659|PASS14,000 files;316.615s|
|Same workload, a58 correction|PASS14,000 files;327.871s|
|Live persistence pause + graceful retained-owner shutdown|PASS; h->mu-taking RAFT_STATUS returns0.219724s over SSH while paused;128fsync files/16workers PASS|
|Canonical seven-FUSE-client concurrent POSIX|**FAIL1475/1512**,36original30s timeouts and1EBUSY|
|Read-only queue sample during concurrent run|300samples/node; pending ordered frames/group/peer never exceed1; flow0|
|Full standalone recovery/GC|PASS both hosts; core216/216 and local peer67/67 each, guarded leases, opid, integrity, namespace, allocation, tiny churn, quota-full reroute, crash/replay, zero inventory/quota and late-PUT fencing|
|Two-fsync omission/refusal/retry|PASS both host directions;512B and8MiB+1, peer EOF0 for both refused fsyncs, exact recovered EOF/checked bytes after clearing fault|
|One owned private daemon paused10s|PASS128fsync files, both peers exact bytes; cold checks also PASS|
|Cold tiny data after all four private daemons restart|PASS28,000 retained files on each host (baseline14k + correction14k), exact EOF/bytes/MD5|
|Existing persistence cases after restart|PASS27/27 per host|
|Final real cross-host peer suite|**FAIL66/67**: concurrent append SSH step timeout15s; shared-pwrite passes|

Seven-client failure distribution: names_crazy_dirs7,
concurrent_appends_two_proc7, mtime_monotonic_many_writes7,
dir_deep_nesting4, concurrent_creates_same_dir4,
dir_deep_nesting_beyond_64=3, concurrent_create_unlink_two_proc3,
names_crazy_roundtrip1 (timeouts); unlink_recreate_new_ino1 (EBUSY).
No matched7659 seven-client baseline was run; older1ac results do not establish
causal improvement/regression. Correctness evidence does not close these gates.

Final main peer run lasts67.2s. `peer_concurrent_append`, ab-a step1, rc124,
SSH timeout15s; shared-pwrite passes (ab step1=10.09s). The earlier runner
incorrectly created only a parent and omitted `--prepare`, producing missing
test directories/0of67. That invalid setup is retained separately and makes
no filesystem-failure claim; the valid run uses all67 prepared directories
and remounts B before testing. No original deadline/assertion was changed.

## Reproduction

Prerequisites: Intel SSH aliases, existing authorized private firewall ports,
compiler/make/FUSE3/verbs development packages, healthy owned fixture; preserve
production17432/17433 processes and `/data1/efs/mnt`. Stage separate normal and
EFS_FAULTS builds from the frozen source; never relink a running image.
Use a full checkout's committed fixtures for native tests:

```sh
make -j8 EFS_GIT_ID=7659df951-persist4 EFS_GIT_DESCRIBE=7659df951-persist4 EFS_GIT_BRANCH=devel all
env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0 EFS_RAFT_FLOW=0 make -j8 EFS_GIT_ID=7659df951-persist4 EFS_GIT_DESCRIBE=7659df951-persist4 EFS_GIT_BRANCH=devel test
CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g' ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_receive_persist.py
CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g' ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_raft_sync_rotation.py
```

`persist4` is the tested precommit build stamp; its C/header contents match
a58 exactly (`tested-core-sha256.json`). The receive regression uses actual
core + extracted actual host callback with typed publication adapters. It
stalls cohort, singleton, multi-entry, conflict and higher-term hard-state
paths, each success/error; real mutex interloper and other-group progress,
blocked frozen-group mutations/read authority, exact ACK/apply. Actual disk
store tests assert durable coverage with another sync hold outstanding,
crash at ACK, vote persistence and cold bytes. Linker-wrapped actual fdatasync
pins rotation and injects EIO without advancing its durable watermark.
Negative controls substitute7659 core/log source via `EFS_RAFT_CORE_SOURCE`
and `EFS_RAFT_LOG_SOURCE`; real `test_raft_store.c` is also linked against the
old core. These compile and execute before assertion failures, not merely
compile-negative tests. Linux old-source controls from the new test checkout:

```sh
git show 7659df951:src/raft/raft.c > /tmp/efs-old-raft.c
git show 7659df951:src/raft/raft_log.c > /tmp/efs-old-raft-log.c
EFS_RAFT_CORE_SOURCE=/tmp/efs-old-raft.c python3 tests/test_receive_persist.py
EFS_RAFT_LOG_SOURCE=/tmp/efs-old-raft-log.c python3 tests/test_raft_sync_rotation.py
gcc -std=c99 -D_GNU_SOURCE -O1 -Iinclude -Isrc/raft -Ideps/blake3 -ffunction-sections -fdata-sections tests/test_raft_store.c /tmp/efs-old-raft.c src/raft/raft_mem.c src/raft/raft_disk.c src/raft/raft_log.c src/common/checksum.c src/common/batch_obs.c -Wl,--gc-sections -pthread -o /tmp/efs-old-core-store
/tmp/efs-old-core-store
```

Each control is expected to exit nonzero at its asserted regression, not to
pass. Unacknowledged suffix survival after process crash is allowed; no power-loss simulation is claimed. Intermediate test adapters
were corrected before the final full native/sanitizer runs.

Owned lifecycle (locally), preserving stores, no mkfs or busy-mount discard:

```sh
python3 scripts/intel_w100_private.py stop merged-aa508c820 CURRENT_SOURCE_LABEL
python3 scripts/intel_w100_private.py resume merged-aa508c820 NEW_SOURCE_LABEL --quota 16G
python3 tests/live/tiny_md5_publication.py --source /data1/efs/w100-private/devel-a58e93942 --files 2000 --output OUT
python3 tests/live/private_intel_receive_persist.py --source /data1/efs/w100-private/devel-a58e93942 --fault-source /data1/efs/w100-private/devel-a58e93942-fault --output OUT
python3 scripts/intel_package3_tests.py concurrent --run-id UNIQUE --output OUT --source-root /data1/efs/w100-private/devel-a58e93942 --mount /data1/efs/w100-private/runtime-merged-aa508c820/mnt --seed-port 27432 --remote-output-root /data1/efs/w100-private/results --extra-mount-root /data1/efs/w100-private/extra-mounts
python3 scripts/intel_queue_sample.py --source /data1/efs/w100-private/devel-a58e93942 --seconds 30 --output OUT.json
```

Writer harness:7writers/7peer crawlers total on two main FUSE clients,128B files,
O_EXCL.part/write/fsync/close/rename.ready; immutable ready files only are
checksummed. No app retries,1800s workload bound. This differs from the
separate seven-FUSE-client POSIX topology (4+3 clients,16jobs/client,
30s/case,900s outer). Retain all writer fixtures. Cold oracle on **each** host:

```sh
python3 tests/live/tiny_md5_publication.py --role verify --relative receive-load-6f4520b28e7b --files 2000
python3 tests/live/tiny_md5_publication.py --role verify --relative receive-load-af3884c49f2c --files 2000
python3 tests/posix/posix_persist.py /data1/efs/w100-private/runtime-merged-aa508c820/mnt --phase verify --tag merged --results OUT.tsv --keep
```

New runs use their generated fixture names instead. Cold tiny oracles used
300s outer bounds per fixture; persistence verifies original case budgets.
Writer script SHA256 in both runs:
`ddedf7293f55d1763f6358781ed13d4548b8cdf06e4fc64baba4ce9234c0b0d1`.
Durations are evidence, not a performance claim; leader/cache/store state differ.

Live persistence controller selects follower-only node3, verifies PID/exe/UID/
private storage, preserves original arguments, and gates the actual callback
before persistence after h->mu release. An independent remote10s timer clears
PAUSE. Management completes while PAUSE still holds;500ms after SIGTERM the
process remains alive and retirement waits. Timer release permits real store
completion and shutdown, then normal source restores. Run overlapped separate
GC fixtures after concurrent POSIX finished. First controller launch had a
Python f-string NameError before any signal; b8 repairs it and the repeated
fault gate PASS is `persistence-pause-fixed`, not the initial failed runner.
The raw aggregate `live-summary.json` retains that initial runner rc1.

Full failure suite on each host, normal frozen build:

```sh
env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0 EFS_RAFT_FLOW=0 timeout 1800 python3 tests/live/gc_reclamation.py --mode direct --transport tcp --disable-efs-commands --fixture-quota 4G --posix --posix-jobs 16 --continue-posix-failure --guarded-leases --opid --integrity --namespace --allocation --tiny-churn --full-node
```

Two-fsync uses temporary opposite-host mounts at runtime `/mnt-eof` from
EFS_FAULTS clients, `EFS_FAULT_REPORT_SIZE_FILE=runtime/omit-eof` and all disabled
feature settings. Run `tests/live/report_eof_contract.py MOUNT FAULT --expect
refused --peer OTHER_HOST` sequentially both directions; resetOFF and drain/
stop both fault mounts in finally. Sparse validation checks anchors/EOF, not
every hole byte. No fault client or enabled fault remains running.

`tests/live/private_intel_queue_pause.py --source PRIVATE_SOURCE` pauses only
verified owned node3 through pidfd, independently resumes after10s, writes
64files/host and verifies128files from both peers. Retained fixture:
`queue-pause-19cb98749aa3`; `--verify FULL_FIXTURE_PATH` repeats after cold start.

Final cross-host peer: on Intel1 run `tests/posix/posix_2client.py --prepare
MOUNT --parent UNIQUE`; drain/remount private B with the same profile. Then
`tests/posix/posix_2client.py --remote intel1_efs intel2_efs --mnt MOUNT --parent
UNIQUE --ssh /data1/efs/w100-private/peer-ssh --results OUT.tsv`. The existing
wrapper/step budget is15s. Final valid parent `persista58-peer-prepared` retained.

## Final state and limits

Final normal private daemons: Intel1 n1=3537538,n2=3537539; Intel2
n3=3803745,n4=3803746. Main FUSE Intel1=3537778,Intel2=3804544. Both .efs
entries absent, commands policy mask0. Extra concurrent/fault clients and
standalone GC daemons all stopped; no workload/controller remains. Fault
gates resetOFF. Production PID/start-time/executable SHA256/arguments and
mounts are unchanged from the baseline inventory.

Normal binary SHA256:

| Host | efsd | efs-fuse |
|---|---|---|
|Intel1|6b587c66b009096e70ffdcf51c885436138399d901b90bddb92cd426f0d4e663|3a0351440efc6790d4645879dbce043fb0b289c23ff1f224af7910c957cd985d|
|Intel2|665cfac035000e87b1a50cf52e75502836d56d0e3644e24bc678a15dfec43c30|d8404fc31721fa3fb0300f9c0946a17a0ceaaf7db9fbaf4b86d96fd2a6feb3fb|

Raw artifacts outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-receive-persist-20261014`.
Native/sanitizer logs `intel{1,2}_efs-persist4-*`; negative controls
`intel1-old-negative-controls.log`, `old-core-negative.log`; baseline/correction
writer directories; `persistence-pause-fixed`; concurrent logs/TSVs and queue
sample; `intel{1,2}_efs-full-gc.log`, two-fsync and cold logs;
`cross-peer-prepared.log/.tsv`, initial invalid runner logs; final process JSONs.
Remote GC roots `/data1/efs/evidence/gc-direct-atg56qow` (Intel1) and
`/data1/efs/evidence/gc-direct-5a6ecapa` (Intel2); data2 companion roots are in manifests.

**Acceptance remains blocked:** reproduce/resolve loaded EBUSY and bounded
operation timeouts without relaxing namespace/read authority, durable ACKs,
queue/window or deadlines; then qualify affected changes on Intel and the
original ORCD load separately. ORCD still needs its own stopped-cluster source
integration/acceptance. This correction neither proves an original EIO cause
nor removes every possible host-lock I/O wait; no speedup is established.
