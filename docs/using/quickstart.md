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

Start **all** servers first: `mkfs` commits the members that have joined at
that moment as the cluster membership; the second and third join the first:

```bash
export EFS_TRANSPORT=tcp
EFS_DEMO_ROOT=$(mktemp -d /tmp/efs-demo.XXXXXX)
./scripts/server.sh 127.0.0.1:17432 "$EFS_DEMO_ROOT/s1:5G" --node-id 1 --no-direct-io
./scripts/server.sh 127.0.0.1:17433 "$EFS_DEMO_ROOT/s2:5G" 127.0.0.1:17432 --node-id 2 --no-direct-io
./scripts/server.sh 127.0.0.1:17434 "$EFS_DEMO_ROOT/s3:5G" 127.0.0.1:17432 --node-id 3 --no-direct-io
```

Node IDs must be 1..N with no gaps; specify them rather than relying on the
wrapper's address-derived ID. The member count and the metadata voters come
from the membership `mkfs` commits, so there is no count to configure (efsd
refuses the removed `EFS_MD_RAFT_N`). Confirm all three logs under
`$EFS_DEMO_ROOT/s*/log/efsd.log` show listening and
`./efs-mgmt status 127.0.0.1:17432` lists all three nodes before
initialization: `mkfs` refuses fewer than three and names the missing ones,
and a node that joins after `mkfs` is refused (adding a node to a formed
cluster is not implemented yet).

Initialize the single export through one seed, then mount:

```bash
./efs-mgmt mkfs 127.0.0.1:17432          # initialize the single export
mkdir -p "$EFS_DEMO_ROOT/mnt"
./scripts/client.sh 127.0.0.1:17432 "$EFS_DEMO_ROOT/mnt"
echo hello > "$EFS_DEMO_ROOT/mnt/world.txt"
cat "$EFS_DEMO_ROOT/mnt/world.txt"
```

Repeated `mkfs` preserves an existing root/salt; after an uncertain outcome,
inspect `raft-status` before retrying against the same intended cluster. It
is not a wipe or a named-export creation command.

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
Use `--direct-io` instead of `--no-direct-io` to select direct fragment I/O.
See [script shutdown limits](../operations/operations.md#scripts) before reusing a
server path or stopping a busy server.
Restarting a server on its existing storage needs no join address;
membership is persisted.
