# Operations

[Quick start](../README.md#quick-start) ·
[Failure tolerance](arch/failure-tolerance.md) ·
[Architecture](architecture.md) · [Testing](testing.md)

## Scripts

```text
./scripts/server.sh [--perf] <addr:port> <path[,path...][:quota]> [join-addr:port] [efsd args...]
./scripts/server.sh stop <path[:quota]|addr:port>

./scripts/client.sh [--perf] <server-addr:port> <mount-path> [efs-fuse args...]
./scripts/client.sh stop <mount-path>
```

The bind address must be a **real local IP** (not `0.0.0.0`). Peers connect
back to it.

`server.sh` kills a previous `efsd` on the same path/port before starting.
If `efsd` exits immediately, it prints the last log lines.

The client needs **one** server address; it discovers the rest. If it runs on
the same host as a server, it prefers the local fragment.

`client.sh` daemonizes and returns **only after** the mount is serving
(FUSE_INIT completed and `stat` of the mountpoint works). `/proc/mounts`
listing the path is not enough. Pass `-f` to stay in the foreground.

## Running the binaries yourself

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 --storage /tmp/efs/s1 &
./efsd --node-id 2 --addr 127.0.0.1 --port 17433 --storage /tmp/efs/s2 &
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage /tmp/efs/s3 &

./efs-mgmt add-node 127.0.0.1:17433 127.0.0.1:17432
./efs-mgmt add-node 127.0.0.1:17434 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432 myexport
./efs-fuse 127.0.0.1:17432 myexport /mnt/efs
```

`mkfs` is an alias for `raft-mkfs` and needs a **quorum of the metadata Raft
voters already running**, so start every server first and only then create the
export. There is no "start the primary, mkfs, then let the others join" order.

Several `--storage` roots (1–24) are allowed. New writes go to the path with
the shortest writer queue (fairness-weighted by bytes already assigned).
Overwrites stay on the existing path.

## Storage layout

Under each storage path:

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

A fragment file is the 64 KiB payload followed by a 4 KiB tail page that
holds its 32-byte digest (one `O_DIRECT` write; no sidecar `.sum`). Reads
verify against that tail. Fragments written before Oct 1 2026 have the
sidecar layout and do not verify; a wipe + `raft-mkfs` is the upgrade.

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

Ignore a saved membership list:

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

Suffixes: `T`, `G`, `M`, `K`. When two or more nodes are out of space, the
client gets `ENOSPC`.

```bash
./efs-mgmt status 127.0.0.1:17432
```

`used` is updated on PUT/migrate and stored in `meta/usage.bin`. A full disk
scan runs only at startup if that file is missing, or after destroying an
export.

## Cluster management

```bash
./efs-mgmt status 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432 myexport          # alias for raft-mkfs
./efs-mgmt add-node 127.0.0.1:17433 127.0.0.1:17432
./efs-mgmt add-storage 127.0.0.1:17432 /tmp/efs/s1b
./efs-mgmt shrink-quota 127.0.0.1:17432 2G
```

`shrink-quota` returns immediately; migration runs in the background. `mkfs`
fails if the name already exists.

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

## Profiling a server

`efsd --perf` attaches `perf record -g` to itself after it is listening and
writes `/tmp/efs-perf/efsd.data`; `--strace` does the same with `strace -f
-tt -T` to `/tmp/efs-perf/efsd.strace`. SIGTERM stops the recorders first
(that is what writes the perf footer), then the daemon. Never start a
daemon under an external `strace` and quote its throughput.

## The fcstor test cluster

Everything above is the product. The dedicated test cluster (fcstor003–006
servers on port 19810, fcstor003–015 clients) has one entry point:

```bash
bash tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]
bash tests/cluster.sh start --fresh        # after tests/wipe_cluster.sh: seed + --join + raft-mkfs
bash tests/preflight.sh                    # read-only health check, run first
```

Rules for that cluster (build on the node, never in the NFS home; `pgrep
-x`, never `-f`; every long command in a screen) are in
`.cursor/rules/efs-fcstor-deploy.mdc`.

## FUSE surface

Supported: `chmod`, `chown`, `truncate`, `rename`, `utime(2)`, `utimens`
(nanosecond mtimes so `rsync -a` is idempotent on a second pass).

Each directory has a **lookup-only** virtual `.stats` file (`cat dir/.stats`).
`ls` does not list it. It is not a real inode. Creating a file named `.stats`
is rejected. Contents: immediate and subtree file/dir counts, bytes, min/max
timestamps.

```bash
./efs-query 127.0.0.1:17432
```

prints total files, total bytes, and a per-uid breakdown.
