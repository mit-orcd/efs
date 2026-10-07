# Quick start — three local servers

[Docs index](../README.md) · [Power user](power-user.md) ·
[Operations](../operations/operations.md) ·
[Architecture](../how-it-works/architecture.md)

Run from a built checkout on Linux with libfuse3 and a usable `/dev/fuse`;
RDMA additionally needs the verbs libraries and suitable hardware; see the [build prerequisites](../../README.md#what-you-need).
Use three distinct empty storage roots for this disposable example.

Start **all** servers first (the metadata engine needs a Raft quorum before
`mkfs`); the second and third join the first:

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G
./scripts/server.sh 127.0.0.1:17433 /tmp/efs/s2:5G 127.0.0.1:17432
./scripts/server.sh 127.0.0.1:17434 /tmp/efs/s3:5G 127.0.0.1:17432
```

Initialize the single export through one seed, then mount:

```bash
./efs-mgmt mkfs 127.0.0.1:17432          # initialize the single export
./scripts/client.sh 127.0.0.1:17432 /mnt/efs
echo hello > /mnt/efs/world.txt && cat /mnt/efs/world.txt
```

Repeated `mkfs` preserves an existing root/salt; after an uncertain outcome,
inspect `raft-status` before retrying against the same intended cluster. It
is not a wipe or a named-export creation command.

Stop the client successfully before stopping any server. A failed ordinary
client stop retains unresolved writes; keep the servers up to resolve them:

```bash
./scripts/client.sh stop /mnt/efs
./scripts/server.sh stop 127.0.0.1:17432
./scripts/server.sh stop 127.0.0.1:17433
./scripts/server.sh stop 127.0.0.1:17434
```

`server.sh --perf` / `client.sh --perf --strace` attach recorders and
write `flat.txt` / `by_thread.txt` / `callers.txt` at `stop`.
`EFS_TRANSPORT=auto|tcp|rdma` (default auto: RDMA if InfiniBand is up).
The local RDMA implementation requires an active InfiniBand port with a LID;
it does not implement Ethernet/RoCE or GPU device-memory registration. See
[script shutdown limits](../operations/operations.md#scripts) before reusing a
server path or stopping a busy server.
Restarting a server on its existing storage needs no join address;
membership is persisted.
