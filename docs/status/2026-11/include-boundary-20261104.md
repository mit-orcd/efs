# Include boundary check (W171 phase 3)

**Verdict: PASS** (Nov 4, 2026). Purpose: cross-directory coupling under `src/` grows only on purpose ([structure plan](structure-plan-20261102.md), phase 3). No product code changed; no K row.

- Source: `devel` at `15f346b48`; the change is `scripts/check_include_boundary.py`, a line in the `docs-check` recipe, a `--selftest` line in `make test` and `tests/budgets.tsv`.
- Rule: every `#include "../.../<header>"` in `src/**/*.c|h` is resolved against the including file and counted per (including `src/<dir>`, included `src/<dir>`) pair, so moving a file or splitting it into a subdirectory of the same `src/<dir>` (CW's `efs_fuse/`) does not change the result. A new pair or a count above the allowlist fails with one line per offending include (exit 1); removals pass and print `can tighten`.
- Plan said 21 includes; the tree has 22 (`client` -> `common` is 8).

| Pair (from -> to) | Count | Files |
|---|---|---|
| bench -> common | 1 | `bench_local.c` |
| bench -> server | 1 | `bench_local.c` |
| client -> bench | 4 | `efs_bench.c` |
| client -> common | 8 | `efs_bench.c`, `efs_fuse.c`, `node_cache.c`, `stage_evict.c` |
| meta -> common | 2 | `metadata.c`, `query_disk.c` |
| mgmt -> client | 1 | `efs_mgmt.c` |
| mgmt -> common | 1 | `efs_mgmt.c` |
| server -> common | 4 | `efsd.c`, `raft_host.c` |

Reproduce (any host with Python 3, repository checkout, no cluster):

```
python3 scripts/check_include_boundary.py            # PASS include boundary: 22 includes, 8 pairs
python3 scripts/check_include_boundary.py --selftest # temp tree: allowed, over-count and new-pair cases
make docs-check
```

Result: `PASS include boundary: 22 includes, 8 pairs`; a temporary extra `#include "../meta/x.h"` in `src/mgmt/efs_mgmt.c` gave `FAIL include boundary: 23 includes, 9 pairs`, exit 1 (reverted). Limits: only quoted relative includes are seen (not `<efs/...>` or `-I` paths); the allowlist is by pair, so a second include inside an allowed pair needs the count raised on purpose.
