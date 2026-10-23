# W90 expiry inspection, worker status and soak — October 12, 2026

Follow-up on `devel-expiry`, original release base
`3a41493591dcede820700db7d934da9dc10f07ee`, accepted D33 singly linked regular-file
scope. Initial lifecycle qualification remains recorded in
[expiry implementation](expiry-implementation-20261012.md).

**Current verdict: PASS.** Readable inspection and worker status, normal/fault
lifecycle matrices, 30-minute two-mount soak, both full POSIX/peer regressions
and cold physical cleanup pass. W90 inspection/status/soak follow-up complete.

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

Read inspection requires `EFS_EXPIRY=1`; worker status remains available when
expiry is disabled. The server resolves paths and checks FileID generation. With `--mount`, the
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
distinguishes initial clock sampling from latched uncertainty; shows last successful removal age, confirmed removed-file count, observed logical
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

The suite enforces TCP/direct I/O, FILES storage, `.efs` disabled. A single `--root` places both node pairs in fresh subdirectories
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

## Final qualification — PASS

Fault/long-soak source: `3b87383970afd2e7d0213caca8ce011236ce835c`.
Final normal/reporting source: `2da7a70a466d29234c965fead5130eba74c8aa4e`.
Both on `devel-expiry`, clean archived sources, native four-job builds with
`EFS_GIT_ID` / `EFS_GIT_DESCRIBE` set to the corresponding short commit and
`EFS_GIT_BRANCH=devel-expiry`. The latter changes only management reporting:
correct expiry-status usage and distinguish initial clock sampling from latched
uncertainty. All server/storage/expiry sources, protocol declarations and live
workload scripts are unchanged between these tested candidates. The 30-minute
run is qualified at its recorded source; it is not mislabeled as a later build.

NUC: Intel i5-1135G7 (4 cores / 8 threads), x86_64, Linux
`6.12.0-211.64.1.el10_2.x86_64`, `/data1` on `/dev/sda`, XFS. Four local nodes,
two metadata groups, two mounts, ports 24410..24413, 1-GiB/node private quotas;
TCP/direct, FILES storage, `.efs` disabled. The normal
build was prepared during early long-soak rounds; this is correctness/resource
headroom evidence, not an isolated performance benchmark or hardware ceiling.

| Run | Expiry gates | Soak | Core POSIX | Peer POSIX | Cold/physical GC |
| --- | --- | --- | --- | --- | --- |
| `3b873839`, faults enabled | 25/25 | 30 minutes + final cold check, PASS | 216/216, 23.8s | 67/67, 12.4s | PASS |
| `2da7a70a`, normal | 24/24 | 60s + final cold check, PASS | 216/216, 24.9s | 67/67, 13.4s | PASS |

Core has zero failures/skips/notrun; peer zero failures. Both commands exit zero.
Both expiry unit executables pass on each build. New management compatibility
regression covers v1/v2, v3 expiry counters, unknown sizes, malformed lengths/
versions, clock warmup and latched uncertainty. Runner self-test and shell syntax
checks pass. An early `1a0fc2a9` native compile found the inode row stores
`base_size`, not full lane-reduced EOF; it was not deployed/qualified. Corrected
`3b873839` uses best-effort stable lane-derived size and explicit unknown counts;
failed build evidence remains `/home/efs/expiry-1a0fc2a9-build.log`.

Long-soak result: **137 rounds**, **2,740 files**, 16 KiB/file,
**1,644 expired originals** independently gone from namespace and physical
storage, **548 renewals** and **548 replacements** surviving with exact bytes,
then reclaimed on cleanup. **Six cold restarts** (five periodic plus final).
Churn completed in 1,808.662s; final cold observation at 1,813.971s.
139 resource/status observations sampled all four daemons each time:
maximum sampled RSS **36,308 KiB (~35.5 MiB)**, minimum free bytes
**219,133,067,264 (~204.1 GiB)**, minimum available inodes **368,409,049**.
Observed expiry errors and unknown-size counts stayed zero. RSS samples are not
a continuous peak measurement or a global capacity/memory-bound proof.
Normal short soak: five rounds, 100 files, 60 expirations, 20 renewals,
20 replacements, one final cold restart, 65.625s churn, PASS.

Both fixtures finish with empty physical inventories, drained orphan/GC queues,
zero quotas and late-PUT fencing. All owned processes and both mounts stopped;
existing user data preserved. This deploys/tests private fixtures, not a silent
replacement of the regular NUC installation. Expiry remains default-off until
explicitly configured; no universal retention period is introduced.

Exact commands used (from each recorded build tree):

```sh
# /home/efs/devel-expiry-3b873839-fault, EFS_FAULTS=1
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/evidence/expiry-3b873839-soak --peer-mount /data1/efs/mnt2 \
  --port 24410 --jobs 4 --clock-fault --soak-seconds 1800
# /home/efs/devel-expiry-2da7a70a-normal, EFS_FAULTS=0
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/evidence/expiry-2da7a70a-normal --peer-mount /data1/efs/mnt2 \
  --port 24410 --jobs 4 --soak-seconds 60
```

Retained raw evidence on NUC, outside Git:

- Long console `/home/efs/expiry-3b873839-soak.log`; fixture
  `/data1/efs/evidence/expiry-3b873839-soak/gc-direct-_xt_r85c`, sibling
  `gc-direct-1k59okx3` for n3/n4.
- Normal console `/home/efs/expiry-2da7a70a-normal.log`; fixture
  `/data1/efs/evidence/expiry-2da7a70a-normal/gc-direct-3cek4dph`, sibling
  `gc-direct-_aeiowzu` for n3/n4.
- Each primary fixture retains manifest, actual mapped runtime/configuration proof,
  named lifecycle verdicts, soak summary/observations, core/peer TSVs and logs.
- Actual readable examples `/home/efs/expiry-3b873839-inspection-example.txt` and
  `/home/efs/expiry-3b873839-status-example.txt`; native build logs
  `/home/efs/expiry-3b873839-build.log` and
  `/home/efs/expiry-2da7a70a-normal-build.log`.

SHA256 identities:

| Artifact | Fault / long | Final normal |
| --- | --- | --- |
| efsd | `92e817e96d36f9b185f37c69a7de4a6c94f301ee1f28a0505b0ee9b936bcea18` | `f378a9102c8ef16531bdf8c879f63ce4f4b218ea7bb59375121291cd13165b47` |
| efs-fuse | `76941f340e563886d718717788ea3934ba6403715e21fc7ffedcb7b05b11b331` | `6baa55e154fc8377221c99188730b9546d27f774414a94394b55aee3c6cd870d` |
| efs-mgmt | `808112ee4425ca5dc59fbe57914b3143d33a2673ea529a9bc4f236fb288e2340` | `71960d6d221614a5544998ae9283d639c6f6209b5cb9aaf72293eeda9c4fcf28` |
| Console | `cc0d5b77d9f2e03bc38d597f785d1bf6021a7f8a9b489f60e5e86958b83a0c18` | `07294796607b869f418f58edf47da61b027d4a45ca4a100872bbdaa0ccdc81bf` |
| Manifest | `02e29bf75fe9fe728d6798899ea0eed5e3d837bb967a3e9c8c4ba1cfe2cb82a5` | `d76598164f9bd020eeb616a01c8ee4074837a3a621b096c676e067e711ed6688` |

Long soak summary SHA256
`2f566276874d4b581d6f5ed732017a0d1221e1ebb10f58a7debcf2e179dbb657`;
observations SHA256
`a5d1a5627e3f9beeacc7ef5dc11b1fec43d72220c8e89c8309c3c22ed6e75ea9`.
