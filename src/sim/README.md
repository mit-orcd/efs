# src/sim/ — sim — deterministic simulator

## Files

- `sim.c` — the discrete-event core: scheduling, message order, drops, delays, crashes, clock steps.
- `sim_internal.h` — shared simulator structs.
- `sim_raft.c`, `sim_ctrl.c` — single-shard Raft groups and the control-plane group.
- `sim_txn.c`, `sim_ns.c`, `sim_dir.c` — second metadata group, cross-shard namespace transactions, directory layout epochs.
- `sim_sess.c`, `sim_lock.c` — session/lease and POSIX lock commands.
- `sim_disk.c` — simulated persistent storage with fault injection; `opid.c` — operation-identity table.

## Contract

- Interface: [`efs/sim.h`](../../include/efs/sim.h). Fault model and what the simulator proves: [verification.md](../../docs/how-it-works/verification.md).
- The same compiled state machines (`raft/`, `kv/`, `meta/`) run under it — purity rule in [developing.md](../../docs/how-it-works/developing.md).
- Invariants exercised: I1–I10, I16–I19, I23; lock/thread rules: single-threaded, not otherwise stated.

## Calls

- May call: `raft/`, `kv/`, `meta/`, `data/`, `common/` headers.
- Called by: tests only.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py sim`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
