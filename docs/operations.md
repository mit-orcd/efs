# Operations

[Quick start](../README.md#quick-start) · [Failure tolerance](failure-tolerance.md) · [Design](design.md)

## Scripts

```text
./scripts/server.sh <addr:port> <path[,path...][:quota]> [join-addr:port] [efsd args...]
./scripts/server.sh stop <path[:quota]|addr:port>

./scripts/client.sh <server-addr:port> <mount-path> [export-name] [efs-fuse args...]
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

Several `--storage` roots (1–24) are allowed. New writes go to the path with
the shortest writer queue (fairness-weighted by bytes already assigned).
Overwrites stay on the existing path.

## Storage layout

Under each storage path:

| Dir | Contents |
|---|---|
| `data/` | fragments (file chunks and metadata pages) |
| `meta/` | export root (`EFSR` in `metadata.bin`), membership, `usage.bin` |
| `log/` | logs, PID file, optional `perf` output |

Fragment paths are sharded by inode (five base-10000 segments) so the export
does not grow one directory per file:

```text
data/exports/{export_id}/{d4}/{d3}/{d2}/{d1}/{d0}/{chunk>>10}/{chunk}.{frag}
```

Inode `2`, chunk `0`, fragment `0` →
`data/exports/1/0000/0000/0000/0000/0002/0/0.0`. Writes always use this
layout. Reads/unlinks also try the older flat
`data/exports/{id}/{ino}/{chunk>>10}/…` path.

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
client gets `ENOSPC`. See [failure tolerance](failure-tolerance.md#disk-full).

```bash
./efs-mgmt status 127.0.0.1:17432
```

`used` is updated on PUT/migrate and stored in `meta/usage.bin`. A full disk
scan runs only at startup if that file is missing, or after destroying an
export.

## Cluster management

```bash
./efs-mgmt status 127.0.0.1:17432
./efs-mgmt list-exports 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432 myexport [--chunk-size 128K]
./efs-mgmt destroy 127.0.0.1:17432 myexport
./efs-mgmt add-node 127.0.0.1:17433 127.0.0.1:17432
./efs-mgmt drain-node 127.0.0.1:17434     # move data off; stay in membership
./efs-mgmt undrain-node 127.0.0.1:17434   # accept new writes again
./efs-mgmt remove-node 127.0.0.1:17434    # leave (must be drained / empty)
./efs-mgmt shrink-quota 127.0.0.1:17432 2G
./efs-mgmt feature 127.0.0.1:17432 myexport show
```

`drain-node` and `shrink-quota` return immediately; migration runs in the
background. Watch `status` for `draining` / `drained` / `active`. A draining
node rejects new fragment PUTs. `remove-node` fails unless the node is empty.
`mkfs` fails if the name already exists.

## Writer threads and direct I/O

Each `--storage` path has a writer pool (default 8). `--writers 0` runs PUTs
inline on the connection thread.

Fragment I/O uses the page cache unless you pass `--direct-io` (`O_DIRECT`,
often better on local NVMe):

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /tmp/efs/s1 --direct-io
```

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
