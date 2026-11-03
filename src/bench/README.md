# src/bench/ — bench — local benchmarks

## Files

- `bench_local.c/.h` — local storage benchmarks, no daemon or network.
- `io_bench.c/.h` — independent raw-file I/O driver.
- `blake3_bench.c/.h` — multi-threaded BLAKE3 microbench.
- `storage_principles.c/.h` — finite EFS-shaped file I/O experiments; no production engine calls.
- `latency.h` — exact integer latency counting; `perf_control.h` — perf control helpers (state not stated).

## Contract

- No production contract; benchmarks must not change production checksum policy ([`io_bench.c`](io_bench.c)).
- Evidence from benchmarks follows [AGENTS.md](../../AGENTS.md) rules.

## Calls

- May call: `kv/`, `raft/`, `server/store`, `common/` (observed private include `server/server_internal.h`).
- Called by: `client/efs_bench.c`, tests.

## Tests

Select the catalogue rows ([tests/budgets.tsv](../../tests/budgets.tsv), column `subsystem`) and print their commands (add `--run` to execute after `make all`):

- `python3 scripts/tests_for.py bench`
- `python3 scripts/tests_for.py bench/bench_local`
- `python3 scripts/tests_for.py bench/latency`

## Open findings

- none: no open K row or W item names this directory (checked `alpha-1.0-2plus1-checklist.md` and `work-queue.md`).
