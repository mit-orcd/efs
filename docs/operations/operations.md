# Operations

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
Use explicit `--node-id 1..N` and `EFS_MD_RAFT_N=3|4`: the wrapper's derived
address ID is not a valid small-cluster Raft configuration. If startup exits
immediately, the wrapper prints the last log lines.

The client needs **one** server address; it discovers the rest. If it runs on
the same host as a server, it prefers the local fragment.

`client.sh` daemonizes and returns **only after** the mount is serving
(FUSE_INIT completed and `stat` of the mountpoint works). `/proc/mounts`
listing the path is not enough. Pass `-f` to stay in the foreground.

Ordinary client stop quiesces mutations, attempts a bounded drain, and refuses
to detach unresolved writes. If stop fails, keep the servers available and
resolve the failure. `--force-discard` explicitly permits loss and lazy detach;
see the [controlled-stop contract](../../README.md#writeback-and-safe-shutdown). Signals and
external unmount bypass that contract.

## Transport and storage mode

TCP and native-IB/RoCE RDMA are independent of buffered/direct fragment I/O.
See [transport selection](../using/power-user.md#mounting-and-transport) for
verbs device/GID settings. Use the same intended transport on servers and
clients when validating a configuration, including clients remounted during
cold verification. AMD's release evidence uses RXE software RoCE on one host.

The portal shown in the [README preview](../../README.md#operator-portal-preview)
is an external tool; its code is not shipped in this repository. For bundled
status inspection use `efs-mgmt status`, process/log inspection and the GC
counters. “Heal idle” is not proof of an implemented fragment repair owner.

## Running the binaries yourself

```bash
# Disposable empty roots and free ports; do not reuse an active cluster here.
export EFS_TRANSPORT=tcp EFS_MD_RAFT_N=3
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 --storage /tmp/efs/s1 &
./efsd --node-id 2 --addr 127.0.0.1 --port 17433 --storage /tmp/efs/s2 --join 127.0.0.1:17432 &
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage /tmp/efs/s3 --join 127.0.0.1:17432 &

# Wait for all three listeners and membership; inspect status before mkfs.
./efs-mgmt status 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432
mkdir -p /mnt/efs
./efs-fuse 127.0.0.1:17432 fs /mnt/efs
```

`mkfs` is an alias for `raft-mkfs` and needs a **quorum of the metadata Raft
voters already running**, so start every server first and only then create the
export. There is no "start the primary, mkfs, then let the others join" order.

Several `--storage` roots (1–24) are allowed. New writes go to the path with
the shortest writer queue (fairness-weighted by bytes already assigned).
Overwrites stay on the existing path.

## Storage layout

Data lives under each storage path; membership, usage snapshots and wrapper
logs/PID files use the first root. Metadata uses the first root unless
`--meta-storage` selects another root:

| Dir | Contents |
|---|---|
| `data/` | fragments (file chunks) |
| `mdraft/` | metadata: this node's Raft log, KV segments, applied index per group |
| `meta/` | membership (`cluster_nodes.bin`) and `usage.bin` |
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
Identity-bound fragment format and repair remain [W75](../backlog/work-items.md#w75).

The former advice to wipe an older sidecar store as an upgrade had no verified
migration gate. Preserve existing stores and establish format compatibility
and a verified data migration before any destructive procedure. `mkfs` is
initialization, not a fragment-format converter.

`--meta-storage <root>` puts `mdraft/` on a root the fragment writers do not
use. Default is the first `--storage` root; the flag does not move an
existing `mdraft/`.

## Auto-rejoin

Membership is saved under the storage path. Restart with the **same path and
node id** and the server rejoins by itself — no `--join`:

```bash
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage /tmp/efs/s3 &
```

If peers are down it starts standalone and retries in the background. Local
data is kept.

Disable loading/saving the membership list (not a data or metadata wipe):

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G --no-persist
```

## Quotas

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G
# or
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /tmp/efs/s1 --quota 5G
```

Suffixes: `T`, `G`, `M`, `K`. The quota belongs to the node across its storage
roots, not independently to each path. A PUT returns QUOTA after at least two
stripe quota failures, mapping to application `ENOSPC`. One quota-rejected
fragment with two ACKs can still return success; W42 tracks protection/repair
acceptance for that case.

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
./efs-mgmt add-node 127.0.0.1:17433 127.0.0.1:17432
./efs-mgmt add-storage 127.0.0.1:17432 /tmp/efs/s1b
./efs-mgmt shrink-quota 127.0.0.1:17432 2G
```

`shrink-quota <node> <amount>` subtracts the amount from the running node
quota immediately and refuses a result below current usage. It does not start
background migration. The current CLI misleadingly says migration started
([W70](../backlog/work-items.md#w70)); do not treat that text as evacuation
progress or a persistence guarantee.

`mkfs` initializes the single Raft export. Repeated application preserves an
existing root and its committed placement salt; it does not fail on an existing
name or create another named export. The current CLI help still advertises a
name argument, which the handler silently ignores ([W79](../backlog/work-items.md#w79)).
Use only the seed argument shown above. The FUSE binary still requires an
export-name token (`fs`, as used by `client.sh`); its current bootstrap builds
a local shell using that token, rather than selecting a separately created export.

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

One writer pool is shared across all `--storage` paths; the default thread
count is `nproc` minus the standing helper threads. `--writers <n>` sets it,
`--writers 0` runs PUTs inline on the connection thread. New fragments go to
the path with the shortest writer queue.

Fragment I/O uses the page cache unless you pass `--direct-io` (`O_DIRECT`,
often better on local NVMe):

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /tmp/efs/s1 --direct-io
```

`O_DIRECT` bypasses the page cache; it does not itself establish persistence.
The production fragment persistence gap is [W71](../backlog/work-items.md#w71).

## Profiling a server

`efsd --perf` attaches `perf record -g` to itself after it is listening and
writes `/tmp/efs-perf/efsd.data`; `--strace` does the same with `strace -f
-tt -T` to `/tmp/efs-perf/efsd.strace`. SIGTERM stops the recorders first
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

Each directory has a **lookup-only** virtual `.stats` file (`cat dir/.stats`).
`ls` does not list it. It is not a real inode. Creating a file named `.stats`
is rejected. Contents: immediate and subtree file/dir counts, bytes, min/max
timestamps.

```bash
./efs-query 127.0.0.1:17432
```

currently prints zero-filled placeholder totals: the legacy whole-table query
was retired and its server handler no longer calculates them
([W80](../backlog/work-items.md#w80)). Do not use this output to infer that a
cluster is empty, that unlink reclaimed space, or that per-user usage is zero.
Use node usage/physical filesystem measurements for capacity and inspect
directory `.stats` separately; those have different scopes.
