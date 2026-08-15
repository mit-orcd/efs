# efs

A prototype distributed filesystem in C. Three storage nodes hold every file
chunk as **2 data fragments + 1 XOR parity**. Any two fragments rebuild the
chunk, so the cluster stays up if **one node dies**. Metadata uses the same
scheme. You mount it with FUSE.

Dependencies are vendored (`deps/blake3/`, `deps/libfuse/`). Needs `gcc`,
`make`, and the FUSE kernel module.

| Binary | Role |
|---|---|
| `efsd` | storage server |
| `efs-fuse` | FUSE client |
| `efs-mgmt` | cluster CLI |
| `efs-query` | file / per-user totals |

## Quick start

```bash
make
```

Three servers — the second and third join the first:

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

```bash
echo hello > /mnt/efs/world.txt
cat /mnt/efs/world.txt
```

Stop:

```bash
./scripts/client.sh stop /mnt/efs
./scripts/server.sh stop 127.0.0.1:17432
./scripts/server.sh stop 127.0.0.1:17433
./scripts/server.sh stop 127.0.0.1:17434
```

`make test` runs the unit and cluster checks.

## Failure tolerance (one node)

| If this happens | Reads | Writes |
|---|---|---|
| 1 of 3 servers down | yes — any 2 fragments rebuild the chunk | yes — 2 of 3 acks |
| 2 of 3 servers down | no | no |
| 1 disk full | yes | yes, until a second node is also full (`ENOSPC`) |

Worked examples (XOR math, kill-a-node, two-node outage):
**[docs/failure-tolerance.md](docs/failure-tolerance.md)**

## More

- **[docs/operations.md](docs/operations.md)** — start/stop, storage layout, quotas, `efs-mgmt`, rejoin
- **[docs/design.md](docs/design.md)** — chunks, placement, metadata, protocol
- **[docs/testing.md](docs/testing.md)** — tests, Slurm harness, `perf`

MIT. See [LICENSE](LICENSE).
