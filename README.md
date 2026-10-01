# efs

A distributed filesystem in C, pre-alpha. Every 128 KiB file chunk is
stored as **2 data fragments + 1 XOR parity** on three storage nodes; any
two fragments rebuild the chunk, so the cluster keeps serving with **one
node down**. Metadata is one Raft group per shard over an on-disk ordered
KV store, hosted by the same servers. Clients mount it with FUSE; the
transport is TCP or RDMA (InfiniBand), chosen at mount time.

Goal: ≥ 2³² files, throughput that tracks the hardware, no software
serialization point — the normative spec is
[docs/architecture.md](docs/architecture.md).

## What you need

- `gcc`, `make`, `pthread`; Blake3 is vendored (`deps/blake3/`).
- FUSE client: OS **fuse3** (`fuse3-devel` ≥ 3.3.0, `fusermount3`, kernel
  `fuse` module).
- RDMA (optional): `libibverbs`/`librdmacm`; `EFS_TRANSPORT=tcp` works
  without.
- **Client hosts: `fs.pipe-max-size` ≥ 8 MiB** (`efs-fuse` sets
  `max_write` to 4 MiB and asks for `FUSE_CAP_SPLICE_READ`; below the cap
  libfuse silently falls back to an extra copy per written byte):

  ```bash
  # /etc/sysctl.d/98-efs-pipe.conf
  fs.pipe-max-size = 8388608
  ```

  then `sysctl -p /etc/sysctl.d/98-efs-pipe.conf`. Servers do not need it.

## Build

```bash
make            # efsd efs-fuse efs-mgmt efs-query efs-bench + unit tests
make test       # runs the unit suites (test_sim is the protocol correctness gate)
make docs-check # the generated docs/architecture-full.md matches its sources
```

Builds are `-O3 -g -march=native`. **Build on the machine class that will
run the binary**: an AVX-512 build host and a Zen2 server give you a daemon
that dies with SIGILL at startup. On the test cluster that means never
`make` in the NFS home; the deploy scripts rsync to `/tmp/efs` and build
there.

| Binary | Role |
|---|---|
| `efsd` | storage + metadata server (one per node) |
| `efs-fuse` | FUSE client |
| `efs-mgmt` | cluster CLI: `status`, `mkfs`, `raft-status`, `add-node`, `add-storage`, `shrink-quota`, `raft-getchunks`, … |
| `efs-query` | file / per-user totals |
| `efs-bench` | metadata op-rate bench (`--meta`) |

## Quick start — three local servers

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

## The fcstor test cluster

Four servers (fcstor003–006, port 19810, six NVMe roots each, RDMA) and
thirteen clients. Three scripts cover every operation; nothing else is
needed:

```bash
bash tests/preflight.sh                                   # read-only health check — run first
bash tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]
bash tests/wipe_cluster.sh && bash tests/cluster.sh start --fresh --clients   # new table (ask first)
```

`cluster.sh` rsyncs, builds on each node, restarts the four servers
together (the build-ID gate rejects mixed builds), and with `--clients`
remounts fcstor003–015. Anything that takes more than 10 s runs in a
detached screen on a cluster node, not in a login-node shell. The rules of
that cluster — pre-flight, timeouts, what a dead mount looks like, why
`pkill -f` is forbidden — are `.cursor/rules/efs-fcstor-deploy.mdc`,
`efs-remote-timeouts.mdc` and `efs-fio-honest.mdc`.

## Tests and numbers

| Command | Gate |
|---|---|
| `make test` | unit suites incl. `test_sim` (deterministic simulator), `test_raft`, `test_kv_lsm`, `test_meta_apply` |
| `tests/run_tests.sh posix <host>` | POSIX suite 1 vs an XFS baseline — expected **200/201** (`mmap_write_read` SKIP by spec) |
| `tests/run_tests.sh posix2 <h1> <h2>` | cross-client visibility — expected **63/63** |
| `tests/run_tests.sh posixpersist` | durability across unmount/remount |
| `tests/run_tests.sh leaks` | valgrind, client and server |
| `tests/measure/dd_wall.sh`, `tests/perf/io500/run.sh` | throughput (dd+fsync walls, IO-500 debug); see `docs/testing.md` |

A write number counts only with the flush inside the clock (`dd
conv=fsync`, fio `--end_fsync=1`) and only from a mount `findmnt` shows as
`fuse.efs-fuse`. Current references (Oct 1 2026, RDMA): one client writes
1.3–1.5 GB/s and reads a cold file at 3.6 GB/s (6.5 GB/s with four
readers); nine clients write 2.5–2.8 GB/s aggregate; IO-500 9×4 debug
runs every phase. The table with hardware ceilings is START-HERE §1a.
Results that a document cites live under `results/`
([results/README.md](results/README.md) has the retention rule).

## Failure tolerance (one node)

| If this happens | Reads | Writes |
|---|---|---|
| 1 of 3 servers down | yes — any 2 fragments XOR-rebuild the chunk | no — publication needs all `k+f` fragments durable |
| 2 of 3 servers down | no | no |
| 1 disk full | yes | yes, until a second node is also full (`ENOSPC`) |

**Pre-alpha caveat: nothing repairs a lost fragment yet.** A chunk that
drops to 2 of 3 stays that way, so the cluster reads fine while being one
failure from losing that data, and reports no degradation. Repair and
degraded publication are specified, not written. Also absent: client-side
fencing, fsck, authentication. See
[docs/product-gaps.md](docs/product-gaps.md).

## Documentation map

| Read this | For |
|---|---|
| [docs/arch/START-HERE.md](docs/arch/START-HERE.md) | the work queue, decisions taken and pending, current numbers, in-flight handoff — start here before changing code |
| [docs/architecture.md](docs/architecture.md) | the normative spec (invariants, protocols); `docs/arch/protocols/*` for the wire-level detail |
| [docs/arch/failure-tolerance.md](docs/arch/failure-tolerance.md) | how the node count sets the guarantee and why a healthy write needs every fragment |
| [docs/operations.md](docs/operations.md) | start/stop, storage layout, quotas, `efs-mgmt`, rejoin, profiling |
| [docs/testing.md](docs/testing.md) | unit tests, POSIX suites, honest throughput, profiling |
| [docs/arch/runbooks.md](docs/arch/runbooks.md) | measurement scripts under `tests/measure/` and the numbers they last produced |
| [docs/arch/development.md](docs/arch/development.md) | source layout, how the spec and the generated docs are kept in sync |
| [docs/product-gaps.md](docs/product-gaps.md) | what is missing before this is a filesystem you could run |
| [docs/scaling-roadmap.md](docs/scaling-roadmap.md) | the longer-range plan |
| [docs/project-history.md](docs/project-history.md) | the archive: every roll, gate and root cause since Aug 2026 — search it before re-deriving one |
| `.cursor/rules/efs-project-state.mdc` | current facts and the do-not-re-chase learnings |

MIT. See [LICENSE](LICENSE).
