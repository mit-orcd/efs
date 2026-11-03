# src/mgmt/ — mgmt — efs-mgmt

## Files

- `efs_mgmt.c` — the management CLI (settings, status, membership); no persistent state.

## Contract

- Interfaces: the public `efs/*.h` headers it includes (`protocol`, `membership`, `session_revoke`, `cli_help`); command semantics: not stated in one place.
- Invariants: none owned; it issues requests the server enforces ([architecture §7.8](../../docs/how-it-works/architecture.md#78-control-plane-membership-and-reconfiguration)).

## Calls

- May call: server RPCs over `common/network`; observed private includes `client/stop_control.h`, `common/parse_host_port.h`.
- Called by: operators and tests.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py mgmt/efs_mgmt`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
