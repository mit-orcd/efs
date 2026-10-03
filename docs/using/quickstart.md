# Quick start — three local servers

[Docs index](../README.md) · [Power user](power-user.md) ·
[Operations](../operations/operations.md) ·
[Architecture](../how-it-works/architecture.md)

Start **all** servers first (the metadata engine needs a Raft quorum before
`mkfs`); the second and third join the first:

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G
./scripts/server.sh 127.0.0.1:17433 /tmp/efs/s2:5G 127.0.0.1:17432
./scripts/server.sh 127.0.0.1:17434 /tmp/efs/s3:5G 127.0.0.1:17432
```

Format once, on one node only, then mount:

```bash
./efs-mgmt mkfs 127.0.0.1:17432          # once; never retry it on another node
./scripts/client.sh 127.0.0.1:17432 /mnt/efs
echo hello > /mnt/efs/world.txt && cat /mnt/efs/world.txt
```

Stop:

```bash
./scripts/client.sh stop /mnt/efs
./scripts/server.sh stop 127.0.0.1:17432   # and 17433, 17434
```

`server.sh --perf` / `client.sh --perf --strace` attach recorders and
write `flat.txt` / `by_thread.txt` / `callers.txt` at `stop`.
`EFS_TRANSPORT=auto|tcp|rdma` (default auto: RDMA if InfiniBand is up).
Restarting a server on its existing storage needs no join address;
membership is persisted.
