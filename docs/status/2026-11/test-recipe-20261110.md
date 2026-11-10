# Test recipe and component routing (Task T8)

**Status.** Source commit: `89849c730` (devel) plus this branch's commit. Verdict: PASS for the docs checks; the Linux-only commands are NOTRUN here. Remaining failures: none. Active run: none. Next action: the merger reads `tests/README.md` once and merges. Merge state: branch `claude/confident-grothendieck-b05988`, unmerged.

## Purpose

Brief T8 of [structure review tasks](structure-review-tasks-20261110.md): `tests/` had no README, and the developer guide's routing table named no component tag. No source or build change; no K row; blocks alpha: no.

## What changed

- New `tests/README.md` (59 lines): which kind of test (interface test in C, Python test with private daemons, sequence-binary case), how to register it (`tests/budgets.tsv` columns, `test` versus `test-long`, `tests/sanitizer_check.sh` list), how to run it.
- `docs/how-it-works/developing.md` §2: a "Component" column (a tag or "none" per row) and the sentence "Start with `graphify explain <Component>`; the Read column is what to read next."
- Two stale statements fixed in the same file:
  1. Row "Module structure / file layout": the Governs cell "~1000-line file cap; header-only deps" now reads "owner interfaces: private state behind a small header (structure plan)".
  2. §5 (the briefing template): "the routing row's gate passes via tests/run_tests.sh" was false in part. `tests/run_tests.sh` exists but runs only the posix, posix2, posixstress, posixpersist, nvme, meta and leaks suites; many routing gates are `tests/test_sim`, `tests/test_kv_lsm`, `tests/stress/raft_host_smoke.sh` and the fio matrix. Only the "via tests/run_tests.sh" part was replaced: "(a posix or fio gate via tests/run_tests.sh <suite>; a tests/test_* or script gate by running it)". The "make test-all passes" part is true and unchanged. (§3 item 3 carries the same words; the brief limits the fix to §5, so it is left.)

## Commands run here (macOS, Darwin 25.6) and results

| Command | Exit | Result |
| --- | --- | --- |
| `python3 scripts/tests_for.py client/efs_fuse.open` | 0 | prints the tag's commands |
| `python3 tests/test_make_test_recipe.py` | 0 | 1147 commands, 103 budget rows, gate 1025 s of 1750 s: PASS |
| `python3 scripts/check_guide_counts.py` | 0 | 13 guides match the catalogue |
| `python3 tests/test_tests_for.py` | 0 | PASS |
| `grep -n test_open_interface Makefile` | 0 | lines 168, 169, 865, 979 (the rule the README names) |

Not run here (Linux or a built tree needed; `make tests/test_meta_apply` fails on macOS at `src/common/log_ts.c`, `fopencookie`): `python3 scripts/tests_for.py client/efs_fuse.open --run`, `./tests/test_meta_apply --list` (the case filter is read from `tests/test_meta_apply.c` lines 7318 to 7361), `make test`. The README marks each "not run here". T7 gives the three interface tests named cases at the same time; the README says to try `--list`.

## Routing-table tags

Each tag was accepted by `python3 scripts/tests_for.py <tag>` with exit 0 (never 2): `wire`, `common/protocol`, `meta/name_index`, `client/efs_fuse.lookup`, `meta/txn`, `client/efs_fuse.namespace`, `meta/meta_apply.mtime`, `meta/meta_apply.lanes`, `client/write`, `meta/meta_apply.append`, `client/read`, `client/efs_fuse.getattr`, `meta/session`, `meta/lock`, `client/efs_fuse.open`, `raft`, `kv`, `server/cluster`, `server/raft_host`, `sim`, `meta/meta_apply`, `common/request_receipt`, `client/efs_fuse`, `docs`. Rows "A hot path, for speed" and "Module structure / file layout" get "none".

## Limitations

The tag choice per row is a judgment from the row's topic; the tag audit owns correctness of the tags themselves. 
## Doc checks

| Command | Exit |
| --- | --- |
| `python3 scripts/check_doc_links.py` | 0 (0 dangling links) |
| `python3 scripts/status_layout.py` | 0 |
| `python3 scripts/check_k_register.py` | 0 |
| `make docs-check` | 1 first (`architecture-full.md` and `architecture.html` stale, because `developing.md` is a source of them); after `python3 gen-architecture-full.py` in `docs/`: 0 |

The two generated files are therefore also in the commit; the brief did not list them, but `make docs-check` cannot pass otherwise.
