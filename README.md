# efs

A distributed filesystem in C with a Linux FUSE client, Raft metadata and
2+1 erasure-coded storage. Supports TCP, InfiniBand/RoCE RDMA, multiple
storage roots, quotas, garbage collection, and buffered or direct fragment I/O.
The current runtime supports three or four servers and two metadata Raft groups.

**Pre-alpha:** persistence, session fencing and automatic fragment repair
remain unfinished. Use disposable data; see [implementation limits](docs/status/spec-implementation.md).

## Build and start

Requires Linux, a C compiler, `make`, `pkg-config`, Python ≥3.9,
**libfuse3 ≥3.12** and **libibverbs** development libraries. Build on the
machine class that runs the binaries (`-march=native`).

```bash
make
make test
make docs-check
```

Start with the [three-node quick start](docs/using/quickstart.md).
[Configuration](docs/using/power-user.md) covers transport, storage and cache settings;
[operations](docs/operations/operations.md) covers management and safe shutdown.

## Portal preview

![External AMD cluster portal showing four servers and two FUSE mounts](docs/images/cluster-portal-amd.png)

**The portal is maintained separately and is not yet included in this repository.**
The picture shows illustrative values on one AMD host using RXE software RoCE;
it is not a benchmark or a live health report.

## Tested checkpoint

**[v0.2.0-pre-alpha](https://github.com/mit-orcd/efs/tree/v0.2.0-pre-alpha)**
passed POSIX, two-client and cold-remount suites on AMD for all four combinations:
direct/buffered fragment I/O × TCP/RXE RDMA. Each passed 216 POSIX tests
(one unsupported mmap skip), 64 peer tests and 26 cold-verification tests.
These runs do not establish hardware power-loss durability or cross-host RDMA acceptance.
[Results and evidence](docs/status/v020-amd-release.md).

## Documentation

- [Docs index](docs/README.md) — tools and guides.
- [Architecture](docs/how-it-works/architecture.md) — accepted design.
- [Current status](docs/status/README.md) — remaining work and acceptance gates.
- [Testing](docs/how-it-works/testing.md) — suites, profiling and measurement.

Development continues on `devel`; tags identify tested checkpoints.

MIT. See [LICENSE](LICENSE).
