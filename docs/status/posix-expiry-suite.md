# Running the POSIX expiry suite (W90 / D33)

`posix_suite.py --suite expiry` runs the existing expiry lifecycle gates in an
owned four-node fixture, then core POSIX, two-client POSIX and physical-GC/cold
recovery checks. It propagates the harness exit status: zero only if all pass.
Expiry is separate from `core`, `extras` and `all`; those operate on an existing
mount, whereas expiry deliberately restarts and faults its own cluster.

## Prerequisites and commands

Run locally on Linux (NUC qualification host), from this branch's source tree.
Install FUSE and permit the invoking user to mount both paths. Build native
binaries first. Both mount directories must be empty
and unmounted, four ports must be free, and storage roots must be private to
this test. Missing private root parents are created; each run creates fresh
fixture subdirectories within them. Do not stop or attach to an unrelated running user cluster.

```sh
make -j4 EFS_FAULTS=0 efsd efs-fuse efs-mgmt
python3 tests/posix/posix_suite.py /data1/efs/mnt --suite expiry \
  --root /data1/efs/expiry-posix-normal --root /data1/efs/expiry-posix-normal \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4
```

Equivalent orchestrator entry point (also local, no SSH baseline step):

```sh
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/expiry-posix-normal --root /data1/efs/expiry-posix-normal \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4
```

For clock uncertainty/pause/reset gates, use a separate clean build directory
with `EFS_FAULTS=1`, fresh private roots, and append `--clock-fault`. The marker
injects unhealthy clock state without changing the host clock. Mixing old
normal objects with fault-enabled objects is not a qualified fault build.

```sh
python3 tests/posix/posix_suite.py /data1/efs/mnt --suite expiry \
  --root /data1/efs/expiry-posix-fault --root /data1/efs/expiry-posix-fault \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4 --clock-fault
```

View options with `python3 tests/posix/posix_suite.py /data1/efs/mnt --suite expiry
--help`. The wrapper enforces TCP, direct I/O, FILES storage, `.efs` disabled. Operator expiry controls run through `efs-mgmt`, so the
suite does not require `.efs` commands. `--jobs` controls core POSIX concurrency;
it does not parallelize restart/fault lifecycle phases.

For sustained two-mount churn/restart validation, add `--soak-seconds 1800`.
See [inspection/status and soak details](expiry-inspection-soak-20261012.md).
A single `--root` is accepted and used for both node pairs.

For large finite backlogs, append `--backlog-files 10000`: three phases each
create 10,000 files with 1-KiB real payloads and eight workers, then schedule
staggered deadlines, restart with a partially drained common-deadline queue,
and disable expiry
across a common deadline before re-enabling. The test records namespace and
physical drain, a renewed survivor, cold inventories, RSS/capacity, and
foreground create/write/fsync/read/unlink latency before and during cleanup.
Deadlines have 180 seconds lead time; staggered groups differ by 10 seconds.
Namespace drain is bounded by max(1800 seconds, file count / 4): 2,500 seconds
for 10,000 files. Physical drain has another 300 seconds. The default expiry
worker attempts at most
eight files/group/pass and normally yields one second, so a single-group
10,000-file queue took about 22–23 minutes in the original October 12 qualification; this is an asynchronous cleanup
test, not an immediate-deletion promise.
Counts must be 1000..100000; zero omits this opt-in gate. Larger counts need
adequate private quota and deadline preparation time; the default fixture is
not a claimed 100,000-file capacity qualification. See
[original backlog results](expiry-backlog-20261012.md) and
[bounded-history/cadence follow-up](expiry-rss-cleanup-20261013.md).

## Results and cleanup

The harness prints `Evidence directory: ...` beneath a private root. That
contains `manifest.json` (source/build/configuration identity),
`expiry-runtime.json` (mapped executable hashes and actual environments),
`expiry-lifecycle.json` (named gate verdicts), core `posix.tsv`, peer `posix2.tsv`
and daemon logs. Capture console output outside Git for a complete failure
trace. Raw output is not automatically committed, even with `COMMIT=1` in the
shell orchestrator. The expiry wrapper rejects unsupported ordinary-runner
options, including `--results`, `--keep`, `--filter` and `--timeout-s`; results
and lifecycle deadlines are owned by the existing harness.

Checks include inherited/fixed deadlines, owner/operator refusals, existing-file
enablement, renewal/replay, rename/replacement, hard links, open descriptors,
publication, restart, disable/re-enable catch-up, actual offline fragment holder,
coordinator/leader loss, a 144-file backlog and independent physical reclamation.
The harness stops its owned mounts/processes on exit and retains private evidence.
Inspect retained logs before deleting only the generated fixture directories;
never remove an existing user store or force-discard pending writes.

[Implementation and full qualification](expiry-implementation-20261012.md).

## NUC entry-point validation — October 12, 2026

**PASS:** committed runner source
`5b41b8d3` invoked through `bash tests/run_tests.sh posix expiry`, with the
exact native fault-enabled daemon/client/management binaries from
`0e975936ce3c4496883fa9ee192aa20c34439853`. No C sources changed between these
commits; [binary SHA256 identities](expiry-implementation-20261012.md#final-qualification--pass)
are the fault column of the preceding qualification. The wrapper source and
companion scripts came from a clean archive of the runner commit.

The clock-fault command above was used with root
`/data1/efs/evidence/expiry-posix-runner-5b41b8d3` repeated twice. Four private 1-GiB-quota
nodes, two mounts, XFS, TCP/direct I/O, FILES storage, `.efs` disabled,
core concurrency four. All expiry lifecycle/fault gates,
ordinary physical GC, cold empty inventory, drained queues and zero quotas
passed. Core POSIX **216/216, 0 fail/skip/notrun (24.4s)**; peer
**67/67, 0 fail (13.0s)**. Orchestrator returned zero. Both mounts and owned
processes stopped; existing user data preserved.

Runner self-test and shell syntax checks pass. Separate launcher checks verified
forced configuration despite conflicting inherited environment, nonzero child
exit propagation, root creation, and rejection of missing/invalid arguments.
The first `42abd400` launch failed before starting any cluster because its new
storage parent did not exist; `5b41b8d3` adds parent creation and the full rerun
passes. Both `--suite expiry` and `--suite=expiry` reach the new suite.

Retained evidence on NUC:
`/home/efs/expiry-posix-runner-5b41b8d3.log`, SHA256
`730ac46b3e83ddb0b7764de0de0be5dff62c2afd19f9c532f028f7af8a180974`;
fixture `/data1/efs/evidence/expiry-posix-runner-5b41b8d3/gc-direct-oszl7ahi/`
contains manifest, actual runtime proof, named expiry verdicts and POSIX TSVs.
These raw artifacts are outside Git. This validates runner integration, not
additional large-scale capacity or a broader expiry policy than D33.

**Raw evidence note (2026-10-23):** the bulk data in `expiry-posix-runner-5b41b8d3` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
