# W90 expiry inspection, worker status and soak — October 12, 2026

Follow-up on `devel-expiry`, original release base
`3a41493591dcede820700db7d934da9dc10f07ee`, accepted D33 singly linked regular-file
scope. Initial lifecycle qualification remains recorded in
[expiry implementation](expiry-implementation-20261012.md).

**Current verdict:** native build/unit and GC status compatibility PASS;
full inspection/status/fault lifecycle and 30-minute NUC soak in progress.
Do not treat initial W90 closure as a completed soak result.

## Inspect a file or folder

Paths are export-relative by default; this works without a local mount:

```sh
./efs-mgmt expiry 127.0.0.1:17432 show /scratch/example.txt
./efs-mgmt expiry 127.0.0.1:17432 show /scratch
```

For an actual local mounted path, explicitly identify the mount prefix:

```sh
./efs-mgmt expiry 127.0.0.1:17432 show /data1/efs/mnt/scratch/example.txt \
  --mount /data1/efs/mnt
```

Files show FileID, a local-time ISO-style deadline with timezone and nanoseconds,
relative `expires in …` / `overdue by …`, and the saved policy root/generation/
revision used to materialize that file deadline. `expiry: none` distinguishes
absent/cleared deadlines. Changing a folder policy does not rewrite existing
file deadlines. Folders show **no folder expiry**, then the nearest explicit
future-file policy: enabled/disabled, default TTL, maximum retention, owner-clear
permission, policy source FileID and revision. A disabled child policy shadows
an enabled ancestor. Recursive directory and symlink expiry remain outside D33.

The server resolves paths and checks FileID generation. With `--mount`, the
local inode must match the server. Changed/missing endpoints, deadline changes,
server errors or bounded ancestry limits return failure; callers must honor
exit status rather than use partial output after failure. This is a sampled
inspection across RPCs, not an atomic namespace/policy snapshot. Directory
ancestry may change between reads. Policy sources are identified by authoritative
inode/generation, not a possibly stale reconstructed path. Existing trusted-client
management identity/transport assumptions remain; no new authentication claim.

## Inspect the worker and counters

```sh
./efs-mgmt expiry-status 127.0.0.1:17432
./efs-mgmt gc-status 127.0.0.1:17432
```

Query all four node ports for local worker observations. Human-readable worker
status distinguishes disabled, clock-paused, standby follower and running leader;
shows last successful removal age, confirmed removed-file count, observed logical
namespace bytes, unknown-size count, refused/retry/errors, and overdue samples
with `as of … ago`. Missing samples are reported as missing, not zero backlog.
A backlog page scans at most 64 due index entries; `at least 64` is a capped
lower bound. Samples may be stale and are masked out if the node no longer leads
that group. Expiry work retains 8 attempts/pass and 50-ms checks between operations;
individual durability/RPC waits can exceed 50 ms.

`gc-status` wire version 3 appends:

- `expired_files`: confirmed namespace removals observed by this coordinator.
- `expired_namespace_bytes`: sum of stable, lane-derived logical sizes observed
  before those removals; concurrent writes can change size after sampling.
- `expired_size_unknown_files`: removals without a stable size observation;
  telemetry failure does not prevent safe conditional deletion.
- Clock/enabled/leader state, passes, refusal/retry/errors, last-pass/removal ages,
  and per-group backlog lower bounds/capping/sample ages.

These are **process-local, since-start observations**, reset on restart, not a
replicated lifetime accounting ledger. Ambiguous successful outcomes may be
missing; successful replay cannot double-delete/count a replacement. Only the
coordinator counts confirmed removals, not both coordinator and forwarding worker.
Existing `removed_fragments`, `unlinked_file_bytes` and `reclaimed_payload_bytes`
are actual GC totals for **all causes**. They do not attribute physical bytes to
expiry alone, and logical namespace bytes must not be labeled physical reclaimed
space. Open descriptors and offline members can delay reclamation after expiry.

The new management binary accepts GC versions 1/2 and refuses expiry-specific
status as unavailable on those nodes. Older management binaries may reject the
new version 3 reply; upgrade tools with servers. No persistent metadata schema
change is introduced by telemetry.

## Repeat the soak

Linux/FUSE on NUC, permission for both empty/unmounted paths, four free ports,
private storage parent and enough free space/inodes. Build in a separate clean
source directory; record exact commit, binary hashes, native build flags and
fault setting. Native build (four jobs):

```sh
make -j4 EFS_FAULTS=1 efsd efs-fuse efs-mgmt tests/test_expiry tests/test_meta_apply
./tests/test_expiry
./tests/test_meta_apply
python3 tests/test_gc_status_expiry.py ./efs-mgmt
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/expiry-soak-private --peer-mount /data1/efs/mnt2 \
  --port 24410 --jobs 4 --clock-fault --soak-seconds 1800
```

The suite enforces TCP/direct I/O, FILES storage, `.efs` disabled, no containers
and no packing. A single `--root` places both node pairs in fresh subdirectories
beneath that parent; two roots may be supplied for separate drives. It first runs
lifecycle/inspection/status/clock-fault gates, then four workers across two mounts
for at least 1,800 seconds, then core/peer POSIX and ordinary GC/cold cleanup.

Each round creates 20 files with 16-KiB payloads and fsync, renews four deadlines,
renames originals and installs four non-expiring replacements at reused paths.
Twelve originals must disappear and physically reclaim; renewed and replacement
payloads must survive past original deadlines and match exactly. Cleanup must
reclaim the survivors too. Policy TTL is 10s, max retention 120s; renewals use
90s deadlines. Cold restarts every ~300s and at the end occur with descriptors
closed and writes already fsynced, preserving the current batch's durable index.
The round verification retains deadlines (35/60/90s waits and a 90s renewal
verification window); overall elapsed time can exceed the requested duration.

Each round records GC/expiry status, free bytes/inodes and owned daemon RSS.
Free-space floor 2 GiB, inode floor 10,000, daemon RSS ceiling 512 MiB; fixture
quotas remain 1 GiB/node. These are finite test guardrails, not proven global
memory bounds or production capacity claims. The fixture retains raw JSON/logs
outside Git and stops its owned mounts/processes; preserve unrelated user data.
[POSIX runner instructions](posix-expiry-suite.md).

Exact run identities/results and retained evidence will follow qualification.
