# Intel directory sync and metadata publication batching — 2026-10-14 EDT

**Implemented and deployed; native/sanitizer and four-client smoke PASS.**
Observed median AMD dd/fsync throughput improves 28.0%; Intel1 improves 24.7%.
These are three-trial before/after deployment measurements, not isolated
code-only attribution or full filesystem acceptance.

Follow-up to [AMD dd diagnosis](archive/intel-amd-dd-profile-20261014.md).
Scope: ordinary files, TCP/direct-I/O, `.efs` client/server and
expiry disabled. Full POSIX/concurrent/crash acceptance is not
claimed by this smoke/performance task.

## Implementation and durability boundary

Source `2256f1a34fc1006e3ecb9ab356a46a0341f0b605` on devel; build ID
`2256f1a34fc1`, version `v0.3.2-pre-alpha-468-g2256f1a34`, common build timestamp
`2026-10-15T02:57:30Z`. Baseline production source
`c775fb15e2530399e052cf48432d6e495a039fb0`.

- `src/server/directory_sync_group.h` groups already queued directory barriers
  for the same pinned `(device,inode)` identity. A leader detaches the covered
  set **before** invoking fsync. Arrivals during the barrier require a subsequent
  barrier. All covered waiters receive the barrier's actual result, including
  failures. Every caller keeps its descriptor open; cancellation is masked
  while its stack-owned waiter is linked. Fixed 64-bucket scheduling state,
  no per-request heap allocation and no cached successful durability outcome.
  Distinct inode identities sharing a hash bucket never share a barrier.
- Only the initialized parent-sync path uses this grouping.
  Unknown ancestry still gets its existing proof/barriers. GC
  callback accounting, and startup ancestry sealing retain their paths.
  **Every fragment still receives its own file fsync before entering the
  directory group.** No acknowledgement skips data persistence or namespace
  persistence; the storage format and transport protocol do not change.
- Background REPORT kicks remain queued for a fixed **10 ms** coalescing window
  from the first kick, measured with a monotonic condition clock. Repeated kicks
  dedupe without extending the deadline. Worker timeout handling rechecks the
  current deadline under the mutex; an expired earlier batch cannot release a
  newly queued batch. Forced fsync/close and pressure drains bypass this delay.
- Remove the lifetime stripe-mark counter from the whole-set flush trigger.
  It was never reset by snapshots and eventually made every write trigger a
  whole-set REPORT. The existing snapshot-owned operation count controls that
  threshold and is restored on failed publication. Retained writes, EOF guards,
  admission bounds, forced publication, and quorum requirements are unchanged.

Distinct fragment files cannot safely share a file fsync merely because they
share a directory. This change intentionally shares only barriers whose
coverage can be established without changing the persistent representation.

## Focused qualification

Native build and `test_directory_sync_group`, `test_store_durability`, and
`test_meta_flush_batch.py` pass on Intel1, Intel2, AMD and physical xorinox.
The directory-group regression checks actual queued requests against controlled
barriers: ten requests require three barriers, including one failed batch;
late arrivals cannot inherit the earlier result, failure fans out correctly,
a fresh retry performs a fresh sync, and colliding directory identities stay
separate. The production store regression checks buffered/direct/alignment
paths, data-before-directory barriers, EINTR, file/directory/close failures,
unknown ancestry, restart/recreated directories, and fault refusal.

ASan+UBSan with leak checking passes for the actual group helper on Intel.
The four-worker metadata queue harness executes the production queue/worker
and trigger functions, testing duplicate kicks, fixed deadlines, queue overflow,
shutdown, foreground forced publication and threshold reset. Its initial
repetition exposed the old-deadline/new-batch race in the candidate; after the
fix it passed 20 sanitizer-enabled repetitions before release.
Affected two-fsync EOF, REPORT pressure and explicit-zero/non-rounded EOF
regressions also pass. These are native fault/ordering checks, not a power-loss
qualification of the entire running cluster.

## Deployment and measured result

All four main daemons and all four Intel-export clients use the exact release
above, verified through executable paths/hashes and process environments.
Each Intel host still runs two 400 GiB quota nodes on its original data roots.
Main mounts are `/data1/efs/mnt` on Intel1/Intel2 and
`/data1/efs/intel-mnt` on AMD/physical xorinox. External client code remains under
`/data1/efs/intel-client`, independent of any host-local cluster deployment.
Private Intel processes, executables and configurations match the pre-rollout
inventory exactly. No data stores were reformatted or cleared.

During preparation AMD's `/data1` filesystem disappeared temporarily, removing
its apparent client directory and stopping its existing clients/dd. The user
restored the mount; the original client/source files returned. A staging
attempt during the outage failed before building anything. The restored
baseline client was mounted and measured before deployment. This task did not
restart the unbounded human dd command or recreate a local AMD cluster.

The xorinox MD5 crawl was gracefully paused for the quiet comparison and
restart. Its earlier output is preserved. It restarted afterward as PID722011
with a fresh `md5-sync-batch-2256-20261014.jsonl` under the independent client
logs; initial check: 3,211 hashes, zero errors. There is no claim that restarting
a crawl resumes its exact traversal position.

All four clients pass cross-client reads of all four smoke files after their
fsync/shrink/regrow/EOF/mtime operations. All **64 preserved controls per client**
match the pre-rollout bytes, sizes and inode identities. All twelve timed
512 MiB trials below passed full payload-hash verification (6 GiB total), and
their generated output files were removed after verification.

|Client|Baseline trials, MiB/s|Candidate trials, MiB/s|Median before → after|Change|
|---|---|---|---|---|
|AMD → Intel|61.30, 64.82, 61.44|78.68, 79.93, 76.54|61.44 → 78.68 MiB/s|+28.0%|
|Intel1 mount|43.91, 57.80, 63.34|74.11, 70.43, 72.07|57.80 → 72.07 MiB/s|+24.7%|

The timed command includes final fsync and process exit; hash reads are outside
that interval. Baseline and candidate use the identical local nonzero fixture
on each host. Source generation is outside the interval. This is deliberately
different from the earlier live `/dev/urandom` run with a competing crawl;
compare only the paired rows here, not 34 MiB/s live versus 79 MiB/s quiet.
`O_DIRECT` is the daemon storage setting; application dd uses ordinary writes
plus `conv=fsync`, matching the intended durable completion measurement.

Limits: three samples each; new output inode/placement each time; warm-up,
cache/catalog state and restart effects remain. Group0 leadership changed from
node2 (Raft ID1) before restart to node3 (ID2) afterward; after restart group2
is led by node4 (ID3). Thus the measured deployment gain cannot be attributed
exclusively to batching. No matched leadership-controlled rollback/A-B, full
POSIX run or live power-loss campaign was requested/performed. The deterministic
native test establishes directory-barrier sharing, but production syscall
reduction per MiB was not measured in this run.

AMD's 2.5 Gb/s link still limits the optimistic 2+1 logical payload ceiling to
about 199 MiB/s before other overhead. This change does not establish or promise
1 GiB/s. Per-fragment file sync latency remains a constraint.

## Reproduction

Build natively on each destination using the identity above:

```sh
make -j8 EFS_GIT_ID=2256f1a34fc1 EFS_GIT_DESCRIBE=v0.3.2-pre-alpha-468-g2256f1a34 EFS_GIT_BRANCH=devel EFS_BUILD_TIME=2026-10-15T02:57:30Z efsd efs-fuse efs-mgmt tests/test_directory_sync_group tests/test_store_durability
./tests/test_directory_sync_group
./tests/test_store_durability
python3 tests/test_meta_flush_batch.py
python3 tests/test_report_retry_eof.py
python3 tests/test_report_pressure.py
python3 tests/test_report_zero_eof.py
```

Sanitizers: compile `tests/test_directory_sync_group.c` with
`gcc -D_GNU_SOURCE -std=c99 -Wall -Wextra -Werror -Iinclude -pthread
-fsanitize=address,undefined -g`, run with `ASAN_OPTIONS=detect_leaks=1`.
For the queue harness set `EXTRA_CFLAGS='-fsanitize=address,undefined -g'`.
Native tests own temporary fixtures, not production storage.

The committed measurement script creates one local random fixture and reuses
it across old/new runs. Each trial writes a **new** 512 MiB file with
`dd bs=1M count=512 conv=fsync,excl status=none`, measures process completion,
then verifies its full SHA256 outside the timed interval and removes only its
own output. The fixture remains outside the export for reproducibility.
Do not substitute zero-filled input: EFS elides zero-chunk PUTs.

```sh
python3 tests/measure/intel_dd_sync_batch.py /data1/efs/intel-mnt /data1/efs/intel-client/sync-batch-measure baseline --mib 512 --repeats 3
# After coordinated deployment, use the same fixture and change phase to candidate.
```

Intel1 uses `/data1/efs/mnt` and local fixture directory
`/data1/efs/sync-batch-measure`. Run clients serially, with no builds, profilers
or human I/O competing. Mounted four-client integrity uses the committed
`tests/measure/intel_getattr_mount_smoke.py`: create roles0..3, read all four,
then clean once. It checks fsync, shrink/regrow, exact EOF, contents and mtime.

Raw evidence is outside Git under
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-sync-batch-20261014/`.
Release source archive SHA256:
`8845d169879ac288261e8d19c2c70e7a2d0a582a6ab190e894a72f6dbb9adfc2`.
Isolated native workspace: `/data1/efs/sync-batch-work-20261014` on Intel1.
