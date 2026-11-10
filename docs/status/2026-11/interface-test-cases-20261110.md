# Named cases and a case filter in the interface tests (Task T7)

- **Source commit:** `7e6902d12` on `claude/competent-hermann-01b16e` (on devel `862a78945`).
- **Verdict:** PASS. Test and documentation change only.
- **Remaining failures:** none.
- **Active run:** none (node9900 `/tmp/efs-taskT7*` removed).
- **Next action:** merge by the coordinator.
- **Merge state:** `claude/competent-hermann-01b16e`, unmerged.

## Purpose

`tests/test_wb_interface.c`, `tests/test_open_interface.c` and
`tests/test_find_res_interface.c` ran every check inline in `main`, so one
case could not be named or run. Task T7 of
`structure-review-tasks-20261110.md` gives them the argument handling of
`tests/test_session.c`: no argument runs every case in the old order, an
argument runs the cases whose name contains it, `--list` prints the names,
an argument that matches nothing exits 2 and prints to stderr.

## What changed

Check groups moved unchanged into `static void test_<what>(void)` functions
(only the declarations a split local needed were added: `leased`,
`publishing`, and `t`, `w` in `test_drain_across_stop`). The run block
(`run_init`, `run_case`, `RUN`, `run_unmatched`) is copied from
`test_session.c`. The case-map tables in `src/client/efs_fuse/README.md`
name the functions and `./tests/<binary> <case>`; `docs/how-it-works/testing.md`
has one added sentence. `grep -c 'assert(' <file>` is 49, 157, 50 before and after.

| Binary | Cases | Checks (assert lines) |
|---|---|---|
| `test_wb_interface` | `test_unstarted`, `test_start_stop_restart`, `test_sticky_error`, `test_deadline_drains`, `test_drain_across_stop`, `test_failed_start`, `test_drains_race_stop`, `test_memo_balance` (8) | 49 |
| `test_open_interface` | `test_no_row`, `test_two_opens`, `test_retained_close`, `test_append_burst`, `test_retained_rows`, 13 existing functions (`open_lease_*`, `open_busy_*`, `open_retry_lock_budget`, `fuse_lease_retention_*`; `retry_order(0)` and `(1)` are `test_fuse_lease_retention_retry_order`), `test_concurrent_churn` (19) | 157 |
| `test_find_res_interface` | `serve_own_result`, `errors_never_serve_cached_bytes`, `other_uid_gets_its_own`, `many_terms_through_sixteen_slots`, `byte_budget`, `third_fetch_is_busy`, `same_term_pairs`, `concurrent_callers` (8) | 50 |

## Environment and commands

node9900 (ORCD, `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`), tree from
`git archive` of the branch (tar sha256 `845ba4bf3787456419a9bd1fe479442be3be1b8dc0530c5fde31047a7077f8fc`
for the first commit; the later fix of `test_wb_interface.c`, sha256 prefix
`5311122c37cb`, was shipped as that single file and compared), under
`/tmp/efs-taskT7`, `TMPDIR=/tmp/efs-taskT7-tmp`. Reproduce in a checkout:

```text
make tests/test_wb_interface tests/test_open_interface tests/test_find_res_interface
for b in wb open find_res; do ./tests/test_${b}_interface; echo rc=$?;
  ./tests/test_${b}_interface --list;
  ./tests/test_${b}_interface no_such_case; echo rc=$?;
  for c in $(./tests/test_${b}_interface --list); do ./tests/test_${b}_interface $c || echo FAIL $c; done; done
make test
EFS_SAN_ONLY="wb_interface open_interface find_res_interface" sh tests/sanitizer_check.sh asan   # and tsan
python3 scripts/check_guide_counts.py
```

## Results

- Build: rc 0, no warnings (`-Wall -Wextra`). The first build failed on a
  missing `t`/`w` declaration in `test_drain_across_stop`; fixed in the second commit.
- Bare run rc 0 for all three, with the original PASS line. `--list` prints the names above.
  `no_such_case` exits 2 with `no case matches 'no_such_case' (see --list)`.
  Every case alone: rc 0 (8, 19, 8 cases).
- `python3 scripts/check_guide_counts.py`: "13 guides match the catalogue".
- `make test` rc 0 (ends `verification matrix: PASS`).
- ASan and TSan lanes, `PASS` for `wb_interface`, `open_interface`, `find_res_interface` in both.
- Raw output: node9900 `~/efs-artifacts/taskT7-20261110/` with `SHA256SUMS`
  (not committed).

## Gate waits

Before `make test` and the ASan lane my script waited for
`pgrep -u $(id -u) -af make_test_summary` to come back empty: about 2700 s
and 3600 s (loop count times 20 s); the TSan lane waited 0 s. The
coordinator later judged the pattern self-matching; when I looked, the
earlier hits were real `scripts/make_test_summary.sh` processes (pids
2201299, 2357206), so I could not tell a self-match from a real gate. After
the correction, `ps | grep 'make_test_summar[y]'` printed nothing and the
runs had finished. The wait cost roughly 1 hour 45 minutes.

## Limitations

The case-map rows keep each row's full case list in the command, so a row
with several cases runs all of them. Cases keep the old order and share
process state, but each was run alone in a fresh process and passes.
