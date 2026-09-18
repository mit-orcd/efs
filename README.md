# efs

A prototype distributed filesystem in C. Three storage nodes hold every file
chunk as **2 data fragments + 1 XOR parity**. Any two fragments rebuild the
chunk, so the cluster stays up if **one node dies**. Metadata uses the same
scheme. You mount it with FUSE.

Blake3 is vendored (`deps/blake3/`). The FUSE client links the OS **fuse3**
library (`fuse3-devel` ≥ 3.3.0, plus `fusermount3` and the kernel `fuse`
module). Needs `gcc` and `make`.

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
| 1 of 3 servers down | yes — any 2 fragments XOR-rebuild the chunk | no — publication needs all `k+f` fragments durable |
| 2 of 3 servers down | no | no |
| 1 disk full | yes | yes, until a second node is also full (`ENOSPC`) |

**Pre-alpha caveat, and it is a big one: nothing repairs a lost fragment yet.**
A chunk that drops to 2 of 3 stays that way, so the cluster reads fine while
being one failure from losing that data, and reports no degradation. The design
for repair and degraded publication is specified; the code is not written. See
[docs/product-gaps.md](docs/product-gaps.md) §1.

How the node count sets the guarantee, and why the write commit rule needs
every fragment when the cluster is healthy:
**[docs/arch/failure-tolerance.md](docs/arch/failure-tolerance.md)**

## More

- **[docs/architecture.md](docs/architecture.md)** — the normative spec (start at
  [docs/arch/START-HERE.md](docs/arch/START-HERE.md) if you are going to change code)
- **[docs/operations.md](docs/operations.md)** — start/stop, storage layout, quotas, `efs-mgmt`, rejoin
- **[docs/testing.md](docs/testing.md)** — unit tests, POSIX suites, perf and leak gates
- **[docs/product-gaps.md](docs/product-gaps.md)** — what is missing before this is
  a filesystem you could run: no repair, no fencing on the client, no fsck, no auth

MIT. See [LICENSE](LICENSE).
