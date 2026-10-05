# efs documentation

Who are you? That picks your file.

## Use it

- **New user** → [using/quickstart.md](using/quickstart.md) — three local servers, mkfs, mount, first file.
- **Power user / operator of a mount** → [using/power-user.md](using/power-user.md) — mount options, environment variables, quotas, `efs-mgmt`, `df`/`du` semantics.

## How it works

- **Software developer (changing code)** → [how-it-works/developing.md](how-it-works/developing.md) — modularity rules, task routing, what "done" means; [how-it-works/verification.md](how-it-works/verification.md) — the simulator and the gates; [how-it-works/testing.md](how-it-works/testing.md) — test suites and honest measurement.
- **Architect (understanding or questioning the design)** → [how-it-works/architecture.md](how-it-works/architecture.md) — the normative spec; [how-it-works/protocols/](how-it-works/protocols/) — wire-level detail; [how-it-works/design-rationale.md](how-it-works/design-rationale.md) — why it has this shape; [how-it-works/naming.md](how-it-works/naming.md); [how-it-works/performance.md](how-it-works/performance.md) — the hot-path contract; [how-it-works/failure-tolerance.md](how-it-works/failure-tolerance.md) — the node-count guarantee derived.

## Project status

- **AI agent (or briefing one)** → [status/README.md](status/README.md) — the task right now and the queue index (one line per open item, links out); [status/in-flight.md](status/in-flight.md) — the current handoff block, finish it first.
- **Master of the agents** → [status/decisions.md](status/decisions.md) — the decision register (decided = implemented; asks are not code until decided); [backlog/](backlog/README.md) — what is not scheduled.

## Backlog

- [backlog/README.md](backlog/README.md) — open features, enhancements and bug fixes: work items with full text, product gaps, parked ideas.

## Operations

- [operations/operations.md](operations/operations.md) — start/stop, storage layout, quotas, `efs-mgmt`, rejoin, profiling; [operations/runbooks.md](operations/runbooks.md) — measurement scripts and the numbers they last produced.

## History / archive

- [archive/README.md](archive/README.md) — the closed-items index (grep a W/D number, see DONE, do not reopen); [archive/project-history.md](archive/project-history.md) — every roll, gate and root cause since Aug 2026; [archive/design-history.md](archive/design-history.md) — the review rounds; [archive/landed/](archive/landed/) — design docs of landed work.

---

**Generated files:** `how-it-works/architecture-full.md` and
`how-it-works/architecture.html` are built by
[gen-architecture-full.py](gen-architecture-full.py) from
`how-it-works/architecture.md` plus its satellites. **Do not edit them** —
edit the sources, regenerate with `python3 docs/gen-architecture-full.py`,
and verify with `python3 docs/check-architecture.py` (`make docs-check`).
