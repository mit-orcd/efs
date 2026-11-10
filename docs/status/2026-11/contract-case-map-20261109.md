# Contract-to-case maps in the component guides (Task EU)

**Verdict: PASS, docs and docs tooling only, no K row, blocks alpha: no.**
Four owners carry a contract → case map in their guide, and
`scripts/check_guide_counts.py` (run by `make docs-check`) checks every row
statically. It passes on the tree and fails on a deliberately misspelled case
name (below, reverted).

Task EU, Nov 9 2026, Mac only (no host, no build). Source: branch
`claude/nervous-heisenberg-b36971` on devel `9bb50fe79` (wave 67). The
change is commits `2f662a8be` and `032181404` (review fixes); neither is merged or pushed. Purpose: the
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
  for batch atomicity, the BUSY write check and the invariants with a case
  named after them (I7, I8, I9, I16, worded as in
  [architecture §4](../../how-it-works/architecture.md#4-invariants)).
  The other invariants the Contract section lists are I5, I6, I17, I18,
  I19, I23 and I24. They share one "none" row, because no case of
  `tests/test_meta_apply.c` names them. The command is
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

For meta_apply, a case is listed when it calls the operation, directly or
through a test helper. The call matrix (which `RUN` case reaches which public
function) came from a scratch script that is not committed. For high-fanout
operations, the cases named after the operation were picked:
`efs_meta_apply_init` is called by 92 cases, `create_file` by 77 and
`get_inode` by 71. Not every case body was read, so "proves" means "calls and
is named after it", not "asserts on every answer". A reverse check confirmed
that every operation in a row is called by at least one of the row's listed
cases. It found one row where none did (`efs_meta_get_chunk_view_tx`), fixed
in `032181404`. For the three interface tests, every listed case body was
read. A helper such as
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
| `open.h` (21 declared + `efs_client_ino_is_open`, stated there) | 22 | 11 | 3 (`efs_open_append_handoff`, `efs_open_append_burst_note`, `efs_open_append_burst_published`) | 8 (`efs_pending_create_alloc`, `efs_pending_create_free`, `efs_pending_create_queue`; `wb_lock_budget`; `efs_fuse_open_ino`, `efs_fuse_read_ino`, `fuse_fi_direct_io`, `fuse_odirect_unaligned`) |
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

## Checks (Mac, on `032181404` plus this page)

```bash
python3 scripts/check_guide_counts.py    # guide counts: 13 guides match the catalogue
python3 tests/test_tests_for.py          # PASS (the check()'s existing self-test)
make docs-check                          # rc 0
python3 scripts/check_doc_links.py       # 0 dangling link(s), 0 orphan script(s)
python3 tests/test_code_graph.py         # after the graph rebuild
graphify explain client/efs_fuse.open    # still lists src/client/efs_fuse/README.md [guided_by]
```

## Deliberate failure (reverted)

Five rows were edited in place with `sed` on `032181404`, the check was run,
and both guides were restored from copies:

1. A misspelled `RUN` case: `test_xattr` became `test_xatr` in the case and
   the command.
2. A misspelled interface case: `open_busy_admission` became
   `open_busy_admision`.
3. A binary that does not exist: `test_find_res_iface`.
4. A command that does not select its case: `test_utimens_fence` with the
   argument `test_stat_collect`.
5. A "none" row whose command is not a fallback: the I7 row.

```
src/client/efs_fuse/README.md:107: open_busy_admision is not a function in tests/test_open_interface.c
src/client/efs_fuse/README.md:148: byte_budget: tests/test_find_res_iface.c is not a tracked file
src/meta/meta_apply/README.md:125: a "none" row names a case or runs no tests_for.py fallback
src/meta/meta_apply/README.md:145: argument test_xatr matches no RUN case of tests/test_meta_apply.c
src/meta/meta_apply/README.md:145: test_xatr is not a RUN case of tests/test_meta_apply.c
src/meta/meta_apply/README.md:174: test_utimens_fence is not selected by the command in tests/test_meta_apply.c
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
- For a `main` entry, the check confirms only that `main` exists, not the
  quoted comment. The writeback table is therefore checked only for its
  binary.
- The map check has no case in `tests/test_tests_for.py`, the existing
  self-test of `check()`. It was shown failing only by the probes above.
- Only these four owners have a map. Other guides keep their file tables.
