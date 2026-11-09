# A case filter on the sequence test binaries (Task EG)

Task EG, Nov 9 2026, fstor007, devel `df7d11371` (wave 64). Purpose: the
Nov 9 outside review asked for focused test-case selection inside the large
sequence test binaries, so that an agent validating one contract need not
read hundreds of unrelated test lines; Task EE
(`docs/status/2026-11/review-feedback-nov9-20261109.md`, branch
`claude/task-ee-review-nov9-b56b0d`, unmerged) agreed with a change: an argv
case filter on the binaries themselves, no new harness, no change to
`scripts/tests_for.py` or the catalogue.

**Verdict: PASS, test change only, no K row, blocks alpha: no.** All three
checks pass on all seven binaries, every bare run passes unchanged, fstor007
`make test` and `tests_for.py meta/meta_apply --run` return 0 (see Results).
Two figures in the brief were off: `test_meta_apply` has 94 cases, not 95,
and `publish` is a substring of three case names, not two (below).

## The change

Commit `bee0a54de`, `main` only in `tests/test_meta_apply.c`,
`tests/test_raft.c`, `tests/test_sim.c`, `tests/test_kv_lsm.c`,
`tests/test_session.c`, `tests/test_txn.c` and `tests/test_raft_store.c`.
None of the seven read argv before (`main(void)` in each). Each file gets
the same small block in front of `main` (no shared header, no new file):
`run_init(argc, argv)`, `run_case(name)` and
`#define RUN(fn) do { if (run_case(#fn)) fn(); } while (0)`; each case line
becomes `RUN(test_x);` in the original order, and the case bodies are
untouched. Rules:

- No argument: every case runs, as before.
- Otherwise a case runs when some argument is a substring of its name.
- `--list` alone prints every case name, one per line, and exits 0.
- An argument that matches no case is named on stderr and the binary exits 2
  (after any matched cases ran: the names are only known as the sequence
  passes; a usage error wins over test failures). `--list` with other
  arguments is treated as a filter and so exits 2.

Three mains needed more than the macro: `test_raft` runs its sequence twice
over `ack_window_fixture_mode` (the loop runs once in list mode, so each name
prints once); `test_raft_store`'s `test_recv_batch_crash` is one case called
six times with arguments (`if(run_case("test_recv_batch_crash"))` guards the
loop); `test_kv_lsm`'s three `EFS_TEST_KV_*_ONLY` environment subsets keep
their direct calls (every name in them is also in the main sequence), and
the list and unmatched checks sit after its `report:` label.

## Results

fstor007 (96 cpus), tree `git archive` of `bee0a54de` (sha256
`e6772769bb45820fd069e361658d4421da5ccb8da7b7834233636be8e5feb774`),
`make -j96 all`, gcc with the Makefile's `-O3 -march=native` flags,
libfuse through the `/tmp/efs-taskEG-fuse` shim. Wall from `date +%s.%N`
around each run (`checks.sh` below).

| Binary | `--list` (rc 0, no duplicates) | Filter (cases selected, rc, wall) | `publsh` | Bare run (rc, wall) |
| --- | ---: | --- | --- | --- |
| `test_meta_apply` | 94 | `test_publish`: 2, rc 0, 0.00 s; `publish`: 3, rc 0, 0.00 s | rc 2 | rc 0, 1.02 s |
| `test_raft` | 50 | `readindex`: 3, rc 0, 0.00 s | rc 2 | rc 0, 0.05 s; with `EFS_TEST_EARLY_READ_ACK=1` rc 0, 0.05 s |
| `test_sim` | 47 | `publish`: 1, rc 0, 0.01 s | rc 2 | rc 0, 0.04 s |
| `test_kv_lsm` | 31 | `snapshot`: 3, rc 0, 0.12 s | rc 2 | rc 0, 1.89 s |
| `test_session` | 28 | `fence`: 6, rc 0, 0.00 s | rc 2 | rc 0, 0.07 s |
| `test_txn` | 23 | `prepare`: 5, rc 0, 0.00 s | rc 2 | rc 0, 0.02 s |
| `test_raft_store` | 17 | `rotation`: 2, rc 0, 0.07 s | rc 2 | rc 0, 0.64 s |

"Cases selected" is `--list | grep -c ARG`, the same substring rule. Each
`publsh` run prints `tests/test_X: no case matches 'publsh' (see --list)`
and runs nothing. Also checked: `test_meta_apply publish publsh` runs the
three `publish` cases, then exits 2; `test_meta_apply --list extra` exits 2
naming both arguments.

`./tests/test_meta_apply publish` runs `test_publish`,
`test_publish_stale_then_retry` and `test_degraded_mark_after_publish` (the
brief expected two: the substring rule was kept, not narrowed to fit);
`./tests/test_meta_apply test_publish` runs the two:

```
$ ./tests/test_meta_apply test_publish      # rc=0 wall=0.00s
test_meta_apply: OK
$ ./tests/test_meta_apply                   # rc=0 wall=1.02s
test_meta_apply: OK
```

Reading set for a `publish` reader (function first line to closing brace,
`awk` on the devel file; line numbers unchanged by the commit, which only
touches `main`): `test_publish` 42 lines (751 to 792) plus
`test_publish_stale_then_retry` 35 lines (841 to 875), 77 of the file's
7,343 lines on devel (1.0 %); with `test_degraded_mark_after_publish`
(85 lines, 5768 to 5852) the `publish` selection is 162 lines (2.2 %). Shared
fixtures and helpers the cases call are not counted. The file is now 7,393
lines (the 46-line filter block and `main`'s two exit lines).

Gate on fstor007, same tree:

| Command | rc | Wall |
| --- | ---: | ---: |
| `make -j96 test` | 0 | 135 s |
| `make -j96` the `TEST_SPECIAL_BINS` of `make test-long` | 0 | 0 s (already built by `make test`) |
| `python3 scripts/tests_for.py meta/meta_apply --run` (after `make all` and the special bins) | 0 (12 commands) | 4 s |

`test_meta_apply` is on no `tests/sanitizer_check.sh` list, so no sanitizer
run is owed.

## Reproduction

On a Linux host with the build prerequisites, from a checkout of
`bee0a54de` or later:

```
make -j"$(nproc)" all
make tests/test_meta_apply tests/test_raft tests/test_sim tests/test_kv_lsm \
     tests/test_session tests/test_txn tests/test_raft_store
for b in meta_apply raft sim kv_lsm session txn raft_store; do
  ./tests/test_$b --list | wc -l          # 94 50 47 31 28 23 17
  ./tests/test_$b publsh; echo "rc=$?"    # rc=2
  ./tests/test_$b; echo "rc=$?"           # OK, rc=0
done
./tests/test_meta_apply test_publish      # OK, the two publish cases
make test
make $(make -s -p -n | sed -n 's/^TEST_SPECIAL_BINS = //p' | head -1)
python3 scripts/tests_for.py meta/meta_apply --run
```

On fstor007 the build ran under `/tmp/efs-taskEG` with
`TMPDIR=/tmp/efs-taskEG-tmp` and the libfuse shim (`EFS_FUSE_PREFIX`,
`PKG_CONFIG_PATH=<shim>/lib64/pkgconfig`, `LD_LIBRARY_PATH=<shim>/lib64`),
the gate in a `screen` session; both trees and the uploaded tar were removed
afterwards.

## Raw output

fstor007 `~/efs-artifacts/taskEG-20261109/` with `SHA256SUMS`
(sha256 of `SHA256SUMS`: `642b419e4950a6094a8cf6ee0b0b28e252deedd00f17eb8ab03058d4a13d18d8`): `checks.txt` (the table's runs),
`filter-*.txt`, `misspell-*.txt`, `bare-*.txt`, `gate.txt`,
`make-all.log`, `make-bins.log`, `make-test.log`, `make-special.log`,
`tests-for.log`, and the scripts `checks.sh`, `gate.sh`, `env.sh`.

## Limitations and not done

- Unmatched arguments are found only after the sequence has passed, so
  `ARG publsh` runs the `ARG` cases before exiting 2.
- The filter matches function names, not contracts; `publish` also selects
  `test_degraded_mark_after_publish`. Use the full `test_` name to narrow.
- The catalogue rows, the Makefile and `scripts/tests_for.py` are unchanged:
  `tests_for.py` still prints and runs whole binaries.
- Other multi-case test binaries (and the single-contract interface tests
  `test_open_interface`, `test_wb_interface`, `test_find_res_interface`) are
  unchanged.
- Not run on node9900; the wave gate that merges this branch records
  `make test-all` on both hosts.
