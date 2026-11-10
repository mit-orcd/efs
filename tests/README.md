# Adding a test

Find the owner first: `graphify explain <tag>` (tags: column 6 of
`tests/budgets.tsv`). Linux-only steps are marked; the C binaries do not
build on macOS.

## Which kind of test

| Kind | Copy | Build rule | Use it when |
| --- | --- | --- | --- |
| C interface test: links the real module through its header, stubs what the module takes from other files | `tests/test_open_interface.c` (rule: `grep -n test_open_interface Makefile`) | add the `.c` to `TEST_SRCS` and the binary to `TEST_BINS` (Makefile line 168 for this family), write a `tests/test_x: ...` rule, add `./tests/test_x` to the `test` recipe | one owner module with a header, no daemon, well under a second |
| Python test with private daemons | `tests/test_k96_status_slots_live.py`, helper `tests/k33_daemons.py` (`k.Cluster`, free loopback ports, never 17xxx) | none; needs `make all` and `tests/faults/efsd` | behavior that needs real `efsd`/`efs-fuse` processes |
| Case in a sequence binary | `tests/test_meta_apply.c` (`RUN(test_x);` in `main`) | none; the binary is already built | a new case for a protocol or state machine the binary already covers |

## Register it

1. `tests/budgets.tsv` row, tab separated, columns in this order: command,
   budget_s, slots, reason, target, subsystem, purpose. Budget = 1.25 times
   the slowest of three solo runs, rounded up to 5 s. Slots: 1 for CPU-bound or
   mostly waiting, 2 for a daemon test that fsyncs a moderate set, 3 for one
   that fsyncs most of its run. Target `test` or `test-long`; subsystem = the
   tag or tags, comma-separated without spaces (`client/write,client/efs_fuse.release`).
2. Put the command in the Makefile recipe of that target. New tests go into
   `make test` only if CPU-bound and well under a second; anything that
   starts a daemon, waits on a deadline or injects faults goes into
   `make test-long`. A new case in an already-listed binary needs no row.
3. A C binary also goes into the list in `tests/sanitizer_check.sh` (a
   `name|command` line, e.g. `open_interface|tests/test_open_interface`) so
   the ASan and TSan gates run it. Python daemon tests are on the same list.
4. A budget never rises to make a test pass: fix the cause.

## Run it

```bash
python3 scripts/tests_for.py client/efs_fuse.open
python3 scripts/tests_for.py client/efs_fuse.open --run
python3 tests/test_make_test_recipe.py
python3 scripts/check_guide_counts.py
```

`tests_for.py` prints a tag's commands (exit 0), exit 1 for a declared
function prefix with no rows, exit 2 for an unknown name; `--run` runs them
after `make all` (Linux; not run here). `test_make_test_recipe.py` fails on
a recipe command without a row, a wrong target or an empty subsystem.
`check_guide_counts.py` fails when a component guide's test counts no longer
match the catalogue.

Sequence binaries (`test_meta_apply`, `test_raft`, `test_sim`, `test_kv_lsm`,
`test_session`, `test_txn`, `test_raft_store`) take a case filter: with no
argument every case runs; otherwise a case runs when an argument is a
substring of its name; an argument that matches nothing exits 2.
`./tests/test_meta_apply --list` prints the case names. The three interface
tests may not have the filter yet; try `--list` first. Linux; not run here.

```bash
make test
```

Linux only (not run here). The gate is `make test-all` before hand-off.
