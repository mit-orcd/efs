# Contract-to-case maps in the component guides (Task EU)

**Verdict: PASS, docs and docs tooling only, no K row, blocks alpha: no.**
Four owners carry a contract → case map in their guide, and
`scripts/check_guide_counts.py` (run by `make docs-check`) checks every row
statically. It passes on the tree and fails on a deliberately misspelled case
name (below, reverted).

Task EU, Nov 9 2026, Mac only (no host, no build). Source: branch
`claude/nervous-heisenberg-b36971` on devel `9bb50fe79` (wave 67). The
change is commit `2f662a8be`; it is not merged and not pushed. Purpose: the
Nov 9 outside review noted that agents could not tell which named case proves
a given operation or invariant. It asked for a small map from contract or
operation to named cases and commands, with the inherited whole-module tests
as the fallback.

## The change

- [`src/client/efs_fuse/README.md`](../../../src/client/efs_fuse/README.md),
  Tests section: three tables, for `open.h`, `writeback.h` and `find_res.h`.
  Each row is one operation or invariant the header states, the case
  function(s) in the owner's interface test that prove it, and the command.
  The interface binaries take no case filter, so the command is
  `make tests/<bin> && ./tests/<bin>` and the case is the function to read.
- [`src/meta/meta_apply/README.md`](../../../src/meta/meta_apply/README.md),
  Tests section only: one table over the operations that
  `include/efs/meta_apply.h` declares and `src/meta/meta_apply/*.c` defines.
  They are grouped by area, with up to three `RUN` cases per row, and a row
  for each invariant the Contract section states (batch atomicity, I8, I9,
  I16, the BUSY write check). The command is
  `./tests/test_meta_apply <case...>` (Task EG's substring filter).
- An operation with no case gets "none; whole-module fallback" and
  `python3 scripts/tests_for.py <tag>`, so the gap is visible.
- [`scripts/check_guide_counts.py`](../../../scripts/check_guide_counts.py):
  a table headed `| Operation or invariant | Cases | Command |` is a map.
  For each row it checks the following:
  - The command names `./tests/<bin>` and `tests/<bin>.c` is tracked
    (`git ls-files`, as `check_doc_links.py` does).
  - Each backticked name in the Cases cell is checked against the file.
    Where the file has `RUN(...)` lines, the name must be a `RUN` case and
    one of the command's arguments must select it. Otherwise the name must
    be a function defined in the file.
  - Every filter argument matches some `RUN` case. This mirrors the binary's
    exit 2.
  - A "none" row names no case and runs a `tests_for.py` fallback. The
    existing check already verifies that the fallback's tag resolves.

  A map header also clears the file-table state, so map rows never pass
  through the file-table count check. A failure prints
  `guide:line: <case> ... <file>`.

No test source, `tests/budgets.tsv` or `scripts/tests_for.py` changed.

## How a case was chosen

A case is listed when it asserts on the operation's answer, not merely when
it calls it. For high-fanout operations, the listed cases are the ones that
assert on the result: `efs_meta_apply_init` is called by 92 cases,
`create_file` by 77 and `get_inode` by 71. The call matrix (which `RUN` case
reaches which public function, directly or through a test helper) came from a
scratch script that is not committed. A helper such as
`test_create_lease_apply` (`static int`, no `RUN` line) is not a case. Its
operations are attributed to the `RUN` cases that call it.

The two inline forms are shown in the guide:

- `test_wb_interface.c` has no case functions: every block is inline in
  `main`. `test_open_interface.c` proves the append-burst trio and the no-row
  answers inline too. Such rows name `` `main` `` plus the block's opening
  comment and are counted separately below. Giving them case functions needs
  a test edit, which this task excludes.
- `efs_meta_apply_check` is called by 38 cases as their end-of-case oracle,
  but no case hands it an inconsistent store. It is counted as "none".

## Per-owner counts

Counted per operation (a row may list several operations that share their
cases).

| Owner | Operations | Named case | Only an inline block of `main` | None (fallback) |
|---|---|---|---|---|
| `open.h` (21 declared + `efs_client_ino_is_open`, stated there) | 22 | 11 | 3 (`efs_open_append_handoff`, `_burst_note`, `_burst_published`) | 8 (`efs_pending_create_alloc`, `_free`, `_queue`; `wb_lock_budget`; `efs_fuse_open_ino`, `efs_fuse_read_ino`, `fuse_fi_direct_io`, `fuse_odirect_unaligned`) |
| `writeback.h` | 8 | 0 | 5 (the five pool operations) | 3 (`bounce_alloc`, `bounce_release`, `efs_fuse_log_err`) |
| `find_res.h` | 5 | 4 | 1 (`find_res_clear`) | 0 |
| `meta/meta_apply` (`efs/meta_apply.h`, defined in this directory) | 152 | 119 | 0 | 33 |

The meta_apply "none" operations by area:

| Area | Count |
|---|---|
| `append.c` | 10 |
| `pub_stage.c` | 5 |
| `init.c` | 4 |
| `namespace.c` (expiry) | 3 |
| `publish.c` | 3 |
| `create.c` | 2 |
| `gc.c` | 2 |
| `lanes.c` | 2 |
| `rows.c` | 1 |
| `check.c` | 1 |

Some stated answers also have no case in the interface binary:
`efs_wb_enqueue_owned` BUSY after 8 s with a full ring; `find_res_intern`
`-ENOMEM`; `-EOVERFLOW`, `-ENAMETOOLONG` and `-EILSEQ`. They have their own
"none" rows. `efs/meta_apply.h` also declares 22 functions that it does not
define in this directory. Twenty-one are in `src/meta/append_plan.c` and one
(`efs_meta_candidate_gen`) is in `src/common/publication.c`. They are left to
those owners and not counted.

Every one of the 152 operations was confirmed to appear in exactly one class
of row, by a scratch cross-check of the header against the table.

## Checks (Mac, on `2f662a8be` plus this page)

```bash
python3 scripts/check_guide_counts.py    # guide counts: 13 guides match the catalogue
python3 tests/test_tests_for.py          # PASS (the check()'s existing self-test)
make docs-check                          # rc 0
python3 scripts/check_doc_links.py       # 0 dangling link(s), 0 orphan script(s)
python3 tests/test_code_graph.py         # after the graph rebuild
graphify explain client/efs_fuse.open    # still lists src/client/efs_fuse/README.md [guided_by]
```

## Deliberate failure (reverted)

Four rows were edited in place with `sed`, the check was run, and both guides
were restored from copies:

1. A misspelled `RUN` case: `test_xattr` became `test_xatr` in the case and
   the command.
2. A misspelled interface case: `open_busy_admission` became
   `open_busy_admision`.
3. A binary that does not exist: `test_find_res_iface`.
4. A command that does not select its case: `test_utimens_fence` with the
   argument `test_stat_collect`.

```
src/client/efs_fuse/README.md:107: open_busy_admision is not a function in tests/test_open_interface.c
src/client/efs_fuse/README.md:148: byte_budget: tests/test_find_res_iface.c is not a tracked file
src/meta/meta_apply/README.md:142: test_xatr is not a RUN case of tests/test_meta_apply.c
src/meta/meta_apply/README.md:171: test_utimens_fence is not selected by the command in tests/test_meta_apply.c
rc=1
```

After the restore, the check printed `guide counts: 13 guides match the
catalogue` with rc 0.

## Limitations

- The check is static. It proves that a named case exists and that the
  command reaches it, not that the case proves the row. That stays a
  reader's judgement, recorded in "How a case was chosen".
- The commands were not run here (Mac only, no build). The interface
  binaries need a Linux build with fuse3 headers.
- Interface-test blocks inline in `main` have no case name of their own.
  Naming them needs test-source changes, which this task excludes.
- Only these four owners have a map. Other guides keep their file tables.
