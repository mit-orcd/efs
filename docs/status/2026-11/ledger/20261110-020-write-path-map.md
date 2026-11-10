# Concern map of `src/client/write.c`

[Concern map of `src/client/write.c`](../write-path-map-20261110.md): **PASS, no K row, blocks alpha: no** (Nov 10, branch `claude/modest-cray-29bc32`, unmerged). New `docs/how-it-works/write-path-map.md` assigns every function of `write.c` to one of 20 concerns with state, mutex, logs, tests and lock order; `scripts/check_concern_map.py` in `make docs-check` fails on a missing, unknown or duplicated function. For the close()-EIO question the map points at 1,101 of 9,812 lines.
