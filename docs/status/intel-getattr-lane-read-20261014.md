# Intel GETATTR first-lane reuse — 2026-10-14

**Implemented/deployed; bounded correctness smoke PASS; isolated GETATTR faster.
Mounted speedup NOT ESTABLISHED: short post-restart samples were slower.**
Implements the next experiment recommended by the
[post-index live profile](intel-post-index-profile-20261014.md). Human scope:
implement, deploy, smoke and measure; no full acceptance or tuning project.

## Source and change

Deployed source `0d996b33dd362cfadba619cddac663f8db48bc49`, devel;
build ID `0d996b33dd36`, version `v0.3.2-pre-alpha-458-g0d996b33d`, common
build metadata `2026-10-14T20:56:57Z`. Baseline library/deployment
`6cbd99e549c8bd9576761556ef6b3b9ca8445e83`; intervening commits are documentation.
Native GCC `-O3 -g -fno-omit-frame-pointer -march=native -mtune=native` builds
on every destination. Final documentation commit adds a mounted smoke helper
and corrects a fixture comment; production C behavior is the deployed commit.

- [txn.c](../../src/meta/txn.c): new `efs_txn_reduce_read_value_ex()` returns
  both the reduction and the exact transactionally visible lane bytes used as
  its base, plus OK/NOT_FOUND status. Full-value capacity remains mandatory;
  storage, authority, scan and short-buffer failures propagate. Existing
  reduction callers retain the old API through a wrapper.
- [meta_apply.c](../../src/meta/meta_apply.c): GETATTR uses this first lane
  image for sequence/present/mtime-generation fields, removing the immediate
  duplicate transactional lane read. It still independently reads the lane
  sequence in its second collect, rereads the inode, and rechecks undecided
  transaction decisions. Content/generation/size/mtime and lane-set fences,
  committed pending reductions, malformed/absent handling and error refusal
  remain. No cached absence/decision or cross-request value reuse.
- No disk/wire format, ReadIndex policy, TTL, durable acknowledgement,
  retry/window, hashing, small-file format or transport change.

## Environment and deployment

Main Intel: two physical Core Ultra7 256V hosts,8 logical CPUs each,5Gb/s
Ethernet; four files-engine daemons on10.0.0.207/.209 ports17432/17433,
400GiB/node,106.33–106.34GiB occupied/node at readiness. Existing data paths
retained, including configured `/data2/efs/n2` and `n4` resolving under
`/home/efs/additional-work-dir`. Intel/xorinox Rocky10.2/Linux6.12/fuse3.16.2;
AMD Ubuntu/Linux7.0/fuse3.18.2. External physical AMD Ryzen AI MAX+395 and
xorinox Ryzen9 7940HS clients each have2.5Gb/s links.

Every main process verified with TCP and `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_EXPIRY=0`.
Daemons use `--direct-io`; `.efs` absent
on all four mounts. Eight process argument/environment/hash checks pass.

|Host|efsd SHA256|Intel-client efs-fuse SHA256|
|---|---|---|
|Intel1|`01fb6e9721f01a8d387914cf81c8989c0cfd0cd9d5498edf16272ce35a85ea11`|`da8d2b45b6ed12544ab434ecf99d5436d86596e46ce573010e600704b0e1e985`|
|Intel2|`bc3ac8427533420d5fbff71dbe30664294819908cb9d3faa40d6da97deab0e18`|`43113428d81f2b5286d10266af62284c87952e188fabbc2ef2b7a48f41aae9e8`|
|AMD|Independent client tree, no daemon deployment|`1e32cb9785e0ebdf399022af9b3cd7c2a17516aa476d9b010ee94af2ad92db0e`|
|xorinox|Independent client tree, no daemon deployment|`4ba39f7c8a4c1e435e17b51c6ee3a55e272dec0d482cc653638da13359c47012`|

Staged all native builds before stopping jobs. Graceful SIGINT let the AMD
writer finish outstanding folder writes and the xorinox MD5 reader preserve
its manifest. Every export client drained/unmounted while full quorum remained;
then only the four owned main daemons stopped. Source links switched to frozen
`releases/0d996b33dd36`; all peers launched before waiting for persisted recovery.
4/4 healthy and identical versions confirmed before remounting. No formatting,
store cleanup/relocation, forced discard, firewall or root package change.

Intel mounts `/data1/efs/mnt`; external mounts `/data1/efs/intel-mnt`.
AMD/xorinox sources/configuration/logs remain under `/data1/efs/intel-client`,
separate from their own clusters. All six private Intel and six AMD-local
processes match prior PIDs/arguments/hashes/profiles exactly.

After timing/smoke, AMD writer resumed with its original arguments plus
`--resume`:157,709 existing files validated in8.3s, preserved, then new creation
confirmed. PID467585; log
`/data1/efs/intel-client/logs/tiny-files11-resume-0d996b33dd36.log`.
xorinox restarted its four-worker MD5 crawl (fresh traversal; old manifest
retained), PID667007, new manifest
`/data1/efs/intel-client/logs/md5-resume-0d996b33dd36.jsonl`.
Both jobs running; reader reported zero errors/changed files at checkpoint.
Progress/rates during these different live workloads are not a speed comparison.

## Bounded checks and measurement

On both Intel hosts, final committed binaries pass `tests/test_txn`,
`tests/test_meta_apply` and the default
[getattr_lane_smoke](../../tests/measure/getattr_lane_smoke.c) checks.
The existing metadata test covers committed/unresolved and undecided reductions,
pending-set overflow/refusal, mtime fences and shrink/generation behavior.
Added transaction assertions cover exact returned lane bytes alongside a
committed reduction, whole-value capacity, absent lane with visible reductions,
and inaccessible authority. The focused fixture injects a lane sequence change
and an inode generation/size change between collects: each forces a retry and
returns the new consistent result. Read/scan failure and malformed-lane refusal,
and a missing lane, also pass. The old-library negative-control assertion fails
because its extra read puts the same hook earlier in the collect; this is not
claimed as independent proof of an old production linearizability bug.

### Matched actual-LSM fixture

Same committed C fixture linked against each revision's own headers/library,
pinned to CPU0. One file with1 or8 active64byte lanes,4,096 unrelated64byte
records, a flushed immutable real LSM. Fixture-only NOSYNC WAL; timed operations
are warm, read-only native GETATTR, not TCP/direct filesystem I/O. Two alternating
baseline/candidate trials per host; five10,000-stat rounds/trial, discard first,
report median ns/stat over eight remaining rounds. Fixture creates/removes
only its own `/tmp/efs-getattr-lane-*` store; no deployed data copied or changed.

|Host / active lanes|Baseline ns/stat|Candidate ns/stat|Time reduction|
|---|---:|---:|---:|
|Intel1 /1|654.60|576.45|11.9%|
|Intel2 /1|652.85|575.50|11.8%|
|Intel1 /8|2,594.85|2,001.05|22.9%|
|Intel2 /8|2,560.80|2,013.55|21.4%|

Instrumented KV gets/stat fall11→9 with one lane and53→37 with eight;
prefix scans stay1 and8 respectively. Each removed transactional lane read
normally costs an intent probe plus the base lookup. The reduction scan and
second validation read remain. This establishes the intended local work
reduction; it does not establish whole-filesystem or sustained throughput gain.

### Mounted measurement and correctness

Human jobs stopped for both baseline and candidate measurements. Run
[intel_metadata_smoke.py](../../tests/measure/intel_metadata_smoke.py) serially
on AMD then xorinox: identical six preserved tiny-file paths, same byte/hash
controls, five120-stat rounds, discard first, average remaining round means.
No tracer attached. Candidate repeated after other checks/recovery activity.

|Client|Baseline mean ms/stat|First candidate|Candidate repeat|
|---|---:|---:|---:|
|AMD|1.684|2.862|2.403|
|xorinox|1.722|2.622|2.173|

**Mounted samples are slower; no mounted speedup claim.** A restart separates
these samples. Persisted catalog/cache/background work and Raft leadership were
not held constant; post-recovery logs show ongoing metadata orphan scans.
Those are confounders, not a proven explanation of the slowdown. There is no
controlled A/B evidence attributing the end-to-end difference to this change.
A repeat with comparable catalog/cache/leadership and fixed workload is the
remaining performance gate; broad profiling/tuning was not added to this smoke
scope. Initial overlapping exploratory measurements were excluded from this
serial comparison. No correctness failure occurred in any timed round.

Before/after64 preserved files per client (32previous checksum-tail EIO cases
and32 controls) retain identical inode, exact size and SHA256 through all four
mounts. [intel_getattr_mount_smoke.py](../../tests/measure/intel_getattr_mount_smoke.py)
then creates one owned file/client, writes262,161 bytes and fsyncs, shrinks to
4,099 and fsyncs/checks size, grows to65,553 with a zero-filled gap and tail
write, fsyncs, sets exact mtime. All four clients verify all four final byte
images, exact EOF, size, nlink and mtime. Owned fixture removed after success.
4/4 healthy final cluster status and matching versions PASS.

Full POSIX/concurrent/fault/sanitizer/long-run acceptance NOTRUN by smoke-only
human scope. Million-file/whole-tree MD5 completion is still running and is
not counted as passing acceptance here.

## Reproduction

Unprivileged Linux efs account, GCC/make, fuse3/libibverbs development packages,
frame-pointer native builds; no root changes needed on these hosts. Use the
same fixed source identity/time for every peer/client. On an isolated build:

```sh
make -j4 EFS_GIT_ID=0d996b33dd36 \
  EFS_GIT_DESCRIBE=v0.3.2-pre-alpha-458-g0d996b33d EFS_GIT_BRANCH=devel \
  EFS_BUILD_TIME=2026-10-14T20:56:57Z \
  efsd efs-fuse efs-mgmt tests/test_txn tests/test_meta_apply tests/measure/getattr_lane_smoke
./tests/test_txn
./tests/test_meta_apply
./tests/measure/getattr_lane_smoke
taskset -c 0 ./tests/measure/getattr_lane_smoke --measure
```

Compile the same measurement C against the old release separately with
`gcc -O3 -g -fno-omit-frame-pointer -march=native -D_GNU_SOURCE
-IOLD/include tests/measure/getattr_lane_smoke.c OLD/libefs.a
-lpthread -lm -ldl -libverbs -o baseline-smoke`.
Alternate baseline/candidate twice on the same CPU; never mix headers/objects.
The no-argument interleaving assertion is candidate qualification; use
`--measure` for the baseline comparison.

Existing deployment controls: Intel `EFS_CLUSTER=intel` devops client-only
stop on both hosts, then daemon-only stop, switch verified source links,
`EFS_INTEL_PHASE=launch` on both before `wait`, verify4/4 then `mounts`.
Explicitly set TCP/direct and all four optional-feature environment switches
above to0. External `intel-client/client.sh stop|start` controls only its own
mount. Preserve `.mkfs-done`, storage, independent cluster controllers and
existing data; never format or force-discard for this experiment.

For read-only mounted comparison, run unchanged `python3
tests/measure/intel_metadata_smoke.py MOUNT` before/after serially with the
same workload state and catalog. Missing fixtures fail; they are not skipped.
For newly owned data smoke choose a unique RUN beginning `getattr-lane-smoke-`:

```sh
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT create RUN ROLE
# Run create once on each host with distinct ROLE0..3, then read on all hosts:
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT read RUN ROLE
# Only after all reads pass, clean once (verifies all images before deleting):
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT clean RUN 0
```

The helper refuses existing writer files; do not reuse a prior RUN or delete
ambiguous output after a failure. Requires no private retained capture.

Raw evidence outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-getattr-lane-20261014/`;
`artifact-sha256.json` covers source archive, all build identities, paired native
runs, mounted samples, process inventories and smoke results. Remote native
runs: `/data1/efs/getattr-lane-final/` on both Intel hosts. Source archive SHA256:
`497e0d4380fa9a5e7f79e7557c4fa76f6340d392f38bc59990141efb1ffad28a`;
native summary: `aa271b671773d09db64562d720a47c541c4939856892392274a9be4c58164bae`.
