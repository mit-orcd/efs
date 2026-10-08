# efs documentation

Start with the [project README](../README.md) for implemented capabilities,
build requirements and product limits. The latest tagged checkpoint is
[v0.2.0-pre-alpha](status/v020-amd-release.md): four AMD I/O/transport
configurations passed POSIX, peer and cold remount tests. Its RDMA coverage
uses software RXE; broader failure/scale acceptance remains in the queue.

The architecture is the accepted design, not a declaration that every path
is implemented. Use the current status and evidence when assessing a guarantee.
The illustrated operator portal is maintained separately and is not shipped
in this repository. Pick the guide below for your task.

## Tools

| Binary | Role |
| --- | --- |
| `efsd` | Storage and metadata daemon |
| `efs-fuse` | Linux FUSE client |
| `efs-mgmt` | Cluster status, initialization and metadata administration |
| `efs-query` | Legacy query tool; placeholder results remain [W80](backlog/work-items.md#w80) |
| `efs-bench` | Local engine/prototype benchmarks and cluster RPC benchmarks |

## Use it

- **New user** → [using/quickstart.md](using/quickstart.md) — three local servers, mkfs, mount, first file.
- **Power user / operator of a mount** → [using/power-user.md](using/power-user.md) — mount options, environment variables, quotas, `efs-mgmt`, `df`/`du` semantics.

## How it works

- **Software developer (changing code)** → [how-it-works/developing.md](how-it-works/developing.md) — modularity rules, task routing, what "done" means; [how-it-works/verification.md](how-it-works/verification.md) — the simulator and the gates; [how-it-works/testing.md](how-it-works/testing.md) — test suites and honest measurement.
- **Architect (understanding or questioning the design)** → [how-it-works/architecture.md](how-it-works/architecture.md) — the normative spec; wire-level detail: [data](how-it-works/protocols/data.md), [directory](how-it-works/protocols/directory.md), [transactions](how-it-works/protocols/transactions.md), [sessions](how-it-works/protocols/sessions.md); [how-it-works/design-rationale.md](how-it-works/design-rationale.md) — why it has this shape; [how-it-works/naming.md](how-it-works/naming.md); [how-it-works/performance.md](how-it-works/performance.md) — the hot-path contract; [how-it-works/failure-tolerance.md](how-it-works/failure-tolerance.md) — the node-count guarantee derived.

## Project status

- **AI agent (or briefing one)** → [status/README.md](status/README.md) — the task right now and the queue index (one line per open item, links out); [status/in-flight.md](status/in-flight.md) — the current handoff block, finish it first.
- **Master of the agents** → [status/decisions.md](status/decisions.md) — the decision register (decided = approved design; implementation and acceptance are separate; unresolved asks remain in the register); [backlog/](backlog/README.md) — what is not scheduled.

## Backlog

- [backlog/README.md](backlog/README.md) — open features, enhancements and bug fixes: work items with full text, product gaps, parked ideas.

## Operations

- [operations/operations.md](operations/operations.md) — start/stop, storage layout, quotas, `efs-mgmt`, rejoin, profiling; [operations/runbooks.md](operations/runbooks.md) — current measurement entry points and archived investigation records.

## History / archive

- [archive/README.md](archive/README.md) — historical checkpoints and the closed-items index (check current status before reopening); [archive/project-history.md](archive/project-history.md) — every roll, gate and root cause since Aug 2026; [archive/design-history.md](archive/design-history.md) — the review rounds; [archived client-cache design](archive/landed/client-cache-design.md) — historical design of landed work.

---

**Generated files:** `how-it-works/architecture-full.md` and
`how-it-works/architecture.html` are built by
[gen-architecture-full.py](gen-architecture-full.py) from
`how-it-works/architecture.md` plus its satellites. **Do not edit them** —
edit the sources, regenerate with `python3 docs/gen-architecture-full.py`,
and verify with `python3 docs/check-architecture.py` (`make docs-check`).
