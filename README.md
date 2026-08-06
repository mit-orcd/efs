# efs

A prototype distributed parallel filesystem in C. Three storage nodes form a
peer-to-peer cluster. Every 128 KiB file chunk is encoded into three 64 KiB
fragments using a 2+1 XOR erasure code, so any two fragments reconstruct the
original chunk. Directory and inode metadata are replicated to every node. A
FUSE client mounts the filesystem in user space, prefers local fragments when
co-located with a server, and transparently falls back to surviving nodes when a
fragment or node is unavailable.

All dependencies are vendored in this repository and built from source:
- `deps/blake3/` — Blake3 reference implementation
- `deps/libfuse/` — FUSE2 userspace library

## Building

```bash
make
```

Requires `gcc`, `make`, standard POSIX tools, and a Linux environment with the
kernel FUSE module available. The first build configures and compiles libfuse;
subsequent builds are incremental. The build produces:

- `efsd` — storage server daemon
- `efs-fuse` — FUSE client mount binary
- `efs-mgmt` — management CLI
- `efs-query` — query cluster metadata statistics

## Running a three-node cluster

The easiest way is to use the wrapper scripts in `scripts/`.

Start three servers. Each one needs a unique address, a storage path, and
optionally a quota. The second and third servers join the first one:

```bash
./scripts/server.sh 127.0.0.1:17432 /data/efs/s1:5T
./scripts/server.sh 127.0.0.1:17433 /data/efs/s2:5T 127.0.0.1:17432
./scripts/server.sh 127.0.0.1:17434 /data/efs/s3:5T 127.0.0.1:17432
```

The server creates three subdirectories under the storage path:

- `<storage>/data/` — fragment data (`data/exports/<name>/...`)
- `<storage>/meta/` — metadata (`meta/exports/<name>/metadata.bin`, `meta/cluster_nodes.bin`)
- `<storage>/log/` — server logs and profiling data

`server.sh` writes a PID file to `<storage>/log/efsd.pid` and redirects server
output to `<storage>/log/efsd.log`. When it starts, it prints the new PID and
the log path. It then waits one second; if `efsd` exits immediately (e.g. the
port is in use or the bind address is invalid), it prints the last log lines
and exits with a non-zero status.

If you run the script again for the same storage path, it kills the previous
`efsd` process and frees the port before starting a new one.

Old deployments that still store `exports/` and `cluster_nodes.bin` directly
under the storage root are automatically migrated to the new layout the next
time the server starts.

Create an export and mount it. The client only needs one server address; it will
discover the full cluster from that server:

```bash
./efs-mgmt mkfs 127.0.0.1:17432 myexport
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport
```

`mkfs` fails if the export name already exists on the cluster. Use
`list-exports` to check which exports already exist.

Now you can use `/mnt/efs` like a local filesystem:

```bash
echo "hello" > /mnt/efs/world.txt
cat /mnt/efs/world.txt
chmod 700 /mnt/efs/world.txt
```

The FUSE client supports `chmod`, `chown`, `truncate`, `rename`, and the old
`utime(2)` interface, so tools that preserve permissions, pre-allocate files,
or rename partial files (custom copy utilities, `cp -p`, `ecopy`, `rclone`,
etc.) can set mode, owner, size, rename, and modification time on files and
directories. The FUSE2 high-level API does not support the newer
`utimensat`/`futimens` calls, so those return `Function not implemented`.

If you run the FUSE client on the same host as one of the servers, it will
read and write the local fragment directly instead of sending it over the
network.

The address passed to `server.sh` must be a real local IP address assigned to
the host (e.g. `172.16.16.12:1981`), not the network address (`172.16.16.0`)
or `0.0.0.0`. Other nodes use this address to connect back. If the port is
already in use, `efsd` will now report the actual error (e.g. `Address already
in use`) before attempting to join the cluster.

You can also run the binaries directly. Start three servers on different ports
and storage paths:

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 --storage /data/efs/s1 &
./efsd --node-id 2 --addr 127.0.0.1 --port 17433 --storage /data/efs/s2 &
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage /data/efs/s3 &
```

Form the cluster with the management CLI and mount. You can pass all three
server addresses, or just one and let the client discover the rest:

```bash
./efs-mgmt add-node 127.0.0.1:17433 127.0.0.1:17432
./efs-mgmt add-node 127.0.0.1:17434 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432 myexport
./efs-fuse 127.0.0.1:17432 myexport /mnt/efs
```

## Auto-rejoin after restart

Each server persists the cluster membership list in its storage path as
`<storage>/cluster_nodes.bin`. After a restart, the server loads the saved node
list and automatically contacts one of its former peers to rejoin the cluster.
You do not need to pass `--join` again if the storage path and node identity are
unchanged.

For example, to restart server 3 after maintenance, just start it with the same
arguments:

```bash
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage /data/efs/s3 &
```

Server 3 will find server 1 and 2 in `cluster_nodes.bin`, send a `HELLO` to one
of them, receive the current membership, and pull any metadata it missed while it
was down. The other nodes continue to recognize it once the handshake completes.

If the former peers are not reachable, the server starts as a standalone node and
retries the rejoin in the background every few seconds. No data is lost: the
server keeps its local fragments and metadata, and it rejoins automatically when
the peers come back. This means you can run `server.sh` without worrying whether
the rest of the cluster is already up.

If you want to start a server as a fresh standalone node and ignore any stale
persisted membership list, use `--no-persist`:

```bash
./scripts/server.sh 172.16.16.12:1981 /scratch/efs:200G --no-persist
```

This prevents the server from loading or writing `cluster_nodes.bin`. It is
useful when the previous cluster no longer exists or the storage directory was
used by a different deployment.

## Storage quotas

Each `efsd` instance can be configured with a local storage quota. When the quota
is reached, the server rejects new fragment writes. The cluster reports full
when two or more nodes (the write quorum) have no space left.

Start a server with a quota. The value can be given as plain bytes or with a
suffix (`T`, `G`, `M`, `K` for TiB, GiB, MiB, KiB):

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /data/efs/s1 --quota 5T &
```

Check per-node usage and the cluster-wide full state:

```bash
./efs-mgmt status 127.0.0.1:17432
```

When the cluster is full, the FUSE client returns `ENOSPC` ("No space left on
device") for subsequent writes.

The FUSE client also reports capacity via `df`. With three servers of equal quota,
usable logical space is `2 * min_quota` because of the 2+1 encoding (1.5x physical
overhead). For example, three servers with `200G` each give about `400G` of
logical filesystem space:

```bash
$ df -h /mnt/efs
Filesystem      Size  Used Avail Use% Mounted on
efs-fuse        400G  1.0M  400G   1% /mnt/efs
```

If any server has no quota (`--quota` omitted), `df` cannot report a meaningful
total and shows `0`.

## Failure tolerance

Stop any one `efsd` process and the FUSE client continues to read files
correctly. Each chunk is encoded so that any two of the three fragments can
reconstruct the third, so the cluster survives the loss of one node or one
fragment.

## Cluster management

`efs-mgmt` provides status, export listing, and background-rebalancing
commands for the cluster.

### Listing exports

`list-exports` asks any server for the exports it knows about. Because metadata
is replicated, you can query any node:

```bash
./efs-mgmt list-exports 127.0.0.1:17432
```

Example output:

```text
Exports (2):
  id=1  name=myexport
  id=2  name=archive
```

### Removing a node

`remove-node` tells a server to migrate all of its fragments to the remaining
nodes and then leave the cluster. The command returns immediately while the
migration runs in the background.

```bash
./efs-mgmt remove-node 127.0.0.1:17434
```

The server at `127.0.0.1:17434` will move its data to the other two nodes,
broadcast a `node-left` notification, and exit. The remaining nodes update
their cluster list automatically.

### Shrinking a quota

`shrink-quota` reduces a node's storage quota. If the node is already using
more than the new limit, the server migrates fragments to other nodes until
its usage falls below the new quota.

```bash
./efs-mgmt shrink-quota 127.0.0.1:17432 2G
```

The amount uses the same suffix syntax as `--quota` (`T`, `G`, `M`, `K`). You
can monitor progress with `efs-mgmt status`.

## Direct I/O for flash-backed servers

Servers can optionally use `O_DIRECT` for fragment reads and writes, bypassing
the kernel page cache. This is useful for flash-backed nodes where double
caching is wasteful and lower latency is desired.

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /data/efs/s1 --direct-io &
```

When enabled, the server allocates 4096-byte aligned buffers and reads/writes
fragments directly from the device. `fsync()` is still used for durability.

## Performance profiling with `perf`

`slurm-jobs/profile.sh` is the usual way to collect profiles: it starts a
3-server + 1-client cluster on one compute node with `--perf`, drives a short
write/read/metadata load, and writes `perf report` text under
`~/orcd/scratch/efs/profile/<jobid>/` (also linked as `.../profile/latest`).

```bash
sbatch --wait slurm-jobs/profile.sh
less ~/orcd/scratch/efs/profile/latest/client.report.txt
less ~/orcd/scratch/efs/profile/latest/server.report.txt
```

Both the server and the FUSE client can also collect a `perf record` profile by
adding `--perf`. The binary detects the flag, forks a child that runs
`perf record -g -p <pid>` against the current process, and continues running
normally. The child is stopped when the server or client exits.

Server profile is written to the log directory:

```bash
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 \
       --storage /data/efs/s1 --perf &
```

This produces `/data/efs/s1/log/perf.data` after the process exits. You can
analyze it with:

```bash
perf report -i /data/efs/s1/log/perf.data
```

With the wrapper script, pass `--perf` as an extra argument:

```bash
./scripts/server.sh 127.0.0.1:17432 /data/efs/s1:5T --perf
```

For the FUSE client, profiles are written to a temporary directory:

```bash
./efs-fuse 127.0.0.1:17432 myexport /mnt/efs --perf
```

The profile lands in `/tmp/efs-fuse-perf-<pid>/perf.data`. With the wrapper:

```bash
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport --perf
```

If `perf` is not installed, the binary prints a warning and continues without
profiling.

## Querying metadata

`efs-query` asks a server for aggregate file statistics. Because metadata is
replicated across the cluster, any server can answer. The tool reports the total
number of files, the total logical bytes stored, and a per-user breakdown:

```bash
./efs-query 127.0.0.1:17432
```

Example output:

```text
Total files: 3
Total bytes: 1.23 MiB (1293948 bytes)
Users:       2

Per-user breakdown:
UID              Files                Bytes
1000                 2             1.00 MiB
1001                 1           241.00 KiB
```

The per-user totals come from the `uid` stored on each inode. Files created
through the FUSE client automatically inherit the caller's uid and gid. Files
created by non-FUSE clients (such as the test suite) show uid `0` unless the
caller sets the owner explicitly.

## Testing

```bash
make test
```

This runs the unit tests, the integration test, and the FUSE end-to-end test:

- `test_erasure` — erasure coding encode/decode
- `test_placement` — deterministic chunk placement and locality detection
- `test_integration` — starts three servers, forms a cluster, writes a file,
  reads it back, kills one server, and verifies the file is still readable
- `test_quota` — starts three servers with a small per-node quota, verifies the
  first write succeeds and the cluster reports full, then verifies a second
  write is rejected with a quota error
- `test_migrate` — verifies `shrink-quota` migrates data off an over-quota node
  and `remove-node` migrates a server's fragments to the remaining nodes before
  the server exits
- `test_directio` — starts three servers with `--direct-io`, writes and reads a
  file, and verifies a surviving-node read after killing one server
- `test_rejoin` — starts three servers, writes a file, reboots one server
  without `--join`, and verifies it auto-rejoins the cluster and the file is
  still readable
- `test_query` — starts three servers, creates files owned by different UIDs,
  and verifies that `efs-query` reports correct totals and per-user statistics
- `test_list_exports` — starts three servers, creates two exports, and verifies
  that `efs-mgmt list-exports` reports them
- `test_rw.sh` — FUSE client smoke test with a real mount; exercises chmod,
  chown, utime, truncate, and rename in addition to read/write and node failure

## Slurm cluster smoke test

`slurm-jobs/run.sh` is a reusable harness that builds the code and runs a
three-server efs cluster plus two client test jobs, all via `sbatch`. Nothing
is compiled or executed on the login node: the build itself runs as a Slurm
job (`slurm-jobs/build.sh`), and each server and client is a separate job on
the `mit_quicktest` partition with minimal resources (1 core, no `--exclusive`).
Server data and FUSE mountpoints live under node-local `/scratch`, while
anything that must be visible across all nodes (server address/state files and
all job logs) lives on the shared parallel filesystem under
`/orcd/scratch/orcd/001/erbmi1/efs` (i.e. `~/orcd/scratch/efs`), as required by
the project rules (see `.cursor/skills/slurm-execution/SKILL.md`).

Run the harness from the repo root:

```bash
./slurm-jobs/run.sh
```

It will:

1. Build the efs binaries via a Slurm job (`efs-build`).
2. Cancel any previous efs Slurm jobs from this harness.
3. Submit three server jobs (`efs-s1`, `efs-s2`, `efs-s3`) on distinct ports
   (1981, 1982, 1983) so they can share a node.
4. Wait up to 5 minutes for each server to write its address to a shared state
   file (`~/orcd/scratch/efs/state/`), checking Slurm status once per minute and
   failing fast if a server job dies.
5. Submit two client jobs (`efs-client1`, `efs-client2`).
6. Wait for the client jobs to finish, polling `squeue` no faster than once
   per minute.
7. Tear down the server jobs and print the log directory.

The client jobs mount the FUSE filesystem and exercise standard tools:
`dd`, `cp`, `mv`, `sed`, `cat`, `rsync`, and `rclone` (loaded via `ml rclone`).
These are smoke tests with tiny amounts of data (a few KB), and each operation
is wrapped in a 15-second timeout so a hung operation fails fast instead of
blocking the job.

Logs are written to the shared filesystem under `~/orcd/scratch/efs/logs/`:

```bash
cat ~/orcd/scratch/efs/logs/client1-*.out
cat ~/orcd/scratch/efs/logs/client1-fuse.log
cat ~/orcd/scratch/efs/logs/s1-*.out
```

A full run takes about a minute when the queue is free.

## Design

- **Chunk size:** 128 KiB
- **Erasure coding:** 2 data fragments + 1 parity fragment (D1, D2, P = D1 XOR D2)
- **Checksums:** Blake3 per fragment
- **Placement:** deterministic based on `(inode, chunk_index)`
- **Inode allocation:** each FUSE client allocates inode numbers from its own
  per-mount namespace (`(tag << 40) | counter`), so concurrent clients never
  assign the same inode to different files and their chunk placements can never
  collide. The server merges each client's metadata update additively, so
  concurrent creates/writes do not drop each other's files.
- **Hot-path notes (from `perf`):** the FUSE client keeps a persistent TCP
  connection per server; writes only touch overlapping chunks; FUSE negotiates
  `big_writes` / 128 KiB `max_write` so the kernel does not force 4 KiB RMWs;
  fragment checksums are verified once on PUT and stored in a `.sum` sidecar
  so GET does not re-hash; `efs_export_merge` uses an open-addressing index.
- **Replication:** metadata is written to all three nodes; chunk writes succeed
  with two of three acks
- **Quotas:** per-server limit configured with `--quota`; cluster reports full
  when the write quorum has no space available
- **Direct I/O:** optional `--direct-io` flag bypasses the page cache for
  fragment reads and writes on flash-backed servers
- **Protocol:** length-prefixed TCP frames

## License

MIT. See [LICENSE](LICENSE).
