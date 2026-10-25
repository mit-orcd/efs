# Quick start — three local servers

**Accepted deployment contract (D36, Oct 9):** three nodes are the minimum;
four nodes provide the maximum two-node-loss safety target; additional nodes
scale volume and performance across all dimensions. Today's fixed 2+1 data and
three-voter metadata groups do not implement that four-node guarantee. See
[binding requirement and implementation gates](../status/node-count-protection-20261009.md); preserve the requirement
rather than deriving the design from current code.

[Docs index](../README.md) · [Power user](power-user.md) ·
[Operations](../operations/operations.md) ·
[Architecture](../how-it-works/architecture.md)

Run from a built checkout on Linux with libfuse3 and a usable `/dev/fuse`.
The build requires libibverbs even for TCP; RDMA additionally needs an active
verbs device. Python 3.9 or newer and Linux pidfd support are required by the
process-control wrappers. FUSE needs `fusermount3` and permission to mount at
the selected path. See the [build prerequisites](../../README.md#build-and-start).
Use free ports 17432–17434 and three empty storage roots for this disposable
example; the wrapper retires an existing daemon on a reused port/path.
The roots must be on a filesystem that supports `O_DIRECT` (ext4, XFS; not
an older tmpfs) with free space above its startup reserve: efsd keeps 20% of
the filesystem (at least 1 GiB) free and refuses to start when nothing is
left above that. A small tmpfs `/tmp` fails both checks, so the example
uses a directory under `$HOME` (set `EFS_DEMO_DIR` to use another disk).
The quota is automatic (the free space above the reserve); the three servers
share one filesystem here, so their quotas overlap: this is a demonstration,
not a capacity layout.

The first server creates the cluster; the second and third join it. The
cluster forms by itself once three servers are there (node ids come from the
cluster: the first is 1, the others follow in address order):

```bash
export EFS_TRANSPORT=tcp
EFS_DEMO_ROOT=$(mktemp -d "${EFS_DEMO_DIR:-$HOME}/efs-demo.XXXXXX")
./scripts/server.sh 127.0.0.1:17432 "$EFS_DEMO_ROOT/s1"
./scripts/server.sh 127.0.0.1:17433 "$EFS_DEMO_ROOT/s2" 127.0.0.1:17432
./scripts/server.sh 127.0.0.1:17434 "$EFS_DEMO_ROOT/s3" 127.0.0.1:17432
```

There is no node id, member count or cluster file to give (efsd refuses the
removed `--node-id` and `EFS_MD_RAFT_N`). The first joiner's `server.sh`
prints `WARNING: join to 127.0.0.1:17432 was requested but no join success
was logged yet`: that is expected, the join completes when the third server
arrives. Until then the servers say `unready: formation needs 3 members` in
`$EFS_DEMO_ROOT/s*/log/efsd.log`; with the third the logs show
`raft-host: formed: 3 members` (within a few seconds). A server started
later with the address of any member (or `join` instead of an address, to
find the cluster on the local subnet) is added online.
`./efs-mgmt status 127.0.0.1:17432` lists the members, the usable capacity,
`Cluster state: OK` and `Protection: full`. `mkfs` below is optional: it
forms at once if the automatic formation has not happened yet, and changes
nothing afterwards.

Mount:

```bash
./efs-mgmt mkfs 127.0.0.1:17432          # initialize the single export
mkdir -p "$EFS_DEMO_ROOT/mnt"
./scripts/client.sh 127.0.0.1:17432 "$EFS_DEMO_ROOT/mnt"
echo hello > "$EFS_DEMO_ROOT/mnt/world.txt"
cat "$EFS_DEMO_ROOT/mnt/world.txt"
```

Repeated `mkfs` preserves an existing root/salt; after an uncertain outcome,
inspect `raft-status` before retrying against the same intended cluster. It
is not a wipe or a named-export creation command. The mount also has the
`.efs` command directory in every directory (`"$EFS_DEMO_ROOT/mnt/.efs/help"`),
because the cluster commands-policy allows it by default.

Stop the client successfully before stopping any server. A failed ordinary
client stop retains unresolved writes; keep the servers up to resolve them:

```bash
./scripts/client.sh stop "$EFS_DEMO_ROOT/mnt"
./scripts/server.sh stop 127.0.0.1:17432
./scripts/server.sh stop 127.0.0.1:17433
./scripts/server.sh stop 127.0.0.1:17434
```

`server.sh --perf` / `client.sh --perf --strace` attach recorders and
write `flat.txt` / `by_thread.txt` / `callers.txt` at `stop`.
`EFS_TRANSPORT=auto|tcp|rdma` defaults to auto. RDMA supports native
InfiniBand (active port/LID) and RoCE (active Ethernet verbs device/GID).
For RoCE, set `EFS_RDMA_DEV` and optionally `EFS_RDMA_GID_INDEX` on every
participating daemon/client; [power-user transport setup](power-user.md#mounting-and-transport)
shows an explicit example. GPU device-memory DMA remains unvalidated.
Fragment I/O is always `O_DIRECT`; a storage root whose filesystem cannot do it is refused at startup.
See [script shutdown limits](../operations/operations.md#scripts) before reusing a
server path or stopping a busy server.
Restarting a server on its existing storage needs no join address;
membership is persisted.
