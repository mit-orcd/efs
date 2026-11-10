# Diagnostics index (T5)

**Status:** source commit `862a78945` plus this branch's commits; verdict
**PASS** (macOS host only; Linux hash not run, see limitations); remaining
failures: none; active run: none; next action: the merging session reruns
`python3 scripts/gen_diagnostics.py --check` at each merge; merge state:
branch `claude/wonderful-brahmagupta-c95440`, unmerged.

## Purpose

Structure review task T5 ([briefs](structure-review-tasks-20261110.md)): a log
line such as `dcache-flush-fail ino=6980 stage=put rc=-3` should lead to its
file, function and component in one lookup, and `rc=-3` to its name.

## What changed

- `scripts/gen_diagnostics.py` (new): scans every `.c` and `.h` under `src/`
  and `include/`, writes [diagnostics.md](../../how-it-works/diagnostics.md);
  `--check` exits 1 when the page on disk differs. No `git`, no timestamps,
  paths relative to the repository, rows sorted by byte value.
- `docs/how-it-works/diagnostics.md` (generated): error-code table from the
  `#define EFS_ERR_*` lines of `include/efs/common.h` and a key table
  `Key | File | Function | Component`.
- `Makefile`: `python3 scripts/gen_diagnostics.py --check` in `docs-check`.
- `AGENTS.md`: one sentence in "Canonical defect-navigation graph", placed
  after the "Owner unknown" sentence so the two lookup rules stay together.

Helpers included: `fprintf(stderr, ...)` (the 370 calls without a key are
counted, not listed) and `efs_telemetry_log(level, ...)`. The other `*log*`
functions of `grep -rhoE '\b[a-z_]*log[a-z_]*\(' src` (`last_log`, `log_term`,
`raft_log_append`, `stripe_log`, `efs_fuse_log_err`, ...) take no literal key
or are not diagnostics, so they are excluded.
Component tag: `src/<dir>/<name>.c` is `dir/name`, `src/<dir>/<sub>/<name>.c`
is `dir/sub.name` (the `tests/budgets.tsv` spelling); an `include/efs/<n>.h`
maps to the `src` component of that name if one exists (via
`tests_for.component_exists`, imported by path), else `include/<n>`.
`tests_for.py` has no path-to-tag function, so only `component_exists` is reused.

## Result

102 keys in 258 (key, function) rows, 29 error codes, 370 keyless
`fprintf(stderr` calls, page 25,104 bytes (limit 40 KB). Functions are found
by a column-0 declarator heuristic; no row has an unresolved function (`?`).

## Commands and results

```text
$ python3 scripts/gen_diagnostics.py; shasum docs/how-it-works/diagnostics.md
23cbf812a7ce797553a4b2935ec3fac9dc761758  docs/how-it-works/diagnostics.md
$ python3 scripts/gen_diagnostics.py; shasum docs/how-it-works/diagnostics.md
23cbf812a7ce797553a4b2935ec3fac9dc761758  docs/how-it-works/diagnostics.md
$ python3 scripts/gen_diagnostics.py --check; echo $?
0
$ sed -i.b 's/Generated page/Generated pagf/' docs/how-it-works/diagnostics.md   # one character
$ python3 scripts/gen_diagnostics.py --check; echo $?
gen_diagnostics: docs/how-it-works/diagnostics.md is stale; run python3 scripts/gen_diagnostics.py
1
$ python3 scripts/gen_diagnostics.py; python3 scripts/gen_diagnostics.py --check; echo $?
0
```

`grep -n 'dcache-flush-fail\|EFS_ERR_BUSY'` on the page:
`| \`dcache-flush-fail\` | \`src/client/write.c\` | \`efs_dcache_flush_ino\` | \`client/write\` |`
and `| -13 | \`EFS_ERR_BUSY\` |  |`. The same output with `LC_ALL=C` has the same
hash. `make docs-check`, `scripts/check_doc_links.py`,
`scripts/status_layout.py` and `scripts/check_k_register.py` pass.

## Limitations

- Only macOS was run; the script uses no locale, `git` or absolute path, so
  Linux output should be byte-identical, but the merging session's rerun is
  the first proof.
- Keys exist only where the literal starts with `word-word` followed by a
  space or `:`; messages without one are only counted.
- Most error codes have an empty Meaning: `common.h` carries a comment on
  only some lines.
