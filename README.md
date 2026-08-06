# efs

A prototype distributed parallel filesystem in C. Three storage nodes form a
peer-to-peer cluster. Every 128 KiB file chunk is encoded into three 64 KiB
fragments (2+1 XOR), so any two fragments reconstruct the original. Metadata is
replicated to every node. A FUSE client mounts the filesystem in user space.

Dependencies are vendored and built from source (`deps/blake3/`, `deps/libfuse/`).

## Quick start

Build:

```bash
make
```

Start three servers (second and third join the first):

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G
./scripts/server.sh 127.0.0.1:17433 /tmp/efs/s2:5G 127.0.0.1:17432
./scripts/server.sh 127.0.0.1:17434 /tmp/efs/s3:5G 127.0.0.1:17432
```

Create an export and mount it:

```bash
./efs-mgmt mkfs 127.0.0.1:17432 myexport
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport
```

Use it like a local filesystem:

```bash
echo "hello" > /mnt/efs/world.txt
cat /mnt/efs/world.txt
```

Run tests:

```bash
make test
```

---

## What you get

| Binary      | Role                                      |
|-------------|-------------------------------------------|
| `efsd`      | storage server daemon                     |
| `efs-fuse`  | FUSE client                               |
| `efs-mgmt`  | cluster management CLI                    |
| `efs-query` | aggregate file / per-user statistics      |

Requires `gcc`, `make`, and Linux with the FUSE kernel module.

## Server and storage layout

`server.sh` usage:

```text
./scripts/server.sh <addr:port> <path[:quota]> [join-addr:port] [extra args...]
```

The bind address must be a real local IP (not `0.0.0.0` or a network address).
Other nodes use it to connect back.

Each server creates under its storage path:

- `data/` — fragment data
- `meta/` — metadata and cluster membership
- `log/` — logs, PID file, optional `perf` output

`server.sh` restarts cleanly if you run it again for the same path (kills the
old process first). If `efsd` exits immediately, it prints the last log lines.

### Running binaries directly

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 --storage /tmp/efs/s1 &
./efsd --node-id 2 --addr 127.0.0.1 --port 17433 --storage /tmp/efs/s2 &
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage /tmp/efs/s3 &

./efs-mgmt add-node 127.0.0.1:17433 127.0.0.1:17432
./efs-mgmt add-node 127.0.0.1:17434 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432 myexport
./efs-fuse 127.0.0.1:17432 myexport /mnt/efs
```

The client only needs one server address; it discovers the rest.

If the FUSE client runs on the same host as a server, it prefers the local
fragment over the network.

## Auto-rejoin after restart

Each server saves cluster membership under its storage path. After a restart
with the same storage path and node identity, it rejoins automatically — you do
not need `--join` again:

```bash
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage /tmp/efs/s3 &
```

If peers are down, it starts standalone and retries in the background. Local
data is kept.

To ignore a saved membership list and start fresh:

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G --no-persist
```

## Quotas

Pass a quota with a size suffix (`T`, `G`, `M`, `K`):

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G
# or
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /tmp/efs/s1 --quota 5G &
```

When two or more nodes (write quorum) are out of space, the cluster is full and
the FUSE client returns `ENOSPC`.

Check status:

```bash
./efs-mgmt status 127.0.0.1:17432
```

`df` on the mount reports logical capacity. With three equal quotas, usable space
is about `2 * min_quota` (2+1 encoding). If any server has no quota, `df` shows `0`.

## Failure tolerance

Stop any one `efsd` and the client can still read files. Any two of the three
fragments reconstruct a chunk.

## Cluster management

```bash
./efs-mgmt list-exports 127.0.0.1:17432
./efs-mgmt remove-node 127.0.0.1:17434      # migrate off, then leave
./efs-mgmt shrink-quota 127.0.0.1:17432 2G  # migrate until under new limit
```

`remove-node` and `shrink-quota` return immediately; migration runs in the
background. Watch progress with `efs-mgmt status`.

`mkfs` fails if the export name already exists.

## Direct I/O

For flash-backed storage, bypass the page cache:

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /tmp/efs/s1 --direct-io &
```

## Querying metadata

```bash
./efs-query 127.0.0.1:17432
```

Reports total files, total bytes, and a per-user breakdown (from inode `uid`).

## Profiling

Add `--perf` to the server or client. It runs `perf record` against itself and
writes `perf.data` when the process exits (server: under `<storage>/log/`;
client: under `/tmp/efs-fuse-perf-<pid>/`).

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G --perf
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport --perf

perf report -i /tmp/efs/s1/log/perf.data
```

If `perf` is missing, the process continues without profiling.

There is also a Slurm helper at `slurm-jobs/profile.sh` that starts a short
cluster, runs load, and writes reports under a scratch directory.

## FUSE notes

Supported: `chmod`, `chown`, `truncate`, `rename`, and the older `utime(2)`
interface. The FUSE2 high-level API does not implement `utimensat`/`futimens`
(`ENOSYS`).

## Testing

`make test` runs:

- unit tests (`test_erasure`, `test_placement`)
- cluster integration, quota, migrate, direct-io, rejoin, query, list-exports
- `test_rw.sh` — real FUSE mount smoke test

## Slurm harness

`slurm-jobs/run.sh` builds on a compute node and runs a three-server cluster
plus two client smoke jobs via `sbatch`. Server data and mounts use node-local
`/scratch`; shared state and logs go on a shared filesystem path configured in
the scripts.

```bash
./slurm-jobs/run.sh
```

Client jobs exercise small `dd` / `cp` / `rsync`-style operations with short
timeouts. See scripts under `slurm-jobs/` for details.

## Design

- **Chunk size:** 128 KiB
- **Erasure coding:** 2 data + 1 parity (`P = D1 XOR D2`)
- **Checksums:** Blake3 per fragment
- **Placement:** deterministic from `(inode, chunk_index)`
- **Inodes:** each FUSE client allocates from its own mount namespace so clients
  never collide
- **Replication:** metadata to all nodes; chunk writes need two of three acks
- **Protocol:** length-prefixed TCP frames

## License

MIT. See [LICENSE](LICENSE).
