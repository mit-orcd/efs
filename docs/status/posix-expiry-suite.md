# Running the POSIX expiry suite (W90 / D33)

`posix_suite.py --suite expiry` runs the existing expiry lifecycle gates in an
owned four-node fixture, then core POSIX, two-client POSIX and physical-GC/cold
recovery checks. It propagates the harness exit status: zero only if all pass.
Expiry is separate from `core`, `extras` and `all`; those operate on an existing
mount, whereas expiry deliberately restarts and faults its own cluster.

## Prerequisites and commands

Run locally on Linux (NUC qualification host), from this branch's source tree.
Install FUSE and permit the invoking user to mount both paths. Build native
binaries first; no containers are used. Both mount directories must be empty
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
--help`. The wrapper enforces TCP, direct I/O, FILES storage, `.efs` disabled
and packing disabled. Operator expiry controls run through `efs-mgmt`, so the
suite does not require `.efs` commands. `--jobs` controls core POSIX concurrency;
it does not parallelize restart/fault lifecycle phases.

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
