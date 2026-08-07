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

`server.sh` / `client.sh` usage:

```text
./scripts/server.sh <addr:port> <path[:quota]> [join-addr:port] [extra args...]
./scripts/server.sh stop <path[:quota]|addr:port>

./scripts/client.sh <server-addr:port> <mount-path> [export-name] [extra args...]
./scripts/client.sh stop <mount-path>
```

Examples:

```bash
./scripts/server.sh stop /tmp/efs/s1
./scripts/server.sh stop 127.0.0.1:17432
./scripts/client.sh stop /mnt/efs
```

The bind address must be a real local IP (not `0.0.0.0` or a network address).
Other nodes use it to connect back.

Each server creates under its storage path:

- `data/` — fragment data (file chunks and 2+1 metadata table pages)
- `meta/` — export root (`EFSR` in `metadata.bin`) and cluster membership
- `log/` — logs, PID file, optional `perf` output

Fragment files are sharded by inode so an export root never holds one directory
per file. Each inode becomes five base-10000 path segments (`0000`–`9999`),
least-significant group last, then the existing chunk bucket:

```text
data/exports/{export_id}/{d4}/{d3}/{d2}/{d1}/{d0}/{chunk>>10}/{chunk}.{frag}
```

Example: inode `2`, chunk `0`, fragment `0` →
`data/exports/1/0000/0000/0000/0000/0002/0/0.0`. Five segments uniquely encode
any 64-bit inode (client namespaces use high bits). Writes always use this
layout; reads/unlinks also fall back to the legacy flat
`data/exports/{id}/{ino}/{chunk>>10}/…` path for older data.

Export metadata is hybrid: a tiny fully-replicated root (generation, `next_ino`,
page checksums) plus bulk inode/chunk tables packed into 128 KiB pages, encoded
with the same 2+1 XOR scheme as file data under reserved inode
`EFS_META_TABLE_INO`. Durability matches data (survive one node loss).

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

Each server keeps a cached `used` counter (updated on PUT/migrate, persisted as
`meta/usage.bin`) so status stays fast; a full disk scan runs only at startup
when that file is missing, or after destroying an export.

`df` on the mount reports logical capacity. With three equal quotas, usable space
is about `2 * min_quota` (2+1 encoding). If any server has no quota, `df` shows `0`.

## Failure tolerance

Stop any one `efsd` and the client can still read files. Any two of the three
fragments reconstruct a chunk.

## Cluster management

```bash
./efs-mgmt list-exports 127.0.0.1:17432
./efs-mgmt drain-node 127.0.0.1:17434       # migrate local data to peers; stay in cluster
./efs-mgmt undrain-node 127.0.0.1:17434     # accept new writes again after drain
./efs-mgmt remove-node 127.0.0.1:17434      # leave cluster (requires prior drain)
./efs-mgmt shrink-quota 127.0.0.1:17432 2G  # migrate until under new limit
```

`drain-node` empties a node but keeps it in the membership (useful for
maintenance or before remove). While draining or drained, the node rejects new
fragment PUTs. `remove-node` fails unless the node is already empty — run
`drain-node` first. `drain-node` and `shrink-quota` return immediately;
migration runs in the background. Watch progress with `efs-mgmt status`
(shows `draining` / `drained` / `active`).

`mkfs` fails if the export name already exists.

## Direct I/O

Fragment reads/writes use the page cache by default. Pass `--direct-io` for
`O_DIRECT` (often better on flash-backed node-local storage):

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /tmp/efs/s1 --direct-io &
```

## NUMA affinity

Soft NUMA pinning (storage writers and FUSE NIC locality) is **off by default**.
Set `EFS_NUMA_AFFINITY=1` (or `on`/`true`) to enable discovery and pinning.

Fragment PUTs are executed on a dedicated writer thread pool (default 8
threads). Override with `--writers <n>` (`0` runs writes inline on the
connection thread).

## Directory rollup stats

Each directory exposes a virtual read-only file `.stats`. It is **lookup-only**:
`ls` / `readdir` of the directory does not list it; access by explicit path
(`cat dir/.stats`, `stat dir/.stats`) still works. The file is not a real inode
and is not counted in the rollup numbers it reports. `cat dir/.stats` prints
immediate and subtree file/dir counts, byte totals, and min/max timestamps
(`min`/`max` of atime, ctime, mtime per entry). Rollups are maintained
incrementally in metadata so reads stay cheap. Creating a real inode named
`.stats` is rejected.

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

Supported: `chmod`, `chown`, `truncate`, `rename`, `utime(2)`, and `utimens`
(nanosecond mtimes so `rsync -a` is idempotent on a second pass). Each directory
also has a lookup-only virtual `.stats` file (see above).

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
