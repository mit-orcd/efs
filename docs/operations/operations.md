# Operations

**Accepted deployment contract (D36, Oct 9):** three nodes are the minimum;
four nodes provide the maximum two-node-loss safety target; additional nodes
scale volume and performance across all dimensions. Today's fixed 2+1 data and
three-voter metadata groups do not implement that four-node guarantee. See
[binding requirement and implementation gates](../status/node-count-protection-20261009.md); preserve the requirement
rather than deriving the design from current code.

[Quick start](../using/quickstart.md) ·
[Failure tolerance](../how-it-works/failure-tolerance.md) ·
[Architecture](../how-it-works/architecture.md) · [Testing](../how-it-works/testing.md)

## Scripts

```text
./scripts/server.sh [--perf] <addr:port> <path[,path...][:quota]> [join-addr:port] [efsd args...]
./scripts/server.sh stop <path[:quota]|addr:port>

./scripts/client.sh [--perf] [--strace] <server-addr:port> <mount-path> [efs-fuse args...]
./scripts/client.sh stop [--force-discard] <mount-path>
```

Pass server `--strace` after the positional storage/join arguments as an
`efsd` option; the server wrapper only parses leading `--perf`. The client
wrapper accepts leading `--perf` and `--strace`.

The bind address must be a **real local IP** (not `0.0.0.0`). Peers connect
back to it.

`server.sh` retires an existing daemon on the same path/port before starting.
`scripts/server_processes.py` verifies executable, arguments and saved process
identity, then uses a Linux pidfd to send SIGTERM and wait up to 60 seconds.
A timeout retains the process and reports failure; it does not escalate to
SIGKILL. Stale PID identity is not authority to signal an unrelated process.
There is no node id to give: the first node is 1 and the cluster assigns the
others (K33). The member count and the metadata voters come from the committed
membership; efsd refuses the removed `--node-id` and `EFS_MD_RAFT_N`. A third
positional argument joins: a member's `addr:port`, or `join` to find the
cluster on the local subnet. If startup exits immediately, the wrapper prints
the last log lines.

The client needs **one** server address; it discovers the rest. If it runs on
the same host as a server, it prefers the local fragment.

`client.sh` daemonizes and returns **only after** the mount is serving
(FUSE_INIT completed and `stat` of the mountpoint works). `/proc/mounts`
listing the path is not enough. Pass `-f` to stay in the foreground.

Ordinary client stop quiesces mutations, attempts a bounded drain, and refuses
to detach unresolved writes. If stop fails, keep the servers available and
resolve the failure. `--force-discard` explicitly permits loss and lazy detach;
see the [controlled-stop contract](#scripts). Signals and
external unmount bypass that contract.

## Transport and storage mode

TCP and native-IB/RoCE RDMA are independent of buffered/direct fragment I/O.
See [transport selection](../using/power-user.md#mounting-and-transport) for
verbs device/GID settings. Use the same intended transport on servers and
clients when validating a configuration, including clients remounted during
cold verification. AMD's release evidence uses RXE software RoCE on one host.

The portal shown in the [README preview](../../README.md#portal-preview)
is an external tool; its code is not shipped in this repository. For bundled
status inspection use `efs-mgmt status`, process/log inspection and the GC
counters. “Heal idle” is not proof of an implemented fragment repair owner.

## Upgrades

Upgrade participating daemons and clients together: the build-ID handshake
rejects incompatible builds. Preserve existing stores and establish format
compatibility before a migration. `mkfs` initializes an export; it is not a
format converter. Compile on the target machine class because native CPU
instructions can make binaries incompatible with a different host.

## Running the binaries yourself

```bash
# Disposable empty roots and free ports; do not reuse an active cluster here.
export EFS_TRANSPORT=tcp
./efsd 127.0.0.1:17432 /tmp/efs/s1 &                                    # creates the cluster
./efsd --join 127.0.0.1:17432 --listen 127.0.0.1:17433 /tmp/efs/s2 &    # joins it
./efsd --join 127.0.0.1:17432 --listen 127.0.0.1:17434 /tmp/efs/s3 &    # third: forms

./efs-mgmt status 127.0.0.1:17432
mkdir -p /mnt/efs
./efs-fuse 127.0.0.1:17432 /mnt/efs
```

`efsd ADDR:PORT STORAGE` creates a cluster in fresh storage (this node is
node 1 and mints the cluster identity) or restarts the node of that store.
`efsd --join [SEED] STORAGE` joins: through SEED (any member's
`ADDR:PORT`, across subnets too) or, without SEED, through a cluster found on
the local subnet (UDP broadcast on each interface's own subnet, port 17430;
`--discovery-port`, `--discover-on IFACE`). A joiner takes the address of the
interface that reaches the cluster and the cluster's service port unless
`--listen ADDR:PORT` names its endpoint (needed for several nodes on one
host). Several clusters answering need a SEED; when nobody answers the node
says so and waits; discovery never creates a cluster and never decides
membership.

The cluster forms by itself once the first node and two joiners are there:
the first node proposes node ids in endpoint order, each joiner persists a
promise for that proposal, and the cluster commits it in both metadata
groups (docs/status/k33-membership-protocol-20261023.md, M4'). Every daemon
derives the member count and the voters from the committed record from then
on, across restarts. A node that joins later is added online with the next
id; group 2 moves to {2,3,4} at four members and later nodes add storage.
Before formation a daemon serves no client metadata. `mkfs` (an alias for
`raft-mkfs`) is optional: before formation it proposes at once on the first
node; afterwards it repeats the notices and keeps the first membership.

Several storage roots (1–24, separated with commas in STORAGE) are allowed. New writes go to the path with
the shortest writer queue (fairness-weighted by bytes already assigned).
Overwrites stay on the existing path.

## Storage layout

Data lives under each storage path; membership, usage snapshots and wrapper
logs/PID files use the first root. Metadata uses the first root unless
`--meta-storage` selects another root:

| Dir | Contents |
|---|---|
| `data/` | fragments (file chunks) |
| `mdraft/` | metadata: this node's Raft log, KV segments, applied index per group; `owner.lock` (one daemon per store), `boot.bin` (durable boot counter) and `identity.bin` (store token, cluster identity, node id and endpoint, the formation promise or the committed membership) |
| `meta/` | node table (`cluster_nodes.bin`, the joined nodes; not the committed membership) and `usage.bin` |
| `log/` | logs, PID file, optional `perf` output |

Fragment paths are sharded by inode (five base-10000 segments) so the export
does not grow one directory per file:

```text
data/exports/{export_id}/{d4}/{d3}/{d2}/{d1}/{d0}/{chunk>>10}/{chunk}.{frag}.{generation}
```

Fragment payload length depends on the object; an ordinary full data chunk
produces 64 KiB fragments. Direct-I/O writes append a 4 KiB tail page holding
the 32-byte digest; buffered writes append the digest itself. Server GET fails closed on missing/corrupt checksum evidence;
client GET paths compare replies with captured metadata digests where available.
Fragment record format 2 binds the fragment identity and payload digest (K16,
W75); the only repair is W154 rebuild on return.

The former advice to wipe an older sidecar store as an upgrade had no verified
migration gate. Preserve existing stores and establish format compatibility
and a verified data migration before any destructive procedure. `mkfs` is
initialization, not a fragment-format converter.

`--meta-storage <root>` puts `mdraft/` on a root the fragment writers do not
use. Default is the first storage root; the flag does not move an
existing `mdraft/`.

## Auto-rejoin

Membership is saved under the storage path. Restart with the **same
storage** and the server rejoins by itself through its persisted peers; the
store keeps its cluster, node id and endpoint (another endpoint is refused):

```bash
./efsd 127.0.0.1:17434 /tmp/efs/s3 &                    # or the same --join command
```

If peers are down it starts standalone and retries in the background. Local
data is kept.

Membership is always persisted (`--no-persist` was removed, D42; efsd
refuses it).

## Quotas

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G
# or
./efsd 127.0.0.1:17432 /tmp/efs/s1 --quota 5G
```

Suffixes: `T`, `G`, `M`, `K`. The quota belongs to the node across its storage
roots, not independently to each path. A PUT returns QUOTA after at least two
stripe quota failures, mapping to application `ENOSPC`. One quota-rejected
fragment with two ACKs can still return success; the stripe is then recorded
as degraded (W154) and rebuilt once that node accepts the fragment.

```bash
./efs-mgmt status 127.0.0.1:17432
```

`used` charges fragment payload on creation and is reduced by successful
fragment garbage collection; `meta/usage.bin` is an advisory persisted snapshot.
It is not physical filesystem usage: metadata, fragment checksum tails and logs
also occupy space. Unlink cleanup is asynchronous and open files can delay it.
Compare EFS node usage with physical `df` on each storage root; a quiet GC log
is not a completion signal. See [the GC review](../status/gc-reclamation-review.md)
for the active incident and validation limits.

## Cluster management

```bash
./efs-mgmt status 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432          # alias for raft-mkfs
./efs-mgmt shrink-quota 127.0.0.1:17432 2G
```

`shrink-quota <node> <amount>` subtracts the amount from the running node
quota immediately and refuses a result below current usage. It does not start
background migration, and the reduction lives only in the running daemon: a
restart applies `--quota` (or the automatic quota) again.

A node is added with `efsd --join [SEED] STORAGE`; `efs-mgmt add-node` was
removed and is refused (finding F4). A node's storage roots are fixed when
its store is created: each root's marker (`.efs-engine`) records the root
set, its position and the count, and a restart with a root added, dropped
or reordered is refused. `efs-mgmt add-storage` was removed and is refused
(finding F11). More storage means another node.

`mkfs` initializes the single Raft export. Repeated application preserves an
existing root and its committed placement salt; it does not fail on an existing
name or create another named export. It takes exactly one argument, the node
address, and refuses a label. `efs-fuse` takes no export label either: a word
after the mountpoint is refused.

Applied-marker experiment (W93): `EFS_RAFT_APPLIED_INTERVAL_MS=10` is the
historical default. Explicit integers 10–1,000 select a best-effort checkpoint
cadence; 1,024 additional applied entries or clean shutdown force persistence.
These per-group restart hints remain behind a successful KV WAL barrier;
Raft/publication ACK durability is unchanged. Each applied entry now commits its
KV effects and node-local applied cursor in one atomic WAL record; restart uses
that validated cursor even if a separate checkpoint is retained. Failed scopes
do not advance application. Oversized entries refuse above 32,768 effect keys
or 64 MiB. Before downgrading to a binary without atomic cursors, shut down
cleanly to bring legacy hints current; crash downgrade is unsupported.
Keep the default pending sustained and recovery acceptance.
[Investigation and gates](../archive/status-20261011/w93-checkpoint-cadence-20261010.md).

GC scheduling experiment (W94/W97): `EFS_GC_BACKLOG_IDLE_MS` is unset by
default, retaining a 1,000-ms inter-pass idle. Explicit integers 20–1,000
shorten the idle when a leader's reap scan is capped, or a full fragment-GC
batch successfully commits ACKs and exhausts its budget with pending work,
and physical deletion makes progress in that pass. Empty scans and wholly blocked work retain the
normal backoff; partially blocked work can still accelerate if other deletions
succeed. Per-group scan/fragment/ACK budgets, deletion authority and durability
ordering remain unchanged. This is an opt-in experiment, not an overload fix
or production default. [AMD gates and limits](../status/amd-sustained-20261011.md#opt-in-gc-scheduling-candidate).

Metadata plane (Raft + KV):

```bash
./efs-mgmt raft-status 127.0.0.1:17432            # per-group term/commit/applied/voters
./efs-mgmt raft-change 127.0.0.1:17432 0 0x7      # voting set of one group
./efs-mgmt raft-dir  127.0.0.1:17432 <ino> begin|migrate|finish
```

`efs-mgmt` also has a `raft-<op>` for every metadata operation
(`raft-create`, `raft-lookup`, `raft-rename`, `raft-publish`, `raft-flock`, …).
Those bypass FUSE and are the fastest way to test one operation against one
node — that is what the smoke scripts under `tests/stress/` use.

## Writer threads and direct I/O

One writer pool is shared across all `--storage` paths; each daemon defaults
to eight writer workers, independent of CPU count. These workers overlap durable
file and directory I/O; increasing their count does not weaken durability.
`--writers <n>` overrides the total (maximum 64), and `--writers 0` runs PUTs
inline on the connection thread. New fragments go to
the path with the shortest writer queue. Each writer slot
preserves FIFO within a file and rotates that file’s queued chunks behind other
queued files after service. Busy streams cannot monopolize admission ahead of
a tiny write.

Fragment I/O is always `O_DIRECT` (D42). At startup efsd writes and reads
back one aligned page on every storage root and refuses a root whose
filesystem cannot do `O_DIRECT` (`storage root ... refused: fragment I/O
requires O_DIRECT`); there is no buffered fallback. The removed
`--direct-io` and `--no-direct-io` flags are refused.

`O_DIRECT` bypasses the page cache; it does not itself establish persistence:
acknowledged fragments still take explicit `fsync`/`syncfs` barriers.
The production fragment persistence gap is [W71](../backlog/work-items.md#w71).

## Profiling a server

`efsd --perf` attaches `perf record -g` to itself after it is listening and
writes `STORAGE/log/perf.data` (first root; `EFS_PERF_PATH` overrides);
`--strace` does the same with `strace -f -tt -T` to `STORAGE/log/strace.txt`
(`EFS_STRACE_PATH` overrides). SIGTERM stops the recorders first
(that is what writes the perf footer), then the daemon. Never start a
daemon under an external `strace` and quote its throughput.

## The fcstor test cluster

Everything above describes the product. The following is the recorded fcstor
test deployment (fcstor003–006 servers on port 19810, fcstor003–015 clients),
not a live inventory or the NUC/xorinox controller. Verify current topology and
use its operator runbook before running these historical entry points:

```bash
bash tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]
bash tests/cluster.sh start --fresh        # after tests/wipe_cluster.sh: seed + --join + raft-mkfs
bash tests/preflight.sh                    # read-only health check, run first
```

Rules for that cluster (build on the node, never in the NFS home; `pgrep
-x`, never `-f`; every long command in a screen) are in
`docs/operations/cluster-testing.md`.

## FUSE surface

Implemented entry points: `chmod`, `chown`, `truncate`, `rename`, `utime(2)`, `utimens`
(nanosecond mtimes so `rsync -a` is idempotent on a second pass).

This list is not blanket POSIX/concurrency acceptance: staged logical truncate
and public-path gates remain in the [queue](../status/README.md) and
[capability matrix](../status/spec-implementation.md).

With `EFS_COMMANDS=1` on the mount, each directory has a **lookup-only**
`.efs/` command directory (`.efs/ls`, `.efs/find`, `.efs/stats`, `.efs/help`;
[commands](../using/efs-commands.md)). `ls` does not list it, and a real
`.efs` entry takes precedence. The former `.stats` and `.find` virtual files
no longer exist (W92).

The old whole-table `efs-query` tool was removed on October 21, 2026. The
server still refuses its `QUERY_STATS` request as unsupported, and never
returns totals ([W80](../backlog/work-items.md#w80)). For server-owned
snapshots, use a mounted directory's `.efs/stats` command. For capacity, use
node usage or physical filesystem measurements.
